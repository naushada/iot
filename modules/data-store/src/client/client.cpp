#include "data_store/client.hpp"

#include "data_store/proto.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace data_store {

namespace {

Status sys_err(const std::string& what) {
    Status s;
    s.ok = false;
    s.code = errno;
    s.err = what + ": " + std::strerror(s.code);
    return s;
}

} // namespace

Client::Client() = default;

Client::~Client() { close(); }

Client::Client(Client&& other) noexcept : m_fd(other.m_fd) {
    other.m_fd = -1;
}

Client& Client::operator=(Client&& other) noexcept {
    if (&other != this) {
        close();
        m_fd       = other.m_fd;
        other.m_fd = -1;
    }
    return *this;
}

void Client::close() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

Status Client::connect(std::string path) {
    if (path.empty()) path = proto::kDefaultSocketPath;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        Status s;
        s.ok = false;
        s.err = "socket path too long: " + path;
        return s;
    }

    close();

    m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) return sys_err("socket()");

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Status s = sys_err("connect(" + path + ")");
        close();
        return s;
    }
    return {};
}

Status Client::recv_welcome(std::string& out, std::int32_t timeout_ms) {
    if (m_fd < 0) {
        Status s; s.ok = false; s.err = "not connected"; return s;
    }

    pollfd pfd{m_fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) {
        Status s; s.ok = false; s.code = ETIMEDOUT;
        s.err = "welcome timeout";
        return s;
    }
    if (pr < 0) return sys_err("poll()");

    // The welcome line is short (well under 256 B). Read until '\n'
    // or the server closes — for D1 the server closes immediately
    // after the welcome so we cap at one buffer.
    char buf[256];
    ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
    if (n < 0) return sys_err("recv()");
    if (n == 0) {
        Status s; s.ok = false; s.err = "server closed before welcome";
        return s;
    }
    out.assign(buf, static_cast<std::size_t>(n));
    return {};
}

} // namespace data_store
