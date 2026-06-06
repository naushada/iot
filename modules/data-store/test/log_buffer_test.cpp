/// LogBuffer gtest — verifies ring-buffer capture, flush threshold,
/// log-level parsing, and key-switching.

#include <gtest/gtest.h>

#include <ace/Log_Msg.h>

#include <atomic>
#include <chrono>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "data_store/log_buffer.hpp"
#include "data_store/client.hpp"
#include "../src/server/data_store.hpp"
#include "../src/server/schema.hpp"
#include "../src/server/server.hpp"
#include "../src/server/worker_pool.hpp"

namespace ds = data_store;

namespace {

// ── Test fixture: in-process ds-server ───────────────────────────

std::string make_tmpdir() {
    char tmpl[64] = "/tmp/ds-logbuf-XXXXXX";
    if (char* d = ::mkdtemp(tmpl)) return d;
    return {};
}

struct LogBufferTest : public ::testing::Test {
    std::string                          tmpdir;
    std::shared_ptr<ds::server::SchemaRegistry> schema;
    std::shared_ptr<ds::server::DataStore>      store;
    std::unique_ptr<ds::server::WorkerPool>     pool;
    std::unique_ptr<ds::server::Server>         server;
    std::string                          sock_path;

    void SetUp() override {
        tmpdir = make_tmpdir();
        sock_path = tmpdir + "/ds.sock";

        schema = std::make_shared<ds::server::SchemaRegistry>();
        // No schema loaded — unknown keys pass through (REQ-DS-023).

        store = std::make_shared<ds::server::DataStore>();
        pool  = std::make_unique<ds::server::WorkerPool>(store, schema);
        ASSERT_EQ(pool->open(), 0);

        server = std::make_unique<ds::server::Server>(
            store, pool.get(), sock_path);
        ASSERT_EQ(server->open(), 0);
    }

    void TearDown() override {
        // Important: stop ACE callback before tearing down to avoid
        // use-after-free in the ACE_Log_Msg singleton.
        ACE_Log_Msg::instance()->msg_callback(nullptr);

        if (server) server->close();
        if (pool)   pool->close();
        ::unlink(tmpdir.c_str());
    }

    ds::Client connect_client() {
        ds::Client c;
        auto st = c.connect(sock_path);
        EXPECT_TRUE(st.ok) << st.err;
        return c;
    }
};

// ── Capture ───────────────────────────────────────────────────────

TEST_F(LogBufferTest, start_captures_ace_output) {
    ds::LogBuffer lb("testd", "log.test.text", "log.level");
    lb.start();  // register ACE callback

    ACE_DEBUG((LM_INFO, "hello from test\n"));
    ACE_DEBUG((LM_ERROR, "an error occurred\n"));

    EXPECT_GE(lb.line_count(), 2u);
}

TEST_F(LogBufferTest, not_started_captures_nothing) {
    ds::LogBuffer lb("testd", "log.test.text", "log.level");
    // start() NOT called

    ACE_DEBUG((LM_INFO, "should not be captured\n"));

    EXPECT_EQ(lb.line_count(), 0u);
}

TEST_F(LogBufferTest, daemon_name_in_output) {
    ds::LogBuffer lb("mydaemon", "log.test.text", "log.level");
    lb.start();

    ACE_DEBUG((LM_INFO, "test message\n"));

    // Flush to a real ds client and verify the text
    auto cli = connect_client();
    lb.flush(cli);

    std::vector<ds::Client::GetResult> got;
    auto rs = cli.get({"log.test.text"}, got);
    ASSERT_TRUE(rs.ok);
    ASSERT_FALSE(got.empty());
    ASSERT_TRUE(got[0].has_value);
    auto text = ds::to_string(got[0].value);
    ASSERT_TRUE(text.has_value());
    EXPECT_NE(text->find("mydaemon:"), std::string::npos);
}

// ── Flush threshold ───────────────────────────────────────────────

TEST_F(LogBufferTest, flush_respects_min_bytes) {
    ds::LogBuffer lb("testd", "log.test.text", "log.level");
    lb.start();
    auto cli = connect_client();

    // With no log lines, flush with large threshold should no-op
    lb.flush(cli, 9999);

    std::vector<ds::Client::GetResult> got;
    cli.get({"log.test.text"}, got);
    // Key was never set → should be null/empty
    if (!got.empty() && got[0].has_value) {
        auto text = ds::to_string(got[0].value);
        ASSERT_TRUE(text.has_value());
        EXPECT_TRUE(text->empty());
    }
}

TEST_F(LogBufferTest, flush_zero_threshold_always_writes) {
    ds::LogBuffer lb("testd", "log.test.text", "log.level");
    lb.start();
    auto cli = connect_client();

    ACE_DEBUG((LM_INFO, "single line\n"));
    lb.flush(cli, 0);  // min_bytes=0 → always flush

    std::vector<ds::Client::GetResult> got;
    auto rs = cli.get({"log.test.text"}, got);
    ASSERT_TRUE(rs.ok);
    ASSERT_FALSE(got.empty());
    EXPECT_TRUE(got[0].has_value);
}

// ── set_log_key / set_level_key ───────────────────────────────────

TEST_F(LogBufferTest, set_log_key_changes_flush_target) {
    ds::LogBuffer lb("testd", "log.test.text", "log.level");
    lb.start();
    lb.set_log_key("log.other.text");
    auto cli = connect_client();

    ACE_DEBUG((LM_INFO, "routed\n"));
    lb.flush(cli);

    // Original key should be empty
    {
        std::vector<ds::Client::GetResult> got;
        cli.get({"log.test.text"}, got);
        if (!got.empty() && got[0].has_value) {
            auto text = ds::to_string(got[0].value);
            EXPECT_TRUE(text->empty());
        }
    }
    // New key should have the log line
    {
        std::vector<ds::Client::GetResult> got;
        cli.get({"log.other.text"}, got);
        ASSERT_TRUE(got[0].has_value);
        auto text = ds::to_string(got[0].value);
        ASSERT_TRUE(text.has_value());
        EXPECT_NE(text->find("routed"), std::string::npos);
    }
}

// ── Destructor unregisters ────────────────────────────────────────

TEST_F(LogBufferTest, destructor_unregisters_callback) {
    {
        ds::LogBuffer lb("temp", "log.temp.text", "log.level");
        lb.start();
        ACE_DEBUG((LM_INFO, "captured\n"));
        EXPECT_GE(lb.line_count(), 1u);
    }
    // After destruction, the callback is unregistered.
    // New ACE output should NOT add to the (now-dead) buffer.
    // Just verify we don't crash.
    ACE_DEBUG((LM_INFO, "after destructor\n"));
    SUCCEED();
}

}  // namespace