/// run_daemon() — wires DsBridge + iface_monitor + ip_route + apply
/// + Lifecycle into a simple poll loop. Mirrors openvpn-client's
/// run_daemon (blocking loop, no ACE_Reactor) for the same reason:
/// the tick is slow (seconds), the side effects are atomic shell-outs,
/// no need for event-driven plumbing.

#include "router.hpp"

#include "apply.hpp"
#include "ds_bridge.hpp"
#include "iface_monitor.hpp"
#include "ip_route.hpp"
#include "lifecycle.hpp"
#include "nft_rules.hpp"
#include "shell.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ace/Log_Msg.h>

namespace net_router {

namespace {

std::atomic_bool g_run{true};

void signal_handler(int) { g_run.store(false); }

void install_signals() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);
    // SIGPIPE: ignore — popen()/shell-outs can fire it under stress.
    struct sigaction ig{};
    ig.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ig, nullptr);
}

/// Walk net.iface.priority ("eth,wifi,cellular") + the per-slot
/// name keys to produce the ordered iface name list. Slots whose
/// name key is unset or whose tag isn't in {eth,wifi,cellular} are
/// silently dropped — Lifecycle handles an empty list as "nothing
/// up".
std::vector<std::string>
ordered_iface_names(const DsBridge& ds) {
    const std::string prio = ds.iface_priority().value_or("eth,wifi,cellular");
    auto eth  = ds.iface_eth_name();
    auto wifi = ds.iface_wifi_name();
    auto cell = ds.iface_cellular_name();

    std::vector<std::string> out;
    std::stringstream ss(prio);
    std::string tag;
    while (std::getline(ss, tag, ',')) {
        // Strip surrounding whitespace.
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front()))) tag.erase(tag.begin());
        while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back())))  tag.pop_back();
        if      (tag == "eth"      && eth  && !eth->empty())  out.push_back(*eth);
        else if (tag == "wifi"     && wifi && !wifi->empty()) out.push_back(*wifi);
        else if (tag == "cellular" && cell && !cell->empty()) out.push_back(*cell);
    }
    return out;
}

Lifecycle::Inputs snapshot_inputs(const DsBridge&                         ds,
                                  const std::vector<iface::State>&        ifaces) {
    Lifecycle::Inputs in;
    in.tun_dev             = ds.tun_dev().value_or("");
    in.lwm2m_target_ip     = ds.lwm2m_target_ip().value_or("");
    in.lwm2m_target_port   = ds.lwm2m_target_port().value_or(0);
    in.forward_ports       = nft::parse_forward_ports(
                                  ds.forward_ports().value_or("80,443,5684"));
    {
        std::string err;
        in.custom_rules = nft::parse_custom_rules(
                              ds.custom_rules().value_or(""), &err);
        if (!err.empty()) {
            ACE_DEBUG((LM_WARNING,
                       ACE_TEXT("%D [netr:%t] %M %N:%l net.custom_rules parse "
                                "error (keeping previous): %C\n"),
                       err.c_str()));
        }
    }
    in.ifaces_in_priority_order = ifaces;
    in.now_unix = static_cast<std::uint32_t>(std::time(nullptr));
    return in;
}

} // namespace

Status run_daemon(const std::string& socketPath,
                  const std::string& nft_path,
                  unsigned           poll_interval_sec_override) {
    DsBridge ds(socketPath);
    if (!ds.connected()) {
        Status s; s.ok = false; s.code = 1;
        s.err = "data-store connect failed"; return s;
    }
    if (auto missing = ds.missing_required()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < missing->size(); ++i) {
            if (i) joined << ", ";
            joined << (*missing)[i];
        }
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [netr:%t] %M %N:%l refuse to start; "
                            "required keys missing: %C\n"),
                   joined.str().c_str()));
        Status s; s.ok = false; s.code = 2;
        s.err = "missing required net.* keys"; return s;
    }

    install_signals();

    // Build sinks. Shell + apply runners use the popen()-backed
    // defaults from D5/D6. The Lifecycle never sees a syscall directly.
    auto shell_runner = shell::default_runner();
    auto nft_apply    = apply::default_nft_apply(nft_path);

    Lifecycle::Sinks sinks;
    sinks.apply_nft = [&nft_apply](const std::string& rs, std::string* err) {
        bool ok = nft_apply(rs, err);
        if (!ok) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [netr:%t] %M %N:%l nft apply FAILED: %C\n"),
                       err && !err->empty() ? err->c_str() : "(no diag)"));
        } else {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l nft ruleset applied "
                                "(%u bytes)\n"),
                       static_cast<unsigned>(rs.size())));
        }
        return ok;
    };
    sinks.apply_routes = [&shell_runner](const std::vector<iface::State>& v) {
        auto rr = route::apply_priorities(v, shell_runner);
        for (const auto& step : rr.steps) {
            if (step.applied) {
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D [netr:%t] %M %N:%l route: %C metric %u "
                                    "applied\n"),
                           step.iface.c_str(), step.metric));
            } else if (!step.error.empty() && step.error != "iface not up") {
                ACE_DEBUG((LM_WARNING,
                           ACE_TEXT("%D [netr:%t] %M %N:%l route: %C metric %u "
                                    "skipped: %C\n"),
                           step.iface.c_str(), step.metric, step.error.c_str()));
            }
        }
        return rr.all_ok;
    };
    sinks.set_state               = [&ds](const std::string& s) { ds.set_state(s); };
    sinks.set_iface_active        = [&ds](const std::string& s) { ds.set_iface_active(s); };
    sinks.set_rules_applied_count = [&ds](std::uint32_t n)      { ds.set_rules_applied_count(n); };
    sinks.set_last_apply_unix     = [&ds](std::uint32_t t)      { ds.set_last_apply_unix(t); };

    Lifecycle lc(std::move(sinks));

    // DsBridge::on_change is a hint — we don't act inside the callback
    // (it fires on the watch-thread). We just flip a flag so the next
    // tick happens immediately instead of after the full poll interval.
    std::atomic_bool wake{false};
    ds.on_change([&wake](DsBridge::Key) { wake.store(true); });

    const auto names = ordered_iface_names(ds);
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [netr:%t] %M %N:%l daemon up; priority ifaces: "
                        "%C (%u entries)\n"),
               names.empty() ? "(none)" : names.front().c_str(),
               static_cast<unsigned>(names.size())));

    while (g_run.load()) {
        const auto ifaces = iface::probe_all(names, shell_runner);
        auto in = snapshot_inputs(ds, ifaces);
        lc.step(in);

        // Sleep interval: override (CLI) > ds key > 5s. Re-read every
        // tick so an operator's `ds-cli set net.poll.interval_sec` lands
        // without a restart.
        unsigned interval = poll_interval_sec_override
                          ? poll_interval_sec_override
                          : ds.poll_interval_sec().value_or(5);
        if (interval < 1) interval = 1;

        // Short-sleep loop so SIGTERM + on_change wake reasonably fast.
        for (unsigned slept = 0; slept < interval && g_run.load(); ++slept) {
            if (wake.exchange(false)) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [netr:%t] %M %N:%l SIGTERM/SIGINT received; "
                        "exiting cleanly\n")));
    ds.set_state("stopped");
    return {};
}

} // namespace net_router
