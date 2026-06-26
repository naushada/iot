#include "containerd.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"
#include "data_store/value.hpp"

#include "container_net.hpp"   // kDefaultSubnet
#include "crun_runtime.hpp"
#include "http_client.hpp"     // mkdir_p
#include "image_ref.hpp"
#include "layer_store.hpp"
#include "net_bridge.hpp"
#include "oci_bundle.hpp"
#include "registry.hpp"
#include "registry_puller.hpp"

namespace containers {

namespace {
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

// crun id / filesystem path component from an operator name: lower-case,
// keep [a-z0-9-], everything else → '-'. Prefixed "c-" so it never collides
// with crun internals and is a stable run/overlay dir name.
std::string sanitize_name(const std::string& n) {
    std::string s;
    for (char c : n) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') s += c;
        else if (c >= 'A' && c <= 'Z') s += static_cast<char>(c - 'A' + 'a');
        else s += '-';
    }
    if (s.empty()) s = "x";
    return s;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned char>(c));
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}
} // namespace

Containerd::~Containerd() {
    m_pull_cancel.store(true);
    m_shutdown.store(true);                // break every supervise loop fast (SIGKILL)
    if (m_pull_thread.joinable()) m_pull_thread.join();

    // Signal + collect the run threads under the lock, then join WITHOUT it (the
    // workers grab m_mtx to publish their final state — joining while holding it
    // would deadlock).
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& kv : m_containers) {
            kv.second->stop_requested.store(true);
            if (kv.second->run_thread.joinable())
                threads.push_back(std::move(kv.second->run_thread));
        }
    }
    for (auto& t : threads) if (t.joinable()) t.join();
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

// ── helpers (m_mtx held) ───────────────────────────────────────────────────

ContainerInstance* Containerd::find_locked(const std::string& name) {
    auto it = m_containers.find(name);
    return it == m_containers.end() ? nullptr : it->second.get();
}

ContainerInstance* Containerd::ensure_locked(const std::string& name) {
    if (auto* p = find_locked(name)) return p;
    auto inst = std::make_unique<ContainerInstance>(name, "c-" + sanitize_name(name));
    ContainerInstance* p = inst.get();
    m_containers.emplace(name, std::move(inst));
    return p;
}

int Containerd::alloc_octet_locked() {
    for (int o = 2; o <= 254; ++o)            // .1 is the bridge gateway
        if (!m_used_octets.count(o)) { m_used_octets.insert(o); return o; }
    return 0;                                 // /24 exhausted
}

void Containerd::free_octet_locked(int octet) {
    if (octet) m_used_octets.erase(octet);
}

void Containerd::publish_instances_locked() {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& kv : m_containers) {
        const ContainerInstance* c = kv.second.get();
        if (!first) ss << ",";
        first = false;
        ss << "{"
           << "\"name\":\""    << json_escape(c->name)       << "\","
           << "\"image\":\""   << json_escape(c->image_ref)  << "\","
           << "\"imageId\":\"" << json_escape(c->image_id)   << "\","
           << "\"size\":\""    << json_escape(c->image_size) << "\","
           << "\"state\":\""   << json_escape(c->state)      << "\","
           << "\"ip\":\""      << json_escape(c->ip)         << "\","
           << "\"gateway\":\"" << json_escape(c->gateway)    << "\","
           << "\"net\":\""     << json_escape(c->net_mode)   << "\","
           << "\"mem\":\""     << json_escape(c->mem)        << "\","
           << "\"cpus\":\""    << json_escape(c->cpus)       << "\","
           << "\"pid\":"       << c->pid                     << ","
           << "\"exitCode\":"  << (c->exit_code < 0 ? std::string("null")
                                                    : std::to_string(c->exit_code)) << ","
           << "\"started\":"   << c->started                 << ","
           << "\"error\":\""   << json_escape(c->error)      << "\"}";
    }
    ss << "]";
    m_ds.set_volatile(std::string("container.instances"), data_store::Value{ss.str()});

    // Legacy single-container mirror (first instance) so an un-migrated UI still
    // shows something during rollout.
    const ContainerInstance* lead =
        m_containers.empty() ? nullptr : m_containers.begin()->second.get();
    m_ds.set_volatile(std::vector<data_store::KV>{
        {"container.state",  data_store::Value{lead ? lead->state : std::string("idle")}},
        {"container.net.ip", data_store::Value{lead ? lead->ip    : std::string()}}});
}

// ── command intake (listener thread → reactor) ─────────────────────────────

void Containerd::on_command_event(const data_store::Client::Event& ev) {
    if (ev.key != "container.cmd.request") return;
    const std::string val = data_store::to_string(ev.value).value_or(std::string());
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (val == m_cmd_token) return;      // unchanged → ignore re-notify
        m_cmd_token = val;
        m_cmd_pending = true;
    }
    ACE_Reactor::instance()->notify(this);
}

int Containerd::handle_exception(ACE_HANDLE) {
    bool cmd;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        cmd = m_cmd_pending; m_cmd_pending = false;
    }
    if (cmd) handle_command();
    return 0;
}

void Containerd::handle_command() {
    const std::string name   = get_str("container.cmd.name");
    const std::string action = get_str("container.cmd.action");
    if (name.empty() || action.empty()) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] command missing name/action "
                                      "(name=%C action=%C)\n"),
                   name.c_str(), action.c_str()));
        return;
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] cmd: action=%C name=%C\n"),
               action.c_str(), name.c_str()));

    if (action == "remove") { do_remove(name); return; }

    if (action == "stop") {
        ContainerInstance* inst;
        { std::lock_guard<std::mutex> lk(m_mtx); inst = find_locked(name); }
        if (inst) do_stop(inst);
        else ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] stop: no such container %C\n"),
                        name.c_str()));
        return;
    }

    // pull carries the container's config in the envelope and creates it if new.
    // run/stop reuse the instance's stored config — the envelope is shared
    // mutable state, so we must NOT re-read its param fields for a bare per-row
    // Run (they may hold another container's values).
    if (action == "pull") {
        const std::string image = get_str("container.cmd.image");
        const std::string net   = get_str("container.cmd.net");
        const std::string sub   = get_str("container.cmd.subnet");
        const std::string mem   = get_str("container.cmd.mem");
        const std::string cpus  = get_str("container.cmd.cpus");
        const std::string ep    = get_str("container.cmd.entrypoint");
        const std::string cm    = get_str("container.cmd.cmd");
        ContainerInstance* inst;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            inst = ensure_locked(name);
            if (!image.empty()) inst->image_ref = image;
            if (!net.empty())   inst->net_mode  = net;
            inst->net_subnet = sub.empty() ? std::string(kDefaultSubnet) : sub;
            inst->mem = mem; inst->cpus = cpus; inst->entrypoint = ep; inst->cmd = cm;
            publish_instances_locked();
        }
        do_pull(inst);
        return;
    }

    if (action == "run") {
        ContainerInstance* inst;
        { std::lock_guard<std::mutex> lk(m_mtx); inst = find_locked(name); }
        if (inst) do_run(inst);
        else ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] run: no such container %C "
                                           "(pull it first)\n"), name.c_str()));
        return;
    }

    ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] unknown action %C\n"), action.c_str()));
}

// ── pull (serialised; one worker) ──────────────────────────────────────────

void Containerd::do_pull(ContainerInstance* inst) {
    if (m_pull_active.load()) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = "a pull is already in progress"; publish_instances_locked();
        return;
    }
    std::string ref_s;
    { std::lock_guard<std::mutex> lk(m_mtx); ref_s = inst->image_ref; }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] pull %C: ref=%C\n"),
               inst->name.c_str(), ref_s.empty() ? "(unset)" : ref_s.c_str()));
    if (ref_s.empty()) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = "image ref is empty — set an image to pull"; inst->state = "error";
        publish_instances_locked();
        return;
    }
    ImageRef ref;
    if (!parse_image_ref(ref_s, ref)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = "could not parse image ref: " + ref_s; inst->state = "error";
        publish_instances_locked();
        return;
    }
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ctr]   -> registry=%C repo=%C tag=%C digest=%C\n"),
        ref.registry.c_str(), ref.repository.c_str(), ref.tag.c_str(), ref.digest.c_str()));

    const std::string user = get_str("container.registry.user");
    const std::string pass = get_str("container.registry.pass");
    // Consume the secret — never leave the registry password persisted in ds.
    if (!pass.empty())
        m_ds.set(std::string("container.registry.pass"), data_store::Value{std::string()});

    if (m_pull_thread.joinable()) m_pull_thread.join();
    m_pull_cancel.store(false);
    m_pull_active.store(true);
    const std::string name = inst->name;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pull_name = name;
        inst->error.clear(); inst->state = "pulling";
        publish_instances_locked();
    }
    m_ds.set_volatile(std::string("container.pull.progress"), data_store::Value{std::string("0")});

    const std::string root = m_cfg.root;
    m_pull_thread = std::thread([this, ref, user, pass, root, name]() {
        auto progress = [this, name](int pct, const std::string& detail) {
            std::vector<data_store::KV> kv;
            kv.emplace_back("container.pull.progress", data_store::Value{std::to_string(pct)});
            kv.emplace_back("container.pull.detail",   data_store::Value{name + ": " + detail});
            m_ds.set_volatile(kv);
        };
        PullResult r = pull_image(ref, user, pass, root, progress, m_pull_cancel);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (ContainerInstance* in = find_locked(name)) {   // re-find (still present)
                if (r.ok) {
                    in->image_id = r.image_id;
                    in->image_size = std::to_string(r.total_size);
                    in->error.clear(); in->state = "pulled";
                } else {
                    in->error = r.error; in->state = "error";
                }
            }
            publish_instances_locked();
        }
        m_ds.set_volatile(std::vector<data_store::KV>{
            {"container.pull.progress", data_store::Value{std::string(r.ok ? "100" : "0")}},
            {"container.pull.detail",   data_store::Value{name + (r.ok ? ": pull complete"
                                                                       : ": " + r.error)}}});
        m_pull_active.store(false);
        if (r.ok)
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] pull ok %C: %C (%lld bytes)\n"),
                       name.c_str(), r.image_id.c_str(),
                       static_cast<long long>(r.total_size)));
        else
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ctr] pull failed %C: %C\n"),
                       name.c_str(), r.error.c_str()));
    });
}

// ── run (per-instance supervise thread) ────────────────────────────────────

void Containerd::do_run(ContainerInstance* inst) {
    if (inst->run_active.load()) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = "container is already running"; publish_instances_locked();
        return;
    }
    std::string image_id, net_mode, net_subnet, mem, cpus, ep, cm, id;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        image_id   = inst->image_id;
        net_mode   = inst->net_mode;
        net_subnet = inst->net_subnet.empty() ? std::string(kDefaultSubnet) : inst->net_subnet;
        mem = inst->mem; cpus = inst->cpus; ep = inst->entrypoint; cm = inst->cmd;
        id = inst->id;
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] run %C (image=%C)\n"),
               inst->name.c_str(), image_id.empty() ? "(none)" : image_id.c_str()));
    if (image_id.empty() || !is_valid_digest(image_id)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = "no image pulled yet — pull an image first"; inst->state = "error";
        publish_instances_locked();
        return;
    }

    const bool bridge = (net_mode == "bridge");
    int octet = 0;
    if (bridge) {
        std::lock_guard<std::mutex> lk(m_mtx);
        octet = alloc_octet_locked();
        if (octet == 0) {
            inst->error = "no free bridge IP (subnet exhausted)"; inst->state = "error";
            publish_instances_locked();
            return;
        }
        inst->host_octet = octet;
    }

    if (inst->run_thread.joinable()) inst->run_thread.join();
    inst->stop_requested.store(false);
    inst->run_active.store(true);
    { std::lock_guard<std::mutex> lk(m_mtx); inst->error.clear(); inst->state = "mounting";
      publish_instances_locked(); }

    const std::string root = m_cfg.root, run_root = m_cfg.run;
    inst->run_thread = std::thread(
        [this, inst, image_id, root, run_root, ep, cm, mem, cpus, bridge, net_subnet, id, octet]() {
            run_worker(inst, image_id, root, run_root, ep, cm, mem, cpus,
                       bridge, net_subnet, id, octet);
        });
}

void Containerd::run_worker(ContainerInstance* inst, std::string image_id,
                            std::string root, std::string run_root,
                            std::string ov_entrypoint, std::string ov_cmd,
                            std::string lim_mem, std::string lim_cpus,
                            bool bridge, std::string net_subnet, std::string id, int octet) {
    std::string err;
    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->error = msg; inst->state = "error";
        if (inst->host_octet) { free_octet_locked(inst->host_octet); inst->host_octet = 0; }
        inst->ip.clear(); inst->gateway.clear();
        publish_instances_locked();
    };

    const std::string hex  = image_id.substr(7);
    ImageManifest im = parse_image_manifest(slurp(root + "/manifests/" + hex + ".json"));
    if (!im.ok) { fail("image manifest missing/corrupt — re-pull the image");
                  inst->run_active.store(false); return; }

    std::vector<std::string> digests;
    for (const auto& l : im.layers) digests.push_back(l.digest);
    std::vector<std::string> layer_dirs;
    if (!ensure_layers_extracted(root, digests, layer_dirs, err)) {
        fail("layer extraction failed: " + err); inst->run_active.store(false); return;
    }

    MountResult mr = mount_overlay(layer_dirs, run_root, id);
    if (!mr.ok) { fail("overlay mount failed: " + mr.error);
                  inst->run_active.store(false); return; }
    { std::lock_guard<std::mutex> lk(m_mtx); inst->merged = mr.merged; }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] %C: overlay mounted: %C (%u layers)\n"),
               id.c_str(), mr.merged.c_str(), static_cast<unsigned>(layer_dirs.size())));

    const std::string bundle = run_root + "/" + id;   // mr.merged == bundle/rootfs
    ImageConfig ic = parse_image_config(slurp(blob_path(root, im.config.digest)));
    std::vector<std::string> args = resolve_process_args(
        ic.entrypoint, ic.cmd,
        parse_args_field(ov_entrypoint), !ov_entrypoint.empty(),
        parse_args_field(ov_cmd),        !ov_cmd.empty());
    if (args.empty()) {
        unmount_overlay(run_root, id, err);
        fail("image has no Entrypoint/Cmd — set a CMD/Entrypoint to run");
        inst->run_active.store(false); return;
    }

    OciSpec spec;
    spec.args = args; spec.env = ic.env;
    spec.cwd  = ic.working_dir.empty() ? "/" : ic.working_dir;
    resolve_user(ic.user, spec.uid, spec.gid);
    spec.mem_limit_bytes = parse_mem_limit(lim_mem);
    spec.cpu_quota       = parse_cpu_quota(lim_cpus, spec.cpu_period);
    spec.host_network    = !bridge;   // bridge → own netns (IP wired below)

    if (!spit(bundle + "/config.json", generate_oci_config(spec))) {
        unmount_overlay(run_root, id, err);
        fail("could not write OCI bundle config.json");
        inst->run_active.store(false); return;
    }

    const std::string state_root = run_root + "/state";
    mkdir_p(state_root);
    CrunRuntime crun(state_root);
    crun.remove(id, true, err);   // clear any stale container
    { std::lock_guard<std::mutex> lk(m_mtx); inst->state = "created"; publish_instances_locked(); }
    if (!crun.create(id, bundle, err)) {
        unmount_overlay(run_root, id, err);
        fail("crun create failed: " + err);
        inst->run_active.store(false); return;
    }
    const long pid = crun.state(id).pid;

    if (bridge) {
        BridgeNet bn = bridge_up(pid, net_subnet, id, octet);
        if (!bn.ok) {
            crun.remove(id, true, err); bridge_down(id); unmount_overlay(run_root, id, err);
            fail("bridge networking failed: " + bn.error);
            inst->run_active.store(false); return;
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->ip = bn.ip; inst->gateway = bn.gateway; publish_instances_locked();
    }

    if (!crun.start(id, err)) {
        crun.remove(id, true, err); if (bridge) bridge_down(id); unmount_overlay(run_root, id, err);
        fail("crun start failed: " + err);
        inst->run_active.store(false); return;
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        inst->pid = pid;
        inst->started = static_cast<long long>(std::time(nullptr));
        inst->exit_code = -1; inst->error.clear(); inst->state = "running";
        publish_instances_locked();
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] %C: started pid=%d\n"),
               id.c_str(), static_cast<int>(pid)));

    // ── Supervise: poll until exit, honoring stop ──────────────────────────
    bool term_sent = false;
    int  grace_ticks = 0;
    while (true) {
        bool shutting = false;
        for (int i = 0; i < 8; ++i) {        // ~2s poll, 250ms wakeups for fast teardown
            ACE_OS::sleep(ACE_Time_Value(0, 250000));
            if (m_shutdown.load()) { shutting = true; break; }
        }
        if (shutting) { crun.kill(id, "KILL", err); break; }

        CrunStatus st = crun.state(id);
        if (!st.exists || st.status == "stopped") break;   // exited
        if (inst->stop_requested.load()) {
            if (!term_sent) {
                crun.kill(id, "TERM", err); term_sent = true; grace_ticks = 0;
            } else if (++grace_ticks >= 5) {               // ~10s grace → SIGKILL
                crun.kill(id, "KILL", err);
            }
        }
    }

    // ── Exited: clean up + report ──────────────────────────────────────────
    crun.remove(id, true, err);
    if (bridge) bridge_down(id);
    unmount_overlay(run_root, id, err);
    const bool by_req = inst->stop_requested.load();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (inst->host_octet) { free_octet_locked(inst->host_octet); inst->host_octet = 0; }
        inst->merged.clear(); inst->ip.clear(); inst->gateway.clear();
        inst->pid = 0; inst->state = "stopped";
        publish_instances_locked();
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] %C: stopped%C\n"),
               id.c_str(), by_req ? " (by request)" : " (exited)"));
    inst->run_active.store(false);
}

void Containerd::do_stop(ContainerInstance* inst) {
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] stop %C\n"), inst->name.c_str()));
    if (inst->run_active.load()) {
        // The run worker owns the container + overlay; ask it to tear down
        // (SIGTERM → grace → SIGKILL → delete + unmount). All crun calls stay on
        // that thread to avoid racing on the crun state dir.
        inst->stop_requested.store(true);
        return;
    }
    // Nothing running: best-effort clear any stale crun state + overlay + veth.
    std::string err;
    CrunRuntime crun(m_cfg.run + "/state");
    crun.remove(inst->id, true, err);
    bridge_down(inst->id);
    unmount_overlay(m_cfg.run, inst->id, err);
    std::lock_guard<std::mutex> lk(m_mtx);
    if (inst->host_octet) { free_octet_locked(inst->host_octet); inst->host_octet = 0; }
    inst->ip.clear(); inst->gateway.clear(); inst->error.clear();
    inst->state = "stopped";
    publish_instances_locked();
}

void Containerd::do_remove(const std::string& name) {
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] remove %C\n"), name.c_str()));
    std::unique_ptr<ContainerInstance> doomed;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_containers.find(name);
        if (it == m_containers.end()) return;
        ContainerInstance* inst = it->second.get();
        // Busy (running, or this instance is the in-flight pull): just signal a
        // stop; the operator removes again once it has stopped. Avoids erasing an
        // instance a worker thread still points at.
        if (inst->run_active.load() || (m_pull_active.load() && m_pull_name == name)) {
            inst->stop_requested.store(true);
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] remove %C deferred (busy) — "
                                         "stopping; remove again when stopped\n"),
                       name.c_str()));
            return;
        }
        if (inst->host_octet) free_octet_locked(inst->host_octet);
        doomed = std::move(it->second);
        m_containers.erase(it);
        publish_instances_locked();
    }
    // The instance's run thread is already finished (run_active was false); join
    // it and clear any residual crun/overlay/veth state OUTSIDE the lock.
    if (doomed->run_thread.joinable()) doomed->run_thread.join();
    std::string err;
    CrunRuntime crun(m_cfg.run + "/state");
    crun.remove(doomed->id, true, err);
    bridge_down(doomed->id);
    unmount_overlay(m_cfg.run, doomed->id, err);
}

int Containerd::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok)
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [ctr] ds connect failed\n")), 1);

    // Baseline the command token BEFORE watching so a stale value (a previous
    // session's request) does not fire on boot.
    { std::lock_guard<std::mutex> lk(m_mtx); m_cmd_token = get_str("container.cmd.request"); }

    m_ds.set_volatile(std::vector<data_store::KV>{
        {"container.instances",     data_store::Value{std::string("[]")}},
        {"container.state",         data_store::Value{std::string("idle")}},
        {"container.pull.progress", data_store::Value{std::string("0")}},
        {"container.pull.detail",   data_store::Value{std::string("")}},
        {"container.error",         data_store::Value{std::string("")}}});

    data_store::Client::WatchHandle h = data_store::Client::kInvalidHandle;
    if (!m_ds.watch(
            std::vector<std::string>{"container.cmd.request"},
            [this](const data_store::Client::Event& ev) { on_command_event(ev); },
            &h).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [ctr] command watch failed\n")), 2);
    }

    g_log.start();
    g_log.apply_level(m_ds);
    g_log.open(m_ds, 5, 1);
    m_ds.set(std::string("services.container.state"), data_store::Value{std::string("running")});

    data_store::StatsPublisher stats("services.container",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ctr] up: root=%C run=%C (multi-container; cmd envelope ready)\n"),
        m_cfg.root.c_str(), m_cfg.run.c_str()));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace containers
