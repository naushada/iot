#include "data_store/client.hpp"

#include "data_store/proto.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/un.h>

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

} // namespace

// --- pimpl --------------------------------------------------------------

class Client::Impl {
public:
    ACE_LSOCK_Stream stream;
    std::string      recvBuf;
    bool             connected = false;

    Status recv_one_line(std::string& out, std::int32_t timeout_ms);
    Status send_json(const std::string& req);
    Status round_trip(json& req, json& resp, std::int32_t timeout_ms);
};

Status Client::Impl::recv_one_line(std::string& out, std::int32_t timeout_ms) {
    // Service from recvBuf first.
    auto pos = recvBuf.find('\n');
    if (pos != std::string::npos) {
        out = recvBuf.substr(0, pos);
        recvBuf.erase(0, pos + 1);
        return {};
    }

    char buf[1024];
    ACE_Time_Value timeout(timeout_ms / 1000,
                           (timeout_ms % 1000) * 1000);
    for (;;) {
        ssize_t n = stream.recv(buf, sizeof(buf), &timeout);
        if (n < 0) {
            if (errno == ETIME || errno == ETIMEDOUT) {
                Status s; s.ok=false; s.code=ETIMEDOUT;
                s.err = "recv timeout"; return s;
            }
            return sys_err("recv");
        }
        if (n == 0) return protocol_err("server closed");
        recvBuf.append(buf, static_cast<std::size_t>(n));
        pos = recvBuf.find('\n');
        if (pos != std::string::npos) {
            out = recvBuf.substr(0, pos);
            recvBuf.erase(0, pos + 1);
            return {};
        }
        // No newline yet — loop and read more (timeout applies per
        // recv call; cumulative wait may exceed it slightly, which
        // is fine for our use case).
    }
}

Status Client::Impl::send_json(const std::string& req) {
    std::string line = req + "\n";
    ssize_t n = stream.send_n(line.data(), line.size());
    if (n < 0 || static_cast<std::size_t>(n) != line.size()) {
        return sys_err("send_n");
    }
    return {};
}

Status Client::Impl::round_trip(json& req, json& resp,
                                std::int32_t timeout_ms) {
    const std::uint64_t id = g_next_id.fetch_add(1);
    req["id"] = id;
    auto ss = send_json(req.dump());
    if (!ss.ok) return ss;

    // Read lines until we find one with our id. Skip notify pushes
    // (`ev=changed`) so the caller's set/get/watch ack arrives
    // without interference. Callers needing pushes should use
    // recv_event AFTER the round-trip completes.
    for (;;) {
        std::string line;
        auto rs = recv_one_line(line, timeout_ms);
        if (!rs.ok) return rs;
        json p;
        try { p = json::parse(line); }
        catch (const std::exception& e) {
            return protocol_err(std::string("bad response json: ") + e.what());
        }
        if (p.contains("ev")) continue;     // push — ignore here
        if (!p.contains("ok")) {
            return protocol_err("response missing ok");
        }
        if (p.contains("id") && p["id"] != id) continue;  // mismatched
        resp = std::move(p);
        return {};
    }
}

// --- Client -------------------------------------------------------------

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
    m_impl->connected = true;
    return {};
}

Status Client::recv_welcome(std::string& out, std::int32_t timeout_ms) {
    if (!m_impl->connected) return protocol_err("not connected");
    std::string line;
    auto rs = m_impl->recv_one_line(line, timeout_ms);
    if (!rs.ok) return rs;
    out = line + "\n";   // restore newline for byte-equality tests
    return {};
}

Status Client::recv_line(std::string& out, std::int32_t timeout_ms) {
    if (!m_impl->connected) return protocol_err("not connected");
    return m_impl->recv_one_line(out, timeout_ms);
}

Status Client::set(const std::vector<KV>& pairs, std::int32_t timeout_ms) {
    if (!m_impl->connected) return protocol_err("not connected");
    json req;
    req["op"]   = "set";
    req["keys"] = json::array();
    for (const auto& kv : pairs) {
        json e;
        e["k"] = kv.first;
        e["v"] = kv.second;
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
    if (!m_impl->connected) return protocol_err("not connected");
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
    if (!m_impl->connected) return protocol_err("not connected");
    json req;
    req["op"]   = "register";
    req["keys"] = keys;
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) return rs;
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "watch failed"));
    }
    return {};
}

Status Client::unwatch(const std::vector<std::string>& keys,
                       std::int32_t                    timeout_ms) {
    if (!m_impl->connected) return protocol_err("not connected");
    json req;
    req["op"]   = "remove";
    req["keys"] = keys;
    json resp;
    auto rs = m_impl->round_trip(req, resp, timeout_ms);
    if (!rs.ok) return rs;
    if (!resp.value("ok", false)) {
        return protocol_err(resp.value("err", "unwatch failed"));
    }
    return {};
}

Status Client::recv_event(Event& out, std::int32_t timeout_ms) {
    if (!m_impl->connected) return protocol_err("not connected");
    std::string line;
    auto rs = m_impl->recv_one_line(line, timeout_ms);
    if (!rs.ok) return rs;
    json p;
    try { p = json::parse(line); }
    catch (const std::exception& e) {
        return protocol_err(std::string("bad event json: ") + e.what());
    }
    if (!p.contains("ev")) return protocol_err("not an event");
    out.key   = p.value("k", "");
    out.value = p.value("v", "");
    if (p.contains("prev") && !p["prev"].is_null()) {
        out.prev           = p["prev"].get<std::string>();
        out.prev_has_value = true;
    } else {
        out.prev_has_value = false;
    }
    return {};
}

void Client::close() {
    if (m_impl && m_impl->connected) {
        m_impl->stream.close();
        m_impl->connected = false;
        m_impl->recvBuf.clear();
    }
}

int Client::fd() const {
    if (!m_impl || !m_impl->connected) return -1;
    return const_cast<ACE_LSOCK_Stream&>(m_impl->stream).get_handle();
}

} // namespace data_store
