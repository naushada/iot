#include "data_store/client.hpp"

#include "data_store/proto.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>

#include <ace/LSOCK_Connector.h>
#include <ace/LSOCK_Stream.h>
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

std::atomic<std::uint64_t> g_next_id{1};
std::atomic<std::uint64_t> g_next_handle{1};

struct WatchEntry {
    std::vector<std::string>      keys;
    Client::EventCallback         cb;
};

} // namespace

// ─────────────────────────────── Impl ───────────────────────────────

class Client::Impl {
public:
    ACE_LSOCK_Stream     stream;
    std::atomic<bool>    connected{false};

    std::thread          listener;
    std::atomic<bool>    stop_listener{false};

    // id → pending promise (fulfilled by the listener thread).
    std::mutex                                            pending_mtx;
    std::unordered_map<std::uint64_t, std::promise<json>> pending;

    // Welcome line bookkeeping.
    std::mutex                  welcome_mtx;
    std::condition_variable     welcome_cv;
    std::string                 welcome_line;
    bool                        welcome_received = false;

    // Pull-style event queue.
    std::mutex                  events_mtx;
    std::condition_variable     events_cv;
    std::deque<Event>           events;

    // Callback-style watches + per-key local refcount.
    std::mutex                                              watches_mtx;
    std::unordered_map<WatchHandle, WatchEntry>             watches;
    std::unordered_map<std::string, std::size_t>            key_refcount;

    Status send_json(const std::string& body);
    Status round_trip(json& req, json& resp, std::int32_t timeout_ms);
    void   run_listener();
    void   fail_all_pending(const std::string& why);
};

Status Client::Impl::send_json(const std::string& body) {
    std::string line = body + "\n";
    ssize_t n = stream.send_n(line.data(), line.size());
    if (n < 0 || static_cast<std::size_t>(n) != line.size()) {
        return sys_err("send_n");
    }
    return {};
}

Status Client::Impl::round_trip(json& req, json& resp,
                                std::int32_t timeout_ms) {
    if (!connected.load()) return protocol_err("not connected");

    const std::uint64_t id = g_next_id.fetch_add(1);
    req["id"] = id;

    std::promise<json>  prom;
    std::future<json>   fut = prom.get_future();
    {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.emplace(id, std::move(prom));
    }

    auto ss = send_json(req.dump());
    if (!ss.ok) {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.erase(id);
        return ss;
    }

    using namespace std::chrono;
    if (fut.wait_for(milliseconds(timeout_ms)) != std::future_status::ready) {
        std::lock_guard<std::mutex> g(pending_mtx);
        pending.erase(id);
        Status s; s.ok=false; s.code=ETIMEDOUT;
        s.err = "request timeout (id=" + std::to_string(id) + ")";
        return s;
    }

    try {
        resp = fut.get();
    } catch (const std::exception& e) {
        return protocol_err(std::string("promise broken: ") + e.what());
    }
    return {};
}

void Client::Impl::run_listener() {
    std::string buf;
    char        chunk[1024];

    while (!stop_listener.load()) {
        ACE_Time_Value timeout(0, 200 * 1000);
        ssize_t n = stream.recv(chunk, sizeof(chunk), &timeout);
        if (n < 0) {
            if (errno == ETIME || errno == ETIMEDOUT || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break;     // EOF

        buf.append(chunk, static_cast<std::size_t>(n));

        for (;;) {
            auto pos = buf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;

            json p;
            try { p = json::parse(line); } catch (...) { continue; }

            // Welcome (server's first line — `hello` field).
            {
                std::lock_guard<std::mutex> g(welcome_mtx);
                if (!welcome_received && p.contains("hello")) {
                    welcome_line     = line + "\n";
                    welcome_received = true;
                    welcome_cv.notify_all();
                    continue;
                }
            }

            // Notify push.
            if (p.contains("ev")) {
                Event ev;
                ev.key   = p.value("k", "");
                ev.value = p.value("v", "");
                if (p.contains("prev") && !p["prev"].is_null()) {
                    ev.prev           = p["prev"].get<std::string>();
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
                    for (const auto& [h, w] : watches) {
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

            // Response: ok + id → fulfill promise.
            if (p.contains("ok") && p.contains("id")) {
                std::uint64_t id = p["id"].get<std::uint64_t>();
                std::promise<json> prom;
                bool found = false;
                {
                    std::lock_guard<std::mutex> g(pending_mtx);
                    auto it = pending.find(id);
                    if (it != pending.end()) {
                        prom  = std::move(it->second);
                        pending.erase(it);
                        found = true;
                    }
                }
                if (found) {
                    try { prom.set_value(std::move(p)); } catch (...) {}
                }
            }
        }
    }

    fail_all_pending("listener exited");

    {
        std::lock_guard<std::mutex> g(welcome_mtx);
        welcome_received = true;
        welcome_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> g(events_mtx);
        events_cv.notify_all();
    }
}

void Client::Impl::fail_all_pending(const std::string& why) {
    std::unordered_map<std::uint64_t, std::promise<json>> snapshot;
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
    m_impl->connected.store(true);
    m_impl->stop_listener.store(false);
    m_impl->listener = std::thread([impl = m_impl.get()]() {
        impl->run_listener();
    });
    return {};
}

Status Client::recv_welcome(std::string& out, std::int32_t timeout_ms) {
    if (!m_impl->connected.load()) return protocol_err("not connected");
    std::unique_lock<std::mutex> g(m_impl->welcome_mtx);
    if (!m_impl->welcome_cv.wait_for(g,
            std::chrono::milliseconds(timeout_ms),
            [&]() { return m_impl->welcome_received; })) {
        Status s; s.ok=false; s.code=ETIMEDOUT;
        s.err = "welcome timeout"; return s;
    }
    if (m_impl->welcome_line.empty()) {
        return protocol_err("connection closed before welcome");
    }
    out = m_impl->welcome_line;
    return {};
}

Status Client::set(const std::vector<KV>& pairs, std::int32_t timeout_ms) {
    json req;
    req["op"]   = "set";
    req["keys"] = json::array();
    for (const auto& kv : pairs) {
        json e; e["k"] = kv.first; e["v"] = kv.second;
        req["keys"].push_back(e);
    }
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) return rs;
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "set failed"));
    }
    return {};
}

Status Client::get(const std::vector<std::string>& keys,
                   std::vector<GetResult>&         out,
                   std::int32_t                    timeout_ms) {
    out.clear();
    json req;
    req["op"]   = "get";
    req["keys"] = keys;
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) return rs;
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "get failed"));
    }
    if (!resp.contains("data") || !resp["data"].is_array()) {
        return protocol_err("get response missing data array");
    }
    for (const auto& item : resp["data"]) {
        GetResult g;
        g.key = item.value("k", "");
        if (item.contains("v") && !item["v"].is_null()) {
            g.value     = item["v"].get<std::string>();
            g.has_value = true;
        }
        out.push_back(std::move(g));
    }
    return {};
}

Status Client::watch(const std::vector<std::string>& keys,
                     std::int32_t                    timeout_ms) {
    // Pull-style: bump local refcount, only send wire `register`
    // for keys that just transitioned 0 → 1.
    std::vector<std::string> wireKeys;
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        for (const auto& k : keys) {
            if (m_impl->key_refcount[k]++ == 0) wireKeys.push_back(k);
        }
    }
    if (wireKeys.empty()) return {};

    json req;
    req["op"]   = "register";
    req["keys"] = wireKeys;
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        for (const auto& k : wireKeys) {
            auto it = m_impl->key_refcount.find(k);
            if (it != m_impl->key_refcount.end() && it->second > 0) {
                if (--it->second == 0) m_impl->key_refcount.erase(it);
            }
        }
        return rs;
    }
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "watch failed"));
    }
    return {};
}

Status Client::watch(const std::vector<std::string>& keys,
                     EventCallback                   cb,
                     WatchHandle*                    out_handle,
                     std::int32_t                    timeout_ms) {
    if (!cb) return protocol_err("watch: callback is null");

    auto pull = watch(keys, timeout_ms);
    if (!pull.ok) return pull;

    WatchHandle h = g_next_handle.fetch_add(1);
    {
        std::lock_guard<std::mutex> g(m_impl->watches_mtx);
        WatchEntry w;
        w.keys = keys;
        w.cb   = std::move(cb);
        m_impl->watches.emplace(h, std::move(w));
    }
    if (out_handle) *out_handle = h;
    return {};
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
    req["op"]   = "remove";
    req["keys"] = wireKeys;
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) return rs;
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "unwatch failed"));
    }
    return {};
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
    if (m_impl->connected.exchange(false)) {
        m_impl->stop_listener.store(true);
        ::shutdown(m_impl->stream.get_handle(), SHUT_RDWR);
        if (m_impl->listener.joinable()) m_impl->listener.join();
        m_impl->stream.close();
        m_impl->fail_all_pending("client closed");
        {
            std::lock_guard<std::mutex> g(m_impl->welcome_mtx);
            m_impl->welcome_received = true;
            m_impl->welcome_cv.notify_all();
        }
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
}

int Client::fd() const {
    if (!m_impl || !m_impl->connected.load()) return -1;
    return const_cast<ACE_LSOCK_Stream&>(m_impl->stream).get_handle();
}

} // namespace data_store
