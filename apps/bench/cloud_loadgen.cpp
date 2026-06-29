/**
 * @file cloud_loadgen.cpp
 * @brief Cloud LwM2M registration-plane load generator (benchmark "a").
 *
 * Simulates N IoT devices performing the *exact* device wire flow against a
 * running cloud (lwm2m-bs + lwm2m-dm), to measure how many concurrent devices
 * the single-threaded ACE_Reactor cloud actually supports.
 *
 * Per simulated device, reusing the production transport + FSMs verbatim:
 *   1. DTLS-PSK handshake to the Bootstrap server (identity = serial, key =
 *      HKDF(master, serial) — the zero-touch tier, so no per-device JSON);
 *   2. POST /bs  (lwm2m::bootstrap::Client) → consume Object 0/1 writes;
 *   3. switch DTLS identity to the bootstrap-delivered DM identity + connect;
 *   4. POST /rd  (lwm2m::RegistrationClient) → 2.01 Created;
 *   5. lifetime Updates to stay registered for the soak window.
 *
 * This drives the SAME byte-builders/parsers the real device uses (no toy
 * reimplementation): DTLSAdapter, CoAPAdapter, ObjectStore, the bootstrap +
 * registration client FSMs. The cloud cannot tell these from real RPis.
 *
 * Scope: registration plane only — it does NOT bring up an OpenVPN tunnel, so
 * it is not bounded by the 254-IP / ~51-proxy-port VPN caps. It measures the
 * reactor/DTLS/registration headroom, which is the unmeasured wall.
 *
 * Build: see apps/bench/CMakeLists.txt. Run: see apps/bench/run-bench.sh.
 *
 * NOT shipped in the cloud image — a standalone test tool, built explicitly.
 * See apps/docs/tdd-cloud-load-benchmark.md.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <sys/resource.h>   // setrlimit (raise the fd ceiling for N sockets)

#include "ace/Reactor.h"
#include "ace/Dev_Poll_Reactor.h"
#include "ace/Event_Handler.h"
#include "ace/INET_Addr.h"
#include "ace/SOCK_Dgram.h"
#include "ace/Log_Msg.h"
#include "ace/Time_Value.h"

#include "coap_adapter.hpp"
#include "dtls_adapter.hpp"
#include "lwm2m_bootstrap.hpp"
#include "lwm2m_bootstrap_client.hpp"
#include "lwm2m_object_store.hpp"
#include "lwm2m_registration_client.hpp"
#include "psk_gen.hpp"
#include "tenant_policy.hpp"

// tinydtls' numeric.h (pulled in transitively via dtls.h) #defines min()/max()
// as function-like macros, which clobber std::min/std::max. Undo them so the
// std versions below compile.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

using clock_t_ = std::chrono::steady_clock;
using tp_t     = clock_t_::time_point;

// ─────────────────────────── configuration ─────────────────────────────────

struct Config {
    std::string bsHost     = "127.0.0.1";
    std::uint16_t bsPort   = 5684;
    std::string masterHex;                 ///< raw 64-hex HKDF master (REQUIRED)
    std::string serialPrefix = "bench";    ///< endpoint = <prefix><index>
    std::uint32_t count    = 100;          ///< devices to spawn
    double rampPerSec      = 50.0;         ///< new devices started per second
    std::uint32_t soakSecs = 60;           ///< hold after the last device starts
    std::uint32_t lifetime = 90;           ///< pre-bootstrap default lt (BS overrides)
    std::uint32_t bootTimeoutSecs = 30;    ///< per-device give-up → Failed
    std::string csvPath;                   ///< optional per-device CSV dump
    bool emitCreds = false;                ///< print cloud.endpoint.credentials + exit
    std::string tenant;                    ///< tenant slug ("" = default tenant)
    log_t logLevel = DTLS_LOG_EMERG;       ///< keep tinydtls quiet at scale
};

// Per-device 128-bit (16-byte) keys, derived deterministically from the seed +
// serial so the loadgen and the provisioned cloud.endpoint.credentials agree.
// tinydtls caps PSKs at DTLS_PSK_MAX_KEY_LEN (16 B), so we use the manual
// provisioning tier with 128-bit keys (the HW-validated path) — NOT the
// zero-touch 256-bit derivation, whose full 32-byte PSK overflows that buffer.
static std::string bs_key16(const std::string& seed, const std::string& serial) {
    return iot::derive_bs_psk_hex(seed, serial).substr(0, 32);
}
static std::string dm_key16(const std::string& seed, const std::string& serial) {
    return iot::derive_dm_psk_hex(seed, serial).substr(0, 32);
}

// ─────────────────────────── per-device milestones ─────────────────────────

enum class Phase : std::uint8_t { NotStarted, BsInProgress, BootDone, Failed };

struct DevMetrics {
    tp_t  start{};
    tp_t  bsConnected{};   ///< BS DTLS handshake complete
    tp_t  bsSent{};        ///< POST /bs on the wire
    tp_t  bootDone{};      ///< bootstrap committed (on_done)
    tp_t  dmConnected{};   ///< DM DTLS handshake complete
    tp_t  regSent{};       ///< POST /rd on the wire
    tp_t  registered{};    ///< 2.01 Created received
    bool  isRegistered{false};
    std::string failStage; ///< empty unless Failed; the furthest stage reached
};

// Forward.
class TickDriver;

// ─────────────────────────── one simulated device ──────────────────────────

class SimDevice : public ACE_Event_Handler {
public:
    SimDevice(std::uint32_t index, const Config& cfg, ACE_Reactor* reactor)
        : m_cfg(cfg),
          m_serial(cfg.serialPrefix + std::to_string(index)) {
        this->reactor(reactor);
    }

    ~SimDevice() override {
        if (m_fd != ACE_INVALID_HANDLE && this->reactor())
            this->reactor()->remove_handler(
                this, ACE_Event_Handler::READ_MASK |
                          ACE_Event_Handler::DONT_CALL);
        m_dtls.reset();   // frees the tinydtls context before the socket closes
        m_sock.close();
    }

    /// Open the socket, build the DTLS/CoAP/FSM stack, install the BS PSK and
    /// kick the Bootstrap handshake. Returns false on socket failure.
    bool start() {
        m_m.start = clock_t_::now();
        // Arm the re-kick clock now so the first tick doesn't immediately
        // re-connect() and reset the in-flight handshake (epoch-0 would read as
        // "3s overdue"). tinydtls' check_retransmit drives flight retransmits;
        // connect() is only re-kicked if the handshake is genuinely stalled.
        m_lastConnectKick = m_m.start;

        ACE_INET_Addr any((u_short)0);     // ephemeral local UDP port
        if (m_sock.open(any) == -1) {
            ACE_ERROR_RETURN((LM_ERROR,
                ACE_TEXT("%D loadgen %N:%l socket open failed for %C\n"),
                m_serial.c_str()), false);
        }
        m_fd = m_sock.get_handle();

        // Device-agnostic tenancy (Option B): the device bootstraps with its
        // BARE serial regardless of tenant — the tenant lives only in the cred
        // row's tag (emitted by emit-creds), never on the wire.
        const std::string ep = m_serial;

        m_dtls  = std::make_shared<DTLSAdapter>(m_fd, m_cfg.logLevel);
        m_store = std::make_shared<lwm2m::ObjectStore>();
        m_bs    = std::make_shared<lwm2m::bootstrap::Client>(
                      ep, m_store, m_dtls);

        lwm2m::ClientConfig rc;
        rc.endpoint     = ep;
        rc.lifetime     = m_cfg.lifetime;   // BS Server Object RID 1 overrides
        rc.binding      = "U";
        rc.lwm2mVersion = "1.1";
        m_reg = std::make_shared<lwm2m::RegistrationClient>(rc, *m_store);

        // Route inbound CoAP (BS writes, the 2.01 Created) to the FSMs, exactly
        // as the real LwM2MClient ServiceContext does.
        m_dtls->coapAdapter()->bootstrapClient(m_bs);
        m_dtls->coapAdapter()->registrationClient(m_reg);

        // BS credential: identity = canonical sha256(endpoint)[:32] (the
        // commissioned-tier identity resolve_bs_psk matches in step 1, so the
        // provisioned row needs no "identity" field — keeps the creds JSON under
        // the 128 KB ds-cli arg cap at high N). Secret = 128-bit key derived
        // from seed + serial. add_credential stores hex; the PSK callback
        // hex-decodes it to 16 bytes (fits DTLS_PSK_MAX_KEY_LEN).
        const std::string bsId     = iot::bs_identity(std::string(), m_serial);
        const std::string bsPskHex = bs_key16(m_cfg.masterHex, m_serial);
        if (bsPskHex.empty()) {
            ACE_ERROR_RETURN((LM_ERROR,
                ACE_TEXT("%D loadgen %N:%l empty BS PSK (bad master?) for %C\n"),
                m_serial.c_str()), false);
        }
        m_dtls->add_credential(bsId, bsPskHex);
        m_dtls->active_identity(bsId);

        wire_on_done();

        if (this->reactor()->register_handler(
                this, ACE_Event_Handler::READ_MASK) == -1) {
            ACE_ERROR_RETURN((LM_ERROR,
                ACE_TEXT("%D loadgen %N:%l register_handler failed for %C\n"),
                m_serial.c_str()), false);
        }

        m_dtls->connect(m_cfg.bsHost, m_cfg.bsPort);   // ClientHello to BS
        return true;
    }

    ACE_HANDLE get_handle() const override { return m_fd; }

    /// Reactor read-readiness → drain one datagram. rx() runs the whole
    /// receive→decrypt→dispatch→reply path and fires the FSM callbacks.
    int handle_input(ACE_HANDLE) override {
        if (m_dtls) m_dtls->rx(m_fd);
        sample_state();   // pick up FSM transitions that just happened
        return 0;
    }

    /// Driven from the global 10 Hz tick: advance the client state machine.
    void tick(tp_t now) {
        if (m_phase == Phase::Failed || m_m.isRegistered) return;

        // Per-device give-up so a stuck handshake doesn't pin the test.
        if (now - m_m.start >
            std::chrono::seconds(m_cfg.bootTimeoutSecs)) {
            fail_here();
            return;
        }

        if (m_dtls) m_dtls->check_retransmit();   // resend handshake flights
        const bool connected =
            m_dtls && m_dtls->clientState() == "connected";

        if (m_reg->state() == lwm2m::RegistrationState::Unregistered) {
            if (m_phase == Phase::NotStarted) {
                if (connected) {
                    if (m_m.bsConnected.time_since_epoch().count() == 0)
                        m_m.bsConnected = now;
                    auto p = m_bs->build_bs_request(
                        next_msgid(), std::string{static_cast<char>(0x40)});
                    m_dtls->tx_peer(p);
                    m_m.bsSent = now;
                    m_phase    = Phase::BsInProgress;
                } else if (now - m_lastConnectKick >=
                           std::chrono::seconds(3)) {
                    m_dtls->connect(m_cfg.bsHost, m_cfg.bsPort);  // re-kick
                    m_lastConnectKick = now;
                }
            } else if (m_phase == Phase::BootDone && connected) {
                if (m_m.dmConnected.time_since_epoch().count() == 0)
                    m_m.dmConnected = now;
                auto p = m_reg->build_register_request(
                    next_msgid(), std::string{static_cast<char>(0x10)});
                m_dtls->tx_peer(p);
                m_m.regSent = now;
            }
        }

        // Lifetime Update keepalive (keeps the device "registered" for soak).
        if (m_reg->should_send_update(now)) {
            auto p = m_reg->build_update_request(
                next_msgid(), std::string{static_cast<char>(0x02)}, false);
            m_dtls->tx_peer(p);
            m_reg->note_update_sent(now);
        }
    }

    const DevMetrics& metrics() const { return m_m; }
    const std::string& serial() const { return m_serial; }

private:
    void wire_on_done() {
        // Mirrors apps/src/main.cpp on_done, minus the data-store persistence:
        // the bootstrap FSM has already installed the DM PSK via add_credential
        // (RID 5 hex), so we only switch identity + peer and connect to the DM.
        m_bs->on_done([this](const lwm2m::bootstrap::StagingBuffer& c) {
            std::string dmUri, dmIdentity;
            for (const auto& s : c.security) {
                if (s.isBootstrapServer || s.securityMode != 0) continue;
                if (s.identity.empty() || s.secretKey.empty()) continue;
                dmUri = s.serverUri; dmIdentity = s.identity; break;
            }
            std::string dmHost; std::uint16_t dmPort = 0;
            parse_coaps(dmUri, dmHost, dmPort);
            if (dmHost.empty() || dmPort == 0) { fail_here(); return; }

            if (!c.server.empty())
                m_reg->set_lifetime(c.server.front().lifetime);

            m_dtls->active_identity(dmIdentity);
            m_dtls->clientState("");            // drop the BS session state
            m_dtls->connect(dmHost, dmPort);    // start the DM handshake
            m_m.bootDone = clock_t_::now();
            m_phase = Phase::BootDone;
        });
    }

    void sample_state() {
        if (!m_m.isRegistered &&
            m_reg->state() == lwm2m::RegistrationState::Registered) {
            m_m.registered   = clock_t_::now();
            m_m.isRegistered = true;
        }
    }

    void fail_here() {
        m_phase = Phase::Failed;
        if      (m_m.bsConnected.time_since_epoch().count() == 0)
            m_m.failStage = "dtls-bs";
        else if (m_m.bootDone.time_since_epoch().count() == 0)
            m_m.failStage = "bootstrap";
        else if (m_m.dmConnected.time_since_epoch().count() == 0)
            m_m.failStage = "dtls-dm";
        else
            m_m.failStage = "register";
    }

    std::uint16_t next_msgid() { return ++m_msgid; }

    // coaps://host:port → host, port. Tiny, sufficient for the DM URI.
    static void parse_coaps(const std::string& uri,
                            std::string& host, std::uint16_t& port) {
        host.clear(); port = 0;
        auto p = uri.find("://");
        if (p == std::string::npos) return;
        std::string rest = uri.substr(p + 3);
        auto slash = rest.find('/');
        if (slash != std::string::npos) rest = rest.substr(0, slash);
        auto colon = rest.rfind(':');
        if (colon == std::string::npos) { host = rest; return; }
        host = rest.substr(0, colon);
        port = static_cast<std::uint16_t>(std::stoi(rest.substr(colon + 1)));
    }

    const Config&                              m_cfg;
    std::string                                m_serial;
    ACE_SOCK_Dgram                             m_sock;
    ACE_HANDLE                                 m_fd{ACE_INVALID_HANDLE};
    std::shared_ptr<DTLSAdapter>               m_dtls;
    std::shared_ptr<lwm2m::ObjectStore>        m_store;
    std::shared_ptr<lwm2m::bootstrap::Client>  m_bs;
    std::shared_ptr<lwm2m::RegistrationClient> m_reg;
    Phase                                      m_phase{Phase::NotStarted};
    DevMetrics                                 m_m;
    std::uint16_t                              m_msgid{0};
    tp_t                                       m_lastConnectKick{};
};

// ─────────────────────────── global tick / ramp / end ──────────────────────

class TickDriver : public ACE_Event_Handler {
public:
    TickDriver(const Config& cfg, ACE_Reactor* reactor,
               std::vector<std::unique_ptr<SimDevice>>& devices)
        : m_cfg(cfg), m_devices(devices) {
        this->reactor(reactor);
    }

    int handle_timeout(const ACE_Time_Value&, const void*) override {
        const tp_t now = clock_t_::now();

        // Ramp: spawn up to (rampPerSec * tickInterval) new devices per tick.
        if (m_spawned < m_cfg.count) {
            if (m_lastSpawn.time_since_epoch().count() == 0) m_lastSpawn = now;
            const double due =
                std::chrono::duration<double>(now - m_lastSpawn).count() *
                m_cfg.rampPerSec;
            std::uint32_t toSpawn = static_cast<std::uint32_t>(due);
            if (toSpawn > 0) {
                m_lastSpawn = now;
                for (std::uint32_t i = 0;
                     i < toSpawn && m_spawned < m_cfg.count; ++i) {
                    auto dev = std::make_unique<SimDevice>(
                        m_spawned, m_cfg, this->reactor());
                    if (dev->start()) m_devices.push_back(std::move(dev));
                    ++m_spawned;
                }
            }
        } else if (m_allStartedAt.time_since_epoch().count() == 0) {
            m_allStartedAt = now;
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D loadgen all %u devices started; "
                "soaking %u s\n"), m_spawned, m_cfg.soakSecs));
        }

        for (auto& d : m_devices) d->tick(now);

        // Progress line ~1 Hz.
        if (now - m_lastLog >= std::chrono::seconds(1)) {
            m_lastLog = now;
            std::uint32_t reg = 0, fail = 0;
            for (auto& d : m_devices) {
                if (d->metrics().isRegistered) ++reg;
                else if (!d->metrics().failStage.empty()) ++fail;
            }
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D loadgen started=%u registered=%u "
                "failed=%u\n"), m_spawned, reg, fail));
        }

        // End condition: ramp complete AND soak elapsed.
        if (m_allStartedAt.time_since_epoch().count() != 0 &&
            now - m_allStartedAt >= std::chrono::seconds(m_cfg.soakSecs)) {
            this->reactor()->end_reactor_event_loop();
        }
        return 0;
    }

private:
    const Config&                            m_cfg;
    std::vector<std::unique_ptr<SimDevice>>& m_devices;
    std::uint32_t                            m_spawned{0};
    tp_t m_lastSpawn{}, m_allStartedAt{}, m_lastLog{};
};

// ─────────────────────────── reporting ─────────────────────────────────────

static double pctl(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t i = static_cast<std::size_t>(
        p / 100.0 * (v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

static double ms(tp_t a, tp_t b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static void report(const Config& cfg,
                   const std::vector<std::unique_ptr<SimDevice>>& devices) {
    std::uint32_t bs = 0, boot = 0, dm = 0, reg = 0;
    std::vector<double> ttr, hsh, btt;          // time-to-register, handshake, bootstrap
    std::vector<tp_t>   regTimes;
    std::vector<std::string> failStages;

    for (auto& d : devices) {
        const auto& m = d->metrics();
        if (m.bsConnected.time_since_epoch().count()) ++bs;
        if (m.bootDone.time_since_epoch().count())    ++boot;
        if (m.dmConnected.time_since_epoch().count()) ++dm;
        if (m.isRegistered) {
            ++reg;
            ttr.push_back(ms(m.start, m.registered));
            if (m.bsConnected.time_since_epoch().count())
                hsh.push_back(ms(m.start, m.bsConnected));
            if (m.bootDone.time_since_epoch().count() &&
                m.bsConnected.time_since_epoch().count())
                btt.push_back(ms(m.bsConnected, m.bootDone));
            regTimes.push_back(m.registered);
        } else if (!m.failStage.empty()) {
            failStages.push_back(m.failStage);
        }
    }

    // Registrations/sec: bucket completion times into 1 s bins.
    std::uint32_t peakRps = 0;
    if (!regTimes.empty()) {
        std::sort(regTimes.begin(), regTimes.end());
        auto base = regTimes.front();
        std::vector<std::uint32_t> bins;
        for (auto t : regTimes) {
            std::size_t b = static_cast<std::size_t>(
                std::chrono::duration<double>(t - base).count());
            if (b >= bins.size()) bins.resize(b + 1, 0);
            ++bins[b];
        }
        for (auto c : bins) peakRps = std::max(peakRps, c);
    }

    std::printf("\n");
    std::printf("================ cloud loadgen report ================\n");
    std::printf("target               : %s:%u (BS)  master=%s\n",
                cfg.bsHost.c_str(), cfg.bsPort,
                cfg.masterHex.empty() ? "<none>" : "set");
    std::printf("devices spawned      : %u  (ramp %.0f/s, soak %us)\n",
                cfg.count, cfg.rampPerSec, cfg.soakSecs);
    std::printf("------------------------------------------------------\n");
    std::printf("reached BS-connected : %u\n", bs);
    std::printf("reached bootstrap    : %u\n", boot);
    std::printf("reached DM-connected : %u\n", dm);
    std::printf("REGISTERED (success) : %u / %u  (%.1f%%)\n",
                reg, cfg.count, cfg.count ? 100.0 * reg / cfg.count : 0.0);
    std::printf("failed               : %zu\n", failStages.size());
    {
        std::uint32_t f_bs=0,f_boot=0,f_dm=0,f_reg=0;
        for (auto& s : failStages) {
            if      (s == "dtls-bs")   ++f_bs;
            else if (s == "bootstrap") ++f_boot;
            else if (s == "dtls-dm")   ++f_dm;
            else                       ++f_reg;
        }
        std::printf("  by stage           : dtls-bs=%u bootstrap=%u "
                    "dtls-dm=%u register=%u\n", f_bs, f_boot, f_dm, f_reg);
    }
    std::printf("------------------------------------------------------\n");
    std::printf("peak registrations/s : %u\n", peakRps);
    auto stat = [](const char* name, std::vector<double> v) {
        if (v.empty()) { std::printf("%-21s: (none)\n", name); return; }
        double sum = 0; for (double x : v) sum += x;
        std::printf("%-21s: p50=%.0f  p90=%.0f  p99=%.0f  max=%.0f  mean=%.0f (ms)\n",
            name, pctl(v,50), pctl(v,90), pctl(v,99),
            *std::max_element(v.begin(), v.end()), sum / v.size());
    };
    stat("time-to-register", ttr);
    stat("dtls handshake", hsh);
    stat("bootstrap exchange", btt);
    std::printf("======================================================\n");

    if (!cfg.csvPath.empty()) {
        std::ofstream csv(cfg.csvPath);
        csv << "serial,registered,ttr_ms,handshake_ms,bootstrap_ms,fail_stage\n";
        for (auto& d : devices) {
            const auto& m = d->metrics();
            csv << d->serial() << ',' << (m.isRegistered ? 1 : 0) << ','
                << (m.isRegistered ? ms(m.start, m.registered) : 0) << ','
                << (m.bsConnected.time_since_epoch().count()
                        ? ms(m.start, m.bsConnected) : 0) << ','
                << (m.bootDone.time_since_epoch().count() &&
                    m.bsConnected.time_since_epoch().count()
                        ? ms(m.bsConnected, m.bootDone) : 0) << ','
                << m.failStage << '\n';
        }
        std::printf("per-device CSV       : %s\n", cfg.csvPath.c_str());
    }
}

// ─────────────────────────── arg parsing / main ────────────────────────────

static bool arg(const std::string& a, const char* key, std::string& out) {
    const std::string k = std::string(key) + "=";
    if (a.rfind(k, 0) == 0) { out = a.substr(k.size()); return true; }
    return false;
}

// Print the cloud.endpoint.credentials JSON array the cloud needs to accept
// this run's devices (manual tier). Keys match bs_key16/dm_key16 so the loadgen
// and the cloud agree byte-for-byte.
static void emit_creds(const Config& cfg) {
    std::printf("[");
    for (std::uint32_t i = 0; i < cfg.count; ++i) {
        const std::string s = cfg.serialPrefix + std::to_string(i);
        // No "identity" field: the device presents the bare sha256(serial)[:32]
        // (Option B — tenant-agnostic), which the cloud recomputes per row.
        // Omitting it keeps the array under ds-cli's 128 KB single-arg cap at
        // high N. The "tenant" tag is emitted for a non-default tenant (default
        // rows stay untagged = legacy); identities stay BARE either way.
        const std::string tenantField =
            cfg.tenant.empty() ? std::string()
                               : ("\"tenant\":\"" + cfg.tenant + "\",");
        std::printf("%s{\"serial\":\"%s\",%s"
                    "\"bs.psk.key\":\"%s\",\"dm.psk.id\":\"%s\","
                    "\"dm.psk.key\":\"%s\"}",
                    i ? "," : "", s.c_str(), tenantField.c_str(),
                    bs_key16(cfg.masterHex, s).c_str(),
                    iot::dm_identity(std::string(), s).c_str(),
                    dm_key16(cfg.masterHex, s).c_str());
    }
    std::printf("]\n");
}

int main(int argc, char* argv[]) {
    Config cfg;
    std::string v;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (arg(a, "bs-host", v)) cfg.bsHost = v;
        else if (arg(a, "bs-port", v)) cfg.bsPort = (std::uint16_t)std::stoi(v);
        else if (arg(a, "master", v))  cfg.masterHex = v;
        else if (arg(a, "count", v))   cfg.count = (std::uint32_t)std::stoul(v);
        else if (arg(a, "ramp", v))    cfg.rampPerSec = std::stod(v);
        else if (arg(a, "soak", v))    cfg.soakSecs = (std::uint32_t)std::stoul(v);
        else if (arg(a, "lifetime", v))cfg.lifetime = (std::uint32_t)std::stoul(v);
        else if (arg(a, "boot-timeout", v))
            cfg.bootTimeoutSecs = (std::uint32_t)std::stoul(v);
        else if (arg(a, "prefix", v))  cfg.serialPrefix = v;
        else if (arg(a, "csv", v))     cfg.csvPath = v;
        else if (arg(a, "tenant", v))  cfg.tenant = v;
        else if (arg(a, "emit-creds", v)) cfg.emitCreds = (v != "0");
        else if (a == "-h" || a == "--help") {
            std::printf(
                "usage: cloud_loadgen master=<64hex> [bs-host=H] [bs-port=5684]\n"
                "       [count=N] [ramp=devices/s] [soak=secs] [lifetime=secs]\n"
                "       [boot-timeout=secs] [prefix=str] [csv=path]\n");
            return 0;
        }
    }
    if (cfg.masterHex.size() != 64) {
        std::fprintf(stderr,
            "error: master=<64 hex chars> is required (key-derivation seed)\n");
        return 2;
    }

    // Provisioning helper mode: print the credentials JSON and exit (no I/O).
    if (cfg.emitCreds) { emit_creds(cfg); return 0; }

    // Raise the fd ceiling: one UDP socket + tinydtls bookkeeping per device.
    struct rlimit rl{};
    getrlimit(RLIMIT_NOFILE, &rl);
    rlim_t want = static_cast<rlim_t>(cfg.count) + 1024;
    if (rl.rlim_cur < want) {
        rl.rlim_cur = std::min(want, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    // Dev-poll reactor scales to many fds (select() caps at FD_SETSIZE).
    auto impl = new ACE_Dev_Poll_Reactor(
        static_cast<size_t>(cfg.count) + 64);
    ACE_Reactor reactor(impl, 1 /*delete impl on dtor*/);

    std::vector<std::unique_ptr<SimDevice>> devices;
    devices.reserve(cfg.count);
    TickDriver driver(cfg, &reactor, devices);

    // 10 Hz tick drives ramp + per-device FSM progress.
    ACE_Time_Value interval(0, 100 * 1000);
    reactor.schedule_timer(&driver, nullptr, ACE_Time_Value::zero, interval);

    ACE_DEBUG((LM_INFO, ACE_TEXT("%D loadgen starting: %u devices → %C:%u, "
        "ramp=%.0f/s soak=%us\n"), cfg.count, cfg.bsHost.c_str(), cfg.bsPort,
        cfg.rampPerSec, cfg.soakSecs));

    reactor.run_reactor_event_loop();

    report(cfg, devices);
    return 0;
}
