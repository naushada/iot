#include "data_store/client.hpp"

#include "data_store/proto.hpp"
#include "proto/value_json.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>

#include <ace/LSOCK_Connector.h>
#include <ace/LSOCK_Stream.h>
#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Time_Value.h>
#include <ace/UNIX_Addr.h>

#include "nlohmann/json.hpp"

namespace data_store {

namespace {

using nlohmann::json;

Status sys_err(const std::string& what, int code = errno) {
    Status s;
    s.ok = false;
    s.code = code;
    s.err = what + ": " + std::strerror(code);
    return s;
}

Status protocol_err(const std::string& what) {
    Status s; s.ok = false; s.err = what; return s;
}

/// reqID is the EMP correlator; 8 bits → 256 slots → wrap every 256
/// requests. Listener thread uses 16-bit (op<<8 | reqID) so concurrent
/// reqs on different opcodes don't collide.
std::atomic<std::uint8_t>  g_next_reqid{1};
std::atomic<std::uint64_t> g_next_handle{1};

struct WatchEntry {
    std::vector<std::string>      keys;
    Client::EventCallback         cb;
};

struct PendingValue {
    proto::Status status = proto::Status::Ok;
    json          body;
};

/// 16-bit correlator combines opcode + reqID so two in-flight ops on
/// the same reqID slot don't collide. We never queue >256 of any one
/// opcode in flight; that's the EMP / Mihini ceiling.
inline std::uint16_t corr_key(proto::Op op, std::uint8_t reqID) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(op) << 8) | reqID);
}

} // namespace

// ─────────────────────────────── Impl ───────────────────────────────

class Client::Impl {
public:
    ACE_LSOCK_Stream     stream;
    std::atomic<bool>    connected{false};
    std::string          path;      // socket path, kept for reconnects

    std::thread          listener;
    std::atomic<bool>    stop_listener{false};

    // Guards the stream handle across writer threads vs the listener
    // thread closing/reopening it on reconnect.
    std::mutex           send_mtx;

    // corr_key → pending promise (fulfilled by the listener thread).
    std::mutex                                                  pending_mtx;
    std::unordered_map<std::uint16_t, std::promise<PendingValue>> pending;

    // Pull-style event queue.
    std::mutex                  events_mtx;
    std::condition_variable     events_cv;
    std::deque<Event>           events;

    // Callback-style watches + per-key local refcount.
    std::mutex                                              watches_mtx;
    std::unordered_map<WatchHandle, WatchEntry>             watches;
    std::unordered_map<std::string, std::size_t>            key_refcount;

    /// Outcome of one wire RegisterWatch — the two failure modes need
    /// opposite handling, so a bare Status is not enough to decide.
    enum class WatchWire {
        Ok,
        Transport,   ///< server down / timeout / half-open socket — RETRYABLE
        Rejected,    ///< server said no (schema, ACL) — permanent
    };

    Status send_frame(const std::string& frame);
    Status round_trip(proto::Op op, std::string_view body,
                      PendingValue& out, std::int32_t timeout_ms);
    WatchWire register_watch(const std::vector<std::string>& keys,
                             std::int32_t timeout_ms, Status& out);
    void   run_listener();
    void   drop_connection();
    bool   try_reconnect();
    void   fail_all_pending(const std::string& why);
};

Status Client::Impl::send_frame(const std::string& frame) {
    if (frame.empty()) return {};
    std::lock_guard<std::mutex> g(send_mtx);
    ssize_t n = stream.send_n(frame.data(), frame.size());
    if (n < 0 || static_cast<std::size_t>(n) != frame.size()) {
        return sys_err("send_n");
    }
    return {};
}

Status Client::Impl::round_trip(proto::Op op,
                                std::string_view body,
                                PendingValue& out,
                                std::int32_t timeout_ms) {
    if (!connected.load()) return protocol_err("not connected");

    const std::uint8_t  reqID = g_next_reqid.fetch_add(1);
    const std::uint16_t key   = corr_key(op, reqID);

    std::promise<PendingValue>  prom;
    std::future<PendingValue>   fut = prom.get_future();
    {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.emplace(key, std::move(prom));
    }

    std::string frame;
    proto::encode_frame_command(op, reqID, body, frame);
    auto ss = send_frame(frame);
    if (!ss.ok) {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.erase(key);
        return ss;
    }

    using namespace std::chrono;
    if (fut.wait_for(milliseconds(timeout_ms)) != std::future_status::ready) {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.erase(key);
        Status s; s.ok=false; s.code=ETIMEDOUT;
        s.err = "request timeout (op=" + proto::op_name(op) +
                " reqID=" + std::to_string(reqID) + ")";
        return s;
    }

    try {
        out = fut.get();
    } catch (const std::exception& e) {
        return protocol_err(std::string("promise broken: ") + e.what());
    }
    return {};
}

void Client::Impl::run_listener() {
    std::string buf;
    char        chunk[1024];

    while (!stop_listener.load()) {
        if (!connected.load()) {
            // Server went away (restart of iot-ds). Keep retrying the
            // connect; on success re-register every watched key — the
            // new server instance has no memory of our subscriptions.
            // Without this every long-lived daemon goes permanently
            // deaf after a ds restart (set() fails, watches dead).
            ACE_OS::sleep(ACE_Time_Value(0, 500 * 1000));
            if (stop_listener.load()) break;
            if (!try_reconnect()) continue;
            buf.clear();
        }

        ACE_Time_Value timeout(0, 200 * 1000);
        ssize_t n = stream.recv(chunk, sizeof(chunk), &timeout);
        if (n < 0) {
            if (errno == ETIME || errno == ETIMEDOUT || errno == EAGAIN) continue;
            drop_connection();
            continue;
        }
        if (n == 0) { drop_connection(); continue; }   // EOF — server gone

        buf.append(chunk, static_cast<std::size_t>(n));

        // Drain as many complete EMP frames as the recv buffer holds.
        while (true) {
            proto::Header h;
            std::string   payload;
            try {
                if (!proto::try_decode_frame(buf, h, payload)) break;
            } catch (const std::exception&) {
                // Corrupt header → drop the connection and resync via
                // a fresh one.
                drop_connection();
                buf.clear();
                break;
            }

            const proto::Op op = proto::parse_op(h.cmdID);

            if (proto::is_push(h.type)) {
                // Push: payload is the JSON body (no status prefix).
                if (op != proto::Op::NotifyEvent) continue;
                json p;
                try { p = json::parse(payload); } catch (...) { continue; }

                Event ev;
                ev.key = p.value("k", "");
                if (p.contains("v")) ev.value = value_from_json(p["v"]);
                if (p.contains("prev") && !p["prev"].is_null()) {
                    ev.prev           = value_from_json(p["prev"]);
                    ev.prev_has_value = true;
                }

                {
                    std::lock_guard<std::mutex> g(events_mtx);
                    events.push_back(ev);
                    events_cv.notify_one();
                }
                std::vector<EventCallback> cbs;
                {
                    std::lock_guard<std::mutex> g(watches_mtx);
                    for (const auto& [hnd, w] : watches) {
                        for (const auto& k : w.keys) {
                            if (k == ev.key) { cbs.push_back(w.cb); break; }
                        }
                    }
                }
                for (auto& cb : cbs) {
                    try { cb(ev); } catch (...) { /* swallow */ }
                }
                continue;
            }

            if (!proto::is_response(h.type)) continue;  // ignore

            // Response: first 2 payload bytes = status, rest = JSON body.
            PendingValue v;
            if (payload.size() < 2) {
                v.status = proto::Status::BadFrame;
            } else {
                v.status = static_cast<proto::Status>(
                    (static_cast<std::uint8_t>(payload[0]) << 8) |
                     static_cast<std::uint8_t>(payload[1]));
                if (payload.size() > 2) {
                    try {
                        v.body = json::parse(payload.data() + 2,
                                             payload.data() + payload.size());
                    } catch (...) {
                        // Tolerate empty / unparseable body — status
                        // already conveys success/failure.
                    }
                }
            }

            const std::uint16_t key = corr_key(op, h.reqID);
            std::promise<PendingValue> prom;
            bool found = false;
            {
                std::lock_guard<std::mutex> g(pending_mtx);
                auto it = pending.find(key);
                if (it != pending.end()) {
                    prom  = std::move(it->second);
                    pending.erase(it);
                    found = true;
                }
            }
            if (found) {
                try { prom.set_value(std::move(v)); } catch (...) {}
            }
        }
    }

    connected.store(false);
    fail_all_pending("listener exited");

    {
        std::lock_guard<std::mutex> g(events_mtx);
        events_cv.notify_all();
    }
}

void Client::Impl::drop_connection() {
    if (!connected.exchange(false)) return;
    {
        std::lock_guard<std::mutex> g(send_mtx);
        stream.close();
    }
    fail_all_pending("connection lost");
    {
        std::lock_guard<std::mutex> g(events_mtx);
        events_cv.notify_all();
    }
    ACE_ERROR((LM_ERROR, ACE_TEXT("%D dsclient %N:%l data-store connection ")
               ACE_TEXT("lost — reconnecting\n")));
}

bool Client::Impl::try_reconnect() {
    ACE_UNIX_Addr        addr(path.c_str());
    ACE_LSOCK_Connector  connector;
    ACE_Time_Value       timeout(2, 0);
    {
        std::lock_guard<std::mutex> g(send_mtx);
        if (connector.connect(stream, addr, &timeout) == -1) {
            stream.close();
            return false;
        }
    }
    connected.store(true);

    // Re-register every watched key. Fire-and-forget: we ARE the
    // listener thread, so a round_trip() would deadlock waiting on a
    // response only we can read. The ack lands in the recv loop and is
    // dropped as an unmatched reqID, which is harmless.
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> g(watches_mtx);
        keys.reserve(key_refcount.size());
        for (const auto& [k, cnt] : key_refcount) { (void)cnt; keys.push_back(k); }
    }
    if (!keys.empty()) {
        json req;
        req["keys"] = keys;
        const std::string body = req.dump();
        std::string frame;
        proto::encode_frame_command(proto::Op::RegisterWatch,
                                    g_next_reqid.fetch_add(1),
                                    std::string_view(body.data(), body.size()),
                                    frame);
        auto ss = send_frame(frame);
        if (!ss.ok) { drop_connection(); return false; }
    }
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D dsclient %N:%l data-store reconnected ")
               ACE_TEXT("(re-watching %d key(s))\n"),
               static_cast<int>(keys.size())));
    return true;
}

void Client::Impl::fail_all_pending(const std::string& why) {
    std::unordered_map<std::uint16_t, std::promise<PendingValue>> snapshot;
    {
        std::lock_guard<std::mutex> g(pending_mtx);
        snapshot.swap(pending);
    }
    for (auto& [id, prom] : snapshot) {
        try {
            prom.set_exception(std::make_exception_ptr(
                std::runtime_error(why)));
        } catch (...) {}
    }
}

// ─────────────────────────── Client ───────────────────────────

Client::Client() : m_impl(std::make_unique<Impl>()) {}
Client::~Client() { close(); }
Client::Client(Client&&) noexcept            = default;
Client& Client::operator=(Client&&) noexcept = default;

Status Client::connect(std::string path) {
    if (path.empty()) path = proto::kDefaultSocketPath;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        return protocol_err("socket path too long: " + path);
    }
    close();

    ACE_UNIX_Addr        addr(path.c_str());
    ACE_LSOCK_Connector  connector;
    ACE_Time_Value       timeout(5, 0);
    if (connector.connect(m_impl->stream, addr, &timeout) == -1) {
        return sys_err("ACE_LSOCK_Connector::connect(" + path + ")");
    }
    m_impl->path = path;
    m_impl->connected.store(true);
    m_impl->stop_listener.store(false);
    m_impl->listener = std::thread([impl = m_impl.get()]() {
        impl->run_listener();
    });
    return {};
}

namespace {

/// Translate a wire-level status + optional error body into a
/// caller-friendly Status. Ok → empty Status{}, anything else gets
/// the err string lifted from the body when present.
Status status_from(const PendingValue& v, const char* op_label) {
    if (v.status == proto::Status::Ok) return {};
    Status s; s.ok = false;
    s.code = static_cast<int>(v.status);
    if (v.body.is_object() && v.body.contains("err") &&
        v.body["err"].is_string()) {
        s.err = v.body["err"].get<std::string>();
    } else {
        s.err = std::string(op_label) + " failed (status=" +
                proto::status_name(v.status) + ")";
    }
    return s;
}

} // namespace

Status Client::schema_dump(std::string& out_json, std::int32_t timeout_ms) {
    out_json.clear();
    PendingValue out;
    auto rs = m_impl->round_trip(proto::Op::SchemaDump,
                                 std::string_view{},  // empty body
                                 out, timeout_ms);
    if (!rs.ok) return rs;
    auto err = status_from(out, "schema-dump");
    if (!err.ok) return err;
    out_json = out.body.dump();
    return {};
}

Status Client::set(const std::vector<KV>& pairs, std::int32_t timeout_ms) {
    json req;
    req["keys"] = json::array();
    for (const auto& kv : pairs) {
        json e;
        e[kv.first] = value_to_json(kv.second);
        req["keys"].push_back(e);
    }
    const std::string body = req.dump();
    PendingValue out;
    auto rs = m_impl->round_trip(proto::Op::Set,
                                 std::string_view(body.data(), body.size()),
                                 out, timeout_ms);
    if (!rs.ok) return rs;
    return status_from(out, "set");
}

Status Client::set_volatile(const std::vector<KV>& pairs,
                             std::int32_t timeout_ms) {
    json req;
    req["volatile"] = true;
    req["keys"] = json::array();
    for (const auto& kv : pairs) {
        json e;
        e[kv.first] = value_to_json(kv.second);
        req["keys"].push_back(e);
    }
    const std::string body = req.dump();
    PendingValue out;
    auto rs = m_impl->round_trip(proto::Op::Set,
                                 std::string_view(body.data(), body.size()),
                                 out, timeout_ms);
    if (!rs.ok) return rs;
    return status_from(out, "set_volatile");
}

Status Client::get(const std::vector<std::string>& keys,
                   std::vector<GetResult>&         out_results,
                   std::int32_t                    timeout_ms) {
    out_results.clear();
    json req;
    req["keys"] = keys;
    const std::string body = req.dump();
    PendingValue out;
    auto rs = m_impl->round_trip(proto::Op::Get,
                                 std::string_view(body.data(), body.size()),
                                 out, timeout_ms);
    if (!rs.ok) return rs;
    auto err = status_from(out, "get");
    if (!err.ok) return err;
    if (!out.body.is_object() || !out.body.contains("data") ||
        !out.body["data"].is_array()) {
        return protocol_err("get response missing data array");
    }
    for (const auto& item : out.body["data"]) {
        GetResult g;
        g.key = item.value("k", "");
        if (item.contains("v") && !item["v"].is_null()) {
            g.value     = value_from_json(item["v"]);
            g.has_value = true;
        }
        out_results.push_back(std::move(g));
    }
    return {};
}

Client::Impl::WatchWire
Client::Impl::register_watch(const std::vector<std::string>& keys,
                             std::int32_t                    timeout_ms,
                             Status&                         out) {
    // key_refcount is our subscription INTENT, and try_reconnect() rebuilds the
    // server's view from it. So the refcount goes up FIRST and only comes back
    // down when the server permanently refuses the key — never on a transport
    // hiccup. Only keys that just went 0 → 1 need a wire registration.
    std::vector<std::string> wireKeys;
    {
        std::lock_guard<std::mutex> g(watches_mtx);
        for (const auto& k : keys) {
            if (key_refcount[k]++ == 0) wireKeys.push_back(k);
        }
    }
    if (wireKeys.empty()) { out = Status{}; return WatchWire::Ok; }

    json req;
    req["keys"] = wireKeys;
    const std::string body = req.dump();
    PendingValue resp;
    auto rs = round_trip(proto::Op::RegisterWatch,
                         std::string_view(body.data(), body.size()),
                         resp, timeout_ms);
    if (!rs.ok) {
        // TRANSPORT failure: ds-server was down, restarting, or the socket was
        // half-open. This used to erase the key from key_refcount — which made
        // the loss PERMANENT, because the reconnect path re-registers only what
        // key_refcount still holds. One timed-out RegisterWatch at boot and that
        // key was deaf for the life of the process, silently. Keep the intent and
        // force a reconnect; try_reconnect() re-sends the whole subscription set.
        out = rs;
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D dsclient %N:%l RegisterWatch failed (%C) — keeping ")
                   ACE_TEXT("the subscription, re-registering on reconnect\n"),
                   rs.err.c_str()));
        drop_connection();
        return WatchWire::Transport;
    }

    out = status_from(resp, "watch");
    if (!out.ok) {
        // The SERVER refused (schema / read-ACL). That is a permanent answer, not
        // a hiccup — retrying it on every reconnect would hammer a doomed key, so
        // release the intent and let the caller see the error.
        std::lock_guard<std::mutex> g(watches_mtx);
        for (const auto& k : wireKeys) {
            auto it = key_refcount.find(k);
            if (it != key_refcount.end() && it->second > 0) {
                if (--it->second == 0) key_refcount.erase(it);
            }
        }
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D dsclient %N:%l RegisterWatch rejected: %C\n"),
                   out.err.c_str()));
        return WatchWire::Rejected;
    }
    return WatchWire::Ok;
}

Status Client::watch(const std::vector<std::string>& keys,
                     std::int32_t                    timeout_ms) {
    Status s;
    m_impl->register_watch(keys, timeout_ms, s);
    return s;
}

Status Client::watch(const std::vector<std::string>& keys,
                     EventCallback                   cb,
                     WatchHandle*                    out_handle,
                     std::int32_t                    timeout_ms) {
    if (!cb) return protocol_err("watch: callback is null");

    // The callback table is local intent too, so it is populated BEFORE the wire
    // round-trip. Registering it only on success meant a transient RegisterWatch
    // failure left the caller holding no callback at all — and since callers
    // routinely ignore the returned Status, the daemon just went quietly deaf.
    // Now a failed registration still leaves a live subscription that the
    // reconnect path heals.
    WatchHandle h = g_next_handle.fetch_add(1);
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        WatchEntry w;
        w.keys = keys;
        w.cb   = cb;
        m_impl->watches.emplace(h, std::move(w));
    }

    Status s;
    const auto wire = m_impl->register_watch(keys, timeout_ms, s);
    if (wire == Impl::WatchWire::Rejected) {
        // Permanently refused — the key is gone from key_refcount, so a callback
        // for it would never fire. Don't hand back a handle that means nothing.
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        m_impl->watches.erase(h);
        return s;
    }
    if (out_handle) *out_handle = h;
    return s;   // Transport → error returned, but the subscription lives on
}

Status Client::unwatch(WatchHandle handle, std::int32_t timeout_ms) {
    if (handle == kInvalidHandle) return protocol_err("invalid handle");

    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        auto it = m_impl->watches.find(handle);
        if (it == m_impl->watches.end()) {
            return protocol_err("unknown watch handle");
        }
        keys = it->second.keys;
        m_impl->watches.erase(it);
    }
    return unwatch(keys, timeout_ms);
}

Status Client::unwatch(const std::vector<std::string>& keys,
                       std::int32_t                    timeout_ms) {
    std::vector<std::string> wireKeys;
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        for (const auto& k : keys) {
            auto it = m_impl->key_refcount.find(k);
            if (it == m_impl->key_refcount.end()) continue;
            if (--it->second == 0) {
                m_impl->key_refcount.erase(it);
                wireKeys.push_back(k);
            }
        }
    }
    if (wireKeys.empty()) return {};

    json req;
    req["keys"] = wireKeys;
    const std::string body = req.dump();
    PendingValue out;
    auto rs = m_impl->round_trip(proto::Op::RemoveWatch,
                                 std::string_view(body.data(), body.size()),
                                 out, timeout_ms);
    if (!rs.ok) return rs;
    return status_from(out, "unwatch");
}

Status Client::recv_event(Event& out, std::int32_t timeout_ms) {
    std::unique_lock<std::mutex> g(m_impl->events_mtx);
    if (!m_impl->events_cv.wait_for(g,
            std::chrono::milliseconds(timeout_ms),
            [&]() { return !m_impl->events.empty() ||
                           !m_impl->connected.load(); })) {
        Status s; s.ok=false; s.code=ETIMEDOUT;
        s.err = "no event within timeout"; return s;
    }
    if (m_impl->events.empty()) return protocol_err("connection closed");
    out = m_impl->events.front();
    m_impl->events.pop_front();
    return {};
}

void Client::close() {
    if (!m_impl) return;
    // The listener may be mid-reconnect with `connected == false`, so
    // the thread must be stopped and joined regardless of that flag.
    m_impl->stop_listener.store(true);
    m_impl->connected.store(false);
    {
        std::lock_guard<std::mutex> g(m_impl->send_mtx);
        ::shutdown(m_impl->stream.get_handle(), SHUT_RDWR);
    }
    if (m_impl->listener.joinable()) m_impl->listener.join();
    m_impl->stream.close();
    m_impl->fail_all_pending("client closed");
    {
        std::lock_guard<std::mutex> g(m_impl->events_mtx);
        m_impl->events_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        m_impl->watches.clear();
        m_impl->key_refcount.clear();
    }
}

int Client::fd() const {
    if (!m_impl || !m_impl->connected.load()) return -1;
    return const_cast<ACE_LSOCK_Stream&>(m_impl->stream).get_handle();
}

} // namespace data_store
