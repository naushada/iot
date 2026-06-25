#include "lwm2m_durable_sample_buffer.hpp"

#include <cmath>       // llround
#include <stdexcept>
#include <string>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>

namespace lwm2m { namespace telemetry {

namespace {

// Sample <-> compact JSON body. {"t":<unix secs>,"v":[["name",val],…]}.
std::string serialize(const Sample& s) {
    nlohmann::json j;
    j["t"] = s.timeUnix;
    auto& v = j["v"] = nlohmann::json::array();
    for (const auto& kv : s.values) v.push_back({kv.first, kv.second});
    return j.dump();
}

bool deserialize(const std::string& body, Sample& out) {
    try {
        auto j = nlohmann::json::parse(body);
        out.timeUnix = j.value("t", 0.0);
        out.values.clear();
        if (j.contains("v") && j["v"].is_array()) {
            for (const auto& p : j["v"]) {
                if (p.is_array() && p.size() == 2 && p[0].is_string())
                    out.values.emplace_back(p[0].get<std::string>(),
                                            p[1].get<double>());
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string join_seqs(const std::vector<std::int64_t>& seqs) {
    std::string s;
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        if (i) s.push_back(',');
        s += std::to_string(seqs[i]);
    }
    return s;
}

} // namespace

DurableSampleBuffer::DurableSampleBuffer(const std::string& path,
                                         std::size_t cap,
                                         std::int64_t ttlSeconds)
    : m_cap(cap ? cap : 1), m_ttl(ttlSeconds) {
    if (sqlite3_open_v2(path.c_str(), &m_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        std::string msg = m_db ? sqlite3_errmsg(m_db) : "open failed";
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        throw std::runtime_error("DurableSampleBuffer: " + msg);
    }
    // WAL = crash-safe + concurrent read; NORMAL fsyncs on checkpoint, not every
    // txn (kind to flash). Bootstrap the outbox; re-arm any row left in-flight
    // (sent=1) by a crash → at-least-once redelivery.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("CREATE TABLE IF NOT EXISTS outbox("
         "  seq  INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  ts   INTEGER NOT NULL,"     // epoch ms (timeUnix*1000), for TTL
         "  body BLOB    NOT NULL,"     // serialized Sample
         "  sent INTEGER NOT NULL DEFAULT 0);");
    exec("CREATE INDEX IF NOT EXISTS ix_unsent ON outbox(seq) WHERE sent=0;");
    exec("UPDATE outbox SET sent=0 WHERE sent=1;");   // crash recovery
}

DurableSampleBuffer::~DurableSampleBuffer() {
    if (m_db) sqlite3_close(m_db);
}

void DurableSampleBuffer::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error("DurableSampleBuffer: " + msg);
    }
}

std::int64_t DurableSampleBuffer::count_pending() const {
    sqlite3_stmt* st = nullptr;
    std::int64_t c = 0;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM outbox WHERE sent=0;",
                           -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) c = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return c;
}

void DurableSampleBuffer::push(const Sample& s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    const std::string body = serialize(s);
    const std::int64_t ts  = static_cast<std::int64_t>(std::llround(s.timeUnix * 1000.0));

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "INSERT INTO outbox(ts,body) VALUES(?1,?2);",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, ts);
        sqlite3_bind_blob(st, 2, body.data(),
                          static_cast<int>(body.size()), SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    // Bound the PENDING set: evict oldest over cap (newest-wins), like the RAM
    // buffer. Leased (in-flight) rows are sent=1 → not counted, not evicted.
    const std::int64_t pending = count_pending();
    if (pending > static_cast<std::int64_t>(m_cap)) {
        const std::int64_t excess = pending - static_cast<std::int64_t>(m_cap);
        exec(("DELETE FROM outbox WHERE seq IN (SELECT seq FROM outbox "
              "WHERE sent=0 ORDER BY seq ASC LIMIT " +
              std::to_string(excess) + ");").c_str());
        m_dropped += static_cast<std::size_t>(sqlite3_changes(m_db));
    }
}

std::size_t DurableSampleBuffer::size() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return static_cast<std::size_t>(count_pending());
}

bool DurableSampleBuffer::empty() const { return size() == 0; }

std::size_t DurableSampleBuffer::dropped() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_dropped;
}

std::vector<Sample> DurableSampleBuffer::take(std::size_t n) {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<Sample> out;
    if (n == 0) return out;

    std::vector<std::int64_t> seqs;
    std::vector<std::int64_t> poison;   // un-deserializable rows → drop
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "SELECT seq,body FROM outbox WHERE sent=0 ORDER BY seq ASC LIMIT ?1;",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(n));
        while (sqlite3_step(st) == SQLITE_ROW) {
            const std::int64_t seq = sqlite3_column_int64(st, 0);
            const void* blob = sqlite3_column_blob(st, 1);
            const int   len  = sqlite3_column_bytes(st, 1);
            Sample s;
            if (blob && deserialize(std::string(static_cast<const char*>(blob),
                                                static_cast<std::size_t>(len)), s)) {
                out.push_back(std::move(s));
                seqs.push_back(seq);
            } else {
                poison.push_back(seq);
            }
        }
    }
    sqlite3_finalize(st);

    if (!poison.empty()) {                // never re-select a corrupt row
        exec(("DELETE FROM outbox WHERE seq IN (" + join_seqs(poison) + ");").c_str());
        m_dropped += poison.size();
    }
    if (!seqs.empty()) {                  // lease the batch (keep rows for crash-safety)
        exec(("UPDATE outbox SET sent=1 WHERE seq IN (" + join_seqs(seqs) + ");").c_str());
        m_leased = std::move(seqs);
    }
    return out;
}

void DurableSampleBuffer::requeue(std::vector<Sample>&& batch) {
    std::lock_guard<std::mutex> lk(m_mtx);
    batch.clear();                        // rows live in the DB; arg is ignored
    if (m_leased.empty()) return;
    // Un-lease: sent=1 → 0. seqs unchanged → next take() re-selects them first
    // (oldest-first retry preserved).
    exec(("UPDATE outbox SET sent=0 WHERE seq IN (" + join_seqs(m_leased) + ");").c_str());
    m_leased.clear();
    // The buffer may have filled with newer samples while this batch was in
    // flight; honour cap (newest-wins) — may drop the just-requeued oldest.
    const std::int64_t pending = count_pending();
    if (pending > static_cast<std::int64_t>(m_cap)) {
        const std::int64_t excess = pending - static_cast<std::int64_t>(m_cap);
        exec(("DELETE FROM outbox WHERE seq IN (SELECT seq FROM outbox "
              "WHERE sent=0 ORDER BY seq ASC LIMIT " +
              std::to_string(excess) + ");").c_str());
        m_dropped += static_cast<std::size_t>(sqlite3_changes(m_db));
    }
}

void DurableSampleBuffer::commit() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_leased.empty()) return;
    exec(("DELETE FROM outbox WHERE seq IN (" + join_seqs(m_leased) + ");").c_str());
    m_leased.clear();
}

std::size_t DurableSampleBuffer::reap_expired(std::time_t now) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_ttl <= 0) return 0;
    const std::int64_t cutoff_ms =
        (static_cast<std::int64_t>(now) - m_ttl) * 1000;
    // Only reap PENDING rows — never an in-flight (leased) batch.
    exec(("DELETE FROM outbox WHERE sent=0 AND ts < " +
          std::to_string(cutoff_ms) + ";").c_str());
    const std::size_t reaped = static_cast<std::size_t>(sqlite3_changes(m_db));
    m_dropped += reaped;
    return reaped;
}

std::unique_ptr<ISampleBuffer>
make_sample_buffer(const std::string& dbPath, std::size_t cap,
                   std::int64_t ttlSeconds) {
    if (!dbPath.empty()) {
        try {
            return std::make_unique<DurableSampleBuffer>(dbPath, cap, ttlSeconds);
        } catch (const std::exception& e) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D lwm2m:thread:%t %M %N:%l telemetry buffer: "
                                "durable open failed (%C) — falling back to "
                                "in-RAM (no offline backfill)\n"), e.what()));
        }
    }
    return std::make_unique<SampleBuffer>(cap);
}

}} // namespace lwm2m::telemetry
