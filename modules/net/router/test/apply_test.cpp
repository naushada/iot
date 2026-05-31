/// Tests for the nft apply wrapper. Uses a real fake-nft shell
/// script written to a tempdir so we exercise the popen() +
/// tempfile + capture path without touching real nft.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "apply.hpp"

using net_router::apply::default_nft_apply;

namespace {

/// Owns a tempdir + fake-nft script for the duration of a TEST.
class FakeNft {
public:
    FakeNft() {
        char templ[] = "/tmp/iot-apply-test.XXXXXX";
        if (!::mkdtemp(templ)) std::abort();
        m_dir = templ;
        m_path = m_dir + "/nft";
    }

    ~FakeNft() {
        ::unlink(m_path.c_str());
        ::unlink((m_dir + "/last_input").c_str());
        ::rmdir(m_dir.c_str());
    }

    /// Writes a `#!/bin/sh` script that copies its `-f <file>` arg
    /// to <dir>/last_input and exits `rc`. If `stderr_msg` non-empty,
    /// echoes it to stderr first.
    void install(int rc, const std::string& stderr_msg = {}) {
        std::ofstream f(m_path);
        f << "#!/bin/sh\n";
        if (!stderr_msg.empty()) {
            // Escape single quotes for shell.
            std::string esc;
            for (char c : stderr_msg) {
                if (c == '\'') esc += "'\\''"; else esc.push_back(c);
            }
            f << "printf '%s\\n' '" << esc << "' 1>&2\n";
        }
        // $1 = "-f", $2 = "<tempfile>"
        f << "if [ \"$1\" = \"-f\" ] && [ -n \"$2\" ]; then\n";
        f << "  cp \"$2\" '" << m_dir << "/last_input'\n";
        f << "fi\n";
        f << "exit " << rc << "\n";
        f.close();
        ::chmod(m_path.c_str(), 0755);
    }

    std::string read_last_input() const {
        std::ifstream f(m_dir + "/last_input");
        if (!f) return {};
        std::string out((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        return out;
    }

    const std::string& path() const { return m_path; }

private:
    std::string m_dir;
    std::string m_path;
};

} // namespace

TEST(NftApply, SuccessfulApplyReturnsTrueAndDeliversRulesetIntact) {
    FakeNft fn;
    fn.install(/*rc=*/0);

    auto fn_apply = default_nft_apply(fn.path());
    const std::string ruleset =
        "table inet iot_router {\n"
        "  chain prerouting { type nat hook prerouting priority -100; }\n"
        "}\n";

    std::string err;
    EXPECT_TRUE(fn_apply(ruleset, &err));
    EXPECT_TRUE(err.empty());
    // The fake nft saved its `-f` input — must be byte-identical.
    EXPECT_EQ(ruleset, fn.read_last_input());
}

TEST(NftApply, NonZeroExitYieldsFalseAndCapturesStderr) {
    FakeNft fn;
    fn.install(/*rc=*/2, "iot-router:5: syntax error");

    auto fn_apply = default_nft_apply(fn.path());
    std::string err;
    EXPECT_FALSE(fn_apply("garbage ruleset", &err));
    // The wrapper combines stdout+stderr; the fake wrote only to stderr.
    EXPECT_NE(std::string::npos, err.find("syntax error"));
}

TEST(NftApply, MissingBinaryReturnsFalseWithDiagnostic) {
    auto fn_apply = default_nft_apply("/usr/bin/definitely-not-a-tool");
    std::string err;
    EXPECT_FALSE(fn_apply("any ruleset", &err));
    EXPECT_FALSE(err.empty());  // popen succeeds; shell reports not-found
}

TEST(NftApply, NullErrPointerIsTolerated) {
    FakeNft fn;
    fn.install(/*rc=*/3);
    auto fn_apply = default_nft_apply(fn.path());
    EXPECT_FALSE(fn_apply("x", nullptr));  // must not crash
}

TEST(NftApply, TempfileIsCleanedUpAfterApply) {
    FakeNft fn;
    fn.install(/*rc=*/0);
    auto fn_apply = default_nft_apply(fn.path());
    fn_apply("some ruleset", nullptr);

    // Walk the tmpdir(s) we'd use — the iot-nft.XXXXXX pattern from
    // pick_tmpdir() should leave nothing behind.
    const std::string dirs[] = {"/tmp", "."};
    for (const auto& d : dirs) {
        std::string cmd = "ls " + d + "/iot-nft.* 2>/dev/null | wc -l";
        std::FILE* p = ::popen(cmd.c_str(), "r");
        ASSERT_TRUE(p);
        char buf[16] = {0};
        std::fread(buf, 1, sizeof(buf) - 1, p);
        ::pclose(p);
        EXPECT_EQ('0', buf[0]) << "leftover iot-nft.* in " << d;
    }
}
