#include "data_store/client.hpp"

#include "data_store/proto.hpp"

#include <cerrno>
#include <cstring>
#include <sys/un.h>

#include <ace/LSOCK_Connector.h>
#include <ace/LSOCK_Stream.h>
#include <ace/Time_Value.h>
#include <ace/UNIX_Addr.h>

namespace data_store {

namespace {

Status sys_err(const std::string& what, int code = errno) {
    Status s;
    s.ok = false;
    s.code = code;
    s.err = what + ": " + std::strerror(code);
    return s;
}

} // namespace

// --- pimpl --------------------------------------------------------------
//
// Hidden behind the header so the public ABI stays free of ACE types.
// The Impl owns the ACE_LSOCK_Stream for the lifetime of the Client.

class Client::Impl {
public:
    ACE_LSOCK_Stream stream;
    bool             connected = false;
};

// --- ctors/dtor ---------------------------------------------------------

Client::Client() : m_impl(std::make_unique<Impl>()) {}
Client::~Client() { close(); }

Client::Client(Client&&) noexcept            = default;
Client& Client::operator=(Client&&) noexcept = default;

// --- operations ---------------------------------------------------------

Status Client::connect(std::string path) {
    if (path.empty()) path = proto::kDefaultSocketPath;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        Status s; s.ok = false; s.err = "socket path too long: " + path;
        return s;
    }

    close();

    ACE_UNIX_Addr        addr(path.c_str());
    ACE_LSOCK_Connector  connector;
    ACE_Time_Value       timeout(5, 0);                 // 5 s hard cap

    if (connector.connect(m_impl->stream, addr, &timeout) == -1) {
        return sys_err("ACE_LSOCK_Connector::connect(" + path + ")");
    }
    m_impl->connected = true;
    return {};
}

Status Client::recv_welcome(std::string& out, std::int32_t timeout_ms) {
    if (!m_impl->connected) {
        Status s; s.ok = false; s.err = "not connected"; return s;
    }

    char buf[256];
    ACE_Time_Value timeout(timeout_ms / 1000,
                           (timeout_ms % 1000) * 1000);
    ssize_t n = m_impl->stream.recv(buf, sizeof(buf), &timeout);
    if (n < 0) {
        // ACE_LSOCK_Stream::recv with a non-null timeout returns -1
        // with errno=ETIME (or ETIMEDOUT on some glibc paths). Normalise.
        if (errno == ETIME || errno == ETIMEDOUT) {
            Status s; s.ok = false; s.code = ETIMEDOUT;
            s.err = "welcome timeout";
            return s;
        }
        return sys_err("ACE_LSOCK_Stream::recv");
    }
    if (n == 0) {
        Status s; s.ok = false; s.err = "server closed before welcome";
        return s;
    }
    out.assign(buf, static_cast<std::size_t>(n));
    return {};
}

void Client::close() {
    if (m_impl && m_impl->connected) {
        m_impl->stream.close();
        m_impl->connected = false;
    }
}

int Client::fd() const {
    if (!m_impl || !m_impl->connected) return -1;
    return const_cast<ACE_LSOCK_Stream&>(m_impl->stream).get_handle();
}

} // namespace data_store
