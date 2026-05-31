#include "apply.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace net_router::apply {

namespace {

/// Locate a writable directory for the tempfile. Mirrors the L12/D5
/// $TMPDIR → /tmp → cwd fallback chain we use for ovpn keys.
std::string pick_tmpdir() {
    if (const char* env = std::getenv("TMPDIR")) {
        struct stat st{};
        if (stat(env, &st) == 0 && S_ISDIR(st.st_mode)) return env;
    }
    {
        struct stat st{};
        if (stat("/tmp", &st) == 0 && S_ISDIR(st.st_mode)) return "/tmp";
        if (mkdir("/tmp", 0777) == 0) return "/tmp";
    }
    return ".";
}

/// Shell-quote for /bin/sh -c — single-quote wrap, escape inner '.
std::string shquote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

bool write_all(int fd, const std::string& s) {
    const char* p = s.data();
    std::size_t left = s.size();
    while (left) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p    += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace

NftApply default_nft_apply(const std::string& nft_path) {
    return [nft_path](const std::string& ruleset, std::string* err) -> bool {
        auto dir = pick_tmpdir();
        std::string templ = dir + "/iot-nft.XXXXXX";
        std::vector<char> buf(templ.begin(), templ.end());
        buf.push_back('\0');

        int fd = ::mkstemp(buf.data());
        if (fd < 0) {
            if (err) *err = std::string("mkstemp failed: ") + std::strerror(errno);
            return false;
        }
        const std::string path(buf.data());
        bool wrote = write_all(fd, ruleset);
        ::close(fd);
        if (!wrote) {
            if (err) *err = "short write to " + path;
            ::unlink(path.c_str());
            return false;
        }

        // Combined output capture: `nft -f <path> 2>&1` through popen.
        const std::string cmd = shquote(nft_path) + " -f " + shquote(path) + " 2>&1";
        std::FILE* fp = ::popen(cmd.c_str(), "r");
        if (!fp) {
            if (err) *err = "popen(nft) failed";
            ::unlink(path.c_str());
            return false;
        }
        std::string captured;
        std::array<char, 4096> chunk{};
        std::size_t n = 0;
        while ((n = std::fread(chunk.data(), 1, chunk.size(), fp)) > 0) {
            captured.append(chunk.data(), n);
        }
        int status = ::pclose(fp);
        ::unlink(path.c_str());

        const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
        if (rc != 0) {
            if (err) *err = captured.empty()
                ? ("nft exited " + std::to_string(rc))
                : captured;
            return false;
        }
        return true;
    };
}

} // namespace net_router::apply
