#include "containerd.hpp"

#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"
#include "data_store/value.hpp"

#include "crun_runtime.hpp"
#include "http_client.hpp"     // mkdir_p
#include "image_ref.hpp"
#include "layer_store.hpp"
#include "oci_bundle.hpp"
#include "registry.hpp"
#include "registry_puller.hpp"

namespace containers {

namespace {
// v1 is single-container; a fixed id keeps the run/overlay paths stable.
constexpr const char* kContainerId = "c0";

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool spit(const std::string& path, const std::string& body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
    return f.good();
}
} // namespace

Containerd::~Containerd() {
    m_pull_cancel.store(true);
    m_shutdown.store(true);        // break the supervise loop fast (kills the container)
    m_stop_requested.store(true);
    if (m_pull_thread.joinable()) m_pull_thread.join();
    if (m_run_thread.joinable())  m_run_thread.join();
}

// Captures this daemon's ACE log output to log.containerd.text and applies the
// per-daemon level from log.level.container (falls back to log.level).
static data_store::LogBuffer g_log("containerd", "log.containerd.text", "log.level.container");

std::string Containerd::get_str(const std::string& key) {
    std::vector<data_store::Client::GetResult> got;
    if (m_ds.get({key}, got).ok && !got.empty() && got[0].has_value)
        if (auto s = data_store::to_string(got[0].value)) return *s;
    return {};
}

void Containerd::set_state(const char* state) {
    m_ds.set_volatile(std::string("container.state"), data_store::Value{std::string(state)});
}

void Containerd::set_error(const std::string& msg) {
    m_ds.set_volatile(std::string("container.error"), data_store::Value{msg});
}

void Containerd::publish_initial_status() {
    std::vector<data_store::KV> kv;
    kv.emplace_back("container.state",         data_store::Value{std::string("idle")});
    kv.emplace_back("container.pull.progress", data_store::Value{std::string("0")});
    kv.emplace_back("container.pull.detail",   data_store::Value{std::string("")});
    kv.emplace_back("container.status",        data_store::Value{std::string("idle")});
    kv.emplace_back("container.error",         data_store::Value{std::string("")});
    m_ds.set_volatile(kv);
}

void Containerd::on_command_event(const data_store::Client::Event& ev) {
    const std::string val = data_store::to_string(ev.value).value_or(std::string());
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (ev.key == "container.pull.request") {
            if (val == m_pull_token) return;     // unchanged → ignore re-notify
            m_pull_token = val; m_pull_pending = true;
        } else if (ev.key == "container.run.request") {
            if (val == m_run_token) return;
            m_run_token = val; m_run_pending = true;
        } else if (ev.key == "container.stop.request") {
            if (val == m_stop_token) return;
            m_stop_token = val; m_stop_pending = true;
        } else {
            return;
        }
    }
    // Hand off to the reactor thread; handle_exception() does the work.
    ACE_Reactor::instance()->notify(this);
}

int Containerd::handle_exception(ACE_HANDLE) {
    bool pull, run, stop;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        pull = m_pull_pending; m_pull_pending = false;
        run  = m_run_pending;  m_run_pending  = false;
        stop = m_stop_pending; m_stop_pending = false;
    }
    if (stop) handle_stop();    // stop first so a stop+run pair restarts cleanly
    if (pull) handle_pull();
    if (run)  handle_run();
    return 0;
}

void Containerd::handle_pull() {
    if (m_pull_active.load()) {
        set_error("a pull is already in progress");
        return;
    }
    const std::string ref_s = get_str("container.image.ref");
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] pull requested: ref=%C\n"),
               ref_s.empty() ? "(unset)" : ref_s.c_str()));
    if (ref_s.empty()) {
        set_error("container.image.ref is empty — set an image to pull");
        set_state("error");
        return;
    }
    ImageRef ref;
    if (!parse_image_ref(ref_s, ref)) {
        set_error("could not parse image ref: " + ref_s);
        set_state("error");
        return;
    }
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ctr]   -> registry=%C repo=%C tag=%C digest=%C\n"),
        ref.registry.c_str(), ref.repository.c_str(),
        ref.tag.c_str(), ref.digest.c_str()));

    const std::string user = get_str("container.registry.user");
    const std::string pass = get_str("container.registry.pass");
    // Consume the secret: do not leave the registry password persisted in ds
    // (it would otherwise sit on the SD card + in any ds snapshot). Cleared
    // persistently; the operator re-enters it per pull (write-only by design).
    if (!pass.empty())
        m_ds.set(std::string("container.registry.pass"), data_store::Value{std::string()});

    // Reap a previously-finished worker before launching a new one.
    if (m_pull_thread.joinable()) m_pull_thread.join();
    m_pull_cancel.store(false);
    m_pull_active.store(true);

    set_error("");
    set_state("pulling");
    m_ds.set_volatile(std::string("container.pull.progress"),
                      data_store::Value{std::string("0")});

    const std::string root = m_cfg.root;
    m_pull_thread = std::thread([this, ref, user, pass, root]() {
        auto progress = [this](int pct, const std::string& detail) {
            std::vector<data_store::KV> kv;
            kv.emplace_back("container.pull.progress", data_store::Value{std::to_string(pct)});
            kv.emplace_back("container.pull.detail",   data_store::Value{detail});
            m_ds.set_volatile(kv);
        };
        PullResult r = pull_image(ref, user, pass, root, progress, m_pull_cancel);
        if (r.ok) {
            std::vector<data_store::KV> kv;
            kv.emplace_back("container.image.id",      data_store::Value{r.image_id});
            kv.emplace_back("container.image.size",    data_store::Value{std::to_string(r.total_size)});
            kv.emplace_back("container.pull.progress", data_store::Value{std::string("100")});
            kv.emplace_back("container.pull.detail",   data_store::Value{std::string("pull complete")});
            kv.emplace_back("container.error",         data_store::Value{std::string()});
            kv.emplace_back("container.state",         data_store::Value{std::string("pulled")});
            m_ds.set_volatile(kv);
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] pull ok: %C (%lld bytes)\n"),
                       r.image_id.c_str(), static_cast<long long>(r.total_size)));
        } else {
            set_error(r.error);
            set_state("error");
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] pull failed: %C\n"), r.error.c_str()));
        }
        m_pull_active.store(false);
    });
}

void Containerd::handle_run() {
    if (m_run_active.load()) { set_error("a run/stop is already in progress"); return; }

    const std::string image_id = get_str("container.image.id");
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] run requested (image=%C)\n"),
               image_id.empty() ? "(none)" : image_id.c_str()));
    if (image_id.empty() || !is_valid_digest(image_id)) {
        set_error("no image pulled yet — pull an image first");
        set_state("error");
        return;
    }

    // Process overrides + resource limits (read on the reactor thread).
    const std::string ov_entrypoint = get_str("container.entrypoint");
    const std::string ov_cmd        = get_str("container.cmd");
    const std::string lim_mem       = get_str("container.limit.mem");
    const std::string lim_cpus      = get_str("container.limit.cpus");

    if (m_run_thread.joinable()) m_run_thread.join();
    m_stop_requested.store(false);
    m_run_active.store(true);
    set_error("");
    set_state("mounting");
    m_ds.set_volatile(std::string("container.pull.detail"),
                      data_store::Value{std::string("extracting layers")});

    const std::string root     = m_cfg.root;
    const std::string run_root = m_cfg.run;
    m_run_thread = std::thread([this, image_id, root, run_root,
                                ov_entrypoint, ov_cmd, lim_mem, lim_cpus]() {
        const std::string hex  = image_id.substr(7);
        const std::string body = slurp(root + "/manifests/" + hex + ".json");
        ImageManifest im = parse_image_manifest(body);
        if (!im.ok) {
            set_error("image manifest missing/corrupt — re-pull the image");
            set_state("error");
            m_run_active.store(false);
            return;
        }

        std::vector<std::string> digests;
        for (const auto& l : im.layers) digests.push_back(l.digest);

        std::string err;
        std::vector<std::string> layer_dirs;
        if (!ensure_layers_extracted(root, digests, layer_dirs, err)) {
            set_error("layer extraction failed: " + err);
            set_state("error");
            m_run_active.store(false);
            return;
        }

        MountResult mr = mount_overlay(layer_dirs, run_root, kContainerId);
        if (!mr.ok) {
            set_error("overlay mount failed: " + mr.error);
            set_state("error");
            m_run_active.store(false);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_merged = mr.merged;
        }
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] overlay mounted: %C (%u layers)\n"),
                   mr.merged.c_str(), static_cast<unsigned>(layer_dirs.size())));

        // ── Build the OCI bundle (config.json next to the rootfs) ──────────
        const std::string bundle = run_root + "/" + kContainerId;   // mr.merged == bundle/rootfs
        ImageConfig ic = parse_image_config(slurp(blob_path(root, im.config.digest)));

        std::vector<std::string> args = resolve_process_args(
            ic.entrypoint, ic.cmd,
            parse_args_field(ov_entrypoint), !ov_entrypoint.empty(),
            parse_args_field(ov_cmd),        !ov_cmd.empty());
        if (args.empty()) {
            set_error("image has no Entrypoint/Cmd — set a CMD/Entrypoint to run");
            set_state("error");
            unmount_overlay(run_root, kContainerId, err);
            m_run_active.store(false);
            return;
        }

        OciSpec spec;
        spec.args = args;
        spec.env  = ic.env;
        spec.cwd  = ic.working_dir.empty() ? "/" : ic.working_dir;
        resolve_user(ic.user, spec.uid, spec.gid);
        spec.mem_limit_bytes = parse_mem_limit(lim_mem);
        spec.cpu_quota       = parse_cpu_quota(lim_cpus, spec.cpu_period);

        if (!spit(bundle + "/config.json", generate_oci_config(spec))) {
            set_error("could not write OCI bundle config.json");
            set_state("error");
            unmount_overlay(run_root, kContainerId, err);
            m_run_active.store(false);
            return;
        }

        // ── crun create + start ────────────────────────────────────────────
        const std::string state_root = run_root + "/state";
        mkdir_p(state_root);
        CrunRuntime crun(state_root);
        crun.remove(kContainerId, true, err);   // clear any stale container

        set_state("created");
        if (!crun.create(kContainerId, bundle, err)) {
            set_error("crun create failed: " + err);
            set_state("error");
            unmount_overlay(run_root, kContainerId, err);
            m_run_active.store(false);
            return;
        }
        if (!crun.start(kContainerId, err)) {
            set_error("crun start failed: " + err);
            set_state("error");
            crun.remove(kContainerId, true, err);
            unmount_overlay(run_root, kContainerId, err);
            m_run_active.store(false);
            return;
        }

        // Grab the pid + announce running.
        long pid = crun.state(kContainerId).pid;
        std::vector<data_store::KV> kv;
        kv.emplace_back("container.run.pid",     data_store::Value{std::to_string(pid)});
        kv.emplace_back("container.run.started", data_store::Value{std::to_string(
                                                     static_cast<long long>(std::time(nullptr)))});
        kv.emplace_back("container.exit.code",   data_store::Value{std::string()});
        kv.emplace_back("container.status",      data_store::Value{std::string("running")});
        kv.emplace_back("container.error",       data_store::Value{std::string()});
        kv.emplace_back("container.state",       data_store::Value{std::string("running")});
        m_ds.set_volatile(kv);
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] container started: pid=%d\n"),
                   static_cast<int>(pid)));

        // ── Supervise: poll until the container exits, honoring stop ───────
        bool term_sent = false;
        int  grace_ticks = 0;
        while (true) {
            // ~2s poll, but wake every 250ms so daemon shutdown is responsive.
            bool shutting = false;
            for (int i = 0; i < 8; ++i) {
                ACE_OS::sleep(ACE_Time_Value(0, 250000));
                if (m_shutdown.load()) { shutting = true; break; }
            }
            if (shutting) { crun.kill(kContainerId, "KILL", err); break; }

            CrunStatus st = crun.state(kContainerId);
            if (!st.exists || st.status == "stopped") break;   // exited
            if (m_stop_requested.load()) {
                if (!term_sent) {
                    crun.kill(kContainerId, "TERM", err);
                    term_sent = true;
                    grace_ticks = 0;
                    m_ds.set_volatile(std::string("container.status"),
                                      data_store::Value{std::string("stopping")});
                } else if (++grace_ticks >= 5) {               // ~10s grace → SIGKILL
                    crun.kill(kContainerId, "KILL", err);
                }
            }
        }

        // ── Exited: clean up + report ──────────────────────────────────────
        crun.remove(kContainerId, true, err);
        unmount_overlay(run_root, kContainerId, err);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_merged.clear();
        }
        std::vector<data_store::KV> done;
        done.emplace_back("container.run.pid", data_store::Value{std::string()});
        done.emplace_back("container.status",  data_store::Value{std::string("stopped")});
        done.emplace_back("container.state",   data_store::Value{std::string("stopped")});
        m_ds.set_volatile(done);
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] container stopped%C\n"),
                   m_stop_requested.load() ? " (by request)" : " (exited)"));
        m_run_active.store(false);
    });
}

void Containerd::handle_stop() {
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] stop requested\n")));
    if (m_run_active.load()) {
        // The run worker owns the container + overlay; ask it to tear down
        // (SIGTERM → grace → SIGKILL → delete + unmount). All crun calls stay
        // on the worker thread to avoid racing on the crun state dir.
        m_stop_requested.store(true);
        m_ds.set_volatile(std::string("container.status"),
                          data_store::Value{std::string("stopping")});
        return;
    }
    // Nothing running: clear any stale crun state + overlay mount.
    std::string err;
    CrunRuntime crun(m_cfg.run + "/state");
    crun.remove(kContainerId, true, err);
    unmount_overlay(m_cfg.run, kContainerId, err);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_merged.clear();
    }
    m_ds.set_volatile(std::string("container.status"), data_store::Value{std::string("stopped")});
    set_error("");
    set_state("stopped");
}

int Containerd::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok)
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [ctr] ds connect failed\n")), 1);

    // Baseline the request tokens BEFORE watching so a stale value (e.g. a
    // previous session's request) does not fire a spurious command on boot.
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pull_token = get_str("container.pull.request");
        m_run_token  = get_str("container.run.request");
        m_stop_token = get_str("container.stop.request");
    }

    publish_initial_status();

    data_store::Client::WatchHandle h = data_store::Client::kInvalidHandle;
    if (!m_ds.watch(
            std::vector<std::string>{"container.pull.request",
                                     "container.run.request",
                                     "container.stop.request"},
            [this](const data_store::Client::Event& ev) { on_command_event(ev); },
            &h).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [ctr] command watch failed\n")), 2);
    }

    // ACE is initialised now (reactor used above): capture logs to
    // log.containerd.text, apply log.level.container, drive the flush timer.
    g_log.start();
    g_log.apply_level(m_ds);
    g_log.open(m_ds, 5, 1);
    // Self-report for the Services page.
    m_ds.set(std::string("services.container.state"), data_store::Value{std::string("running")});

    // L22 resource telemetry → services.container.{cpu,mem,fd,threads}. We pump
    // the singleton reactor in-thread, so schedule on it (no extra thread).
    data_store::StatsPublisher stats("services.container",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ctr] up: root=%C run=%C (pull/run/stop ready)\n"),
        m_cfg.root.c_str(), m_cfg.run.c_str()));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace containers
