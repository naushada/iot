#include "data_store/log_buffer.hpp"
#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <ace/Event_Handler.h>
#include <ace/Log_Msg.h>
#include <ace/Log_Record.h>
#include <ace/Log_Msg_Callback.h>
#include <ace/OS_NS_Thread.h>
#include <ace/Reactor.h>
#include <ace/Synch_Traits.h>
#include <ace/Task.h>
#include <ace/Time_Value.h>

#include <atomic>
#include <cerrno>
#include <ctime>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>

namespace data_store {

// Most recently start()ed callback + the level mask computed by the last
// apply_level(). Used by attach_current_thread() so a worker thread running
// its own reactor can route its ACE log output into the same ring buffer AND
// honour the configured log level — ACE_Log_Msg is per-thread, so a spawned
// reactor thread inherits neither. One LogBuffer per process in practice.
namespace {
    ACE_Log_Msg_Callback*      s_active_cb   = nullptr;
    std::atomic<unsigned long> s_active_mask{LM_INFO};
    // Bumped on every apply_level(): lets a long-lived worker thread (which
    // pinned its per-thread mask once at attach_current_thread()) notice a
    // runtime level change and re-pin via refresh_level(). ACE_Log_Msg is
    // per-thread, so apply_level() on the main thread cannot reach it otherwise.
    std::atomic<unsigned>      s_mask_gen{0};
    thread_local unsigned      t_seen_gen = 0;
}

struct LogBuffer::Impl {
    std::mutex              mtx;
    std::deque<std::string> buf;
    std::string             key;        // ds key for flush ("log.cloudd.text")
    std::string             daemon;     // "cloudd", "httpd", "lwm2m"
    std::string             level_key;  // "log.level.cloudd"
    std::size_t             bytes_since_flush = 0;
    static constexpr std::size_t kMaxLines = 200;

    // Inner callback registered with ACE — lives as long as the Impl.
    class Callback : public ACE_Log_Msg_Callback {
    public:
        explicit Callback(Impl* owner) : m_owner(owner) {}
        void log(ACE_Log_Record& rec) override {
            // ACE already formats timestamps + levels via %D %M %N:%l.
            // Just tag with the daemon name for grep-ability.
            std::string line =
                m_owner->daemon + ": " + rec.msg_data() + "\n";
            std::lock_guard<std::mutex> lk(m_owner->mtx);
            m_owner->bytes_since_flush += line.size();
            m_owner->buf.push_back(std::move(line));
            while (m_owner->buf.size() > kMaxLines)
                m_owner->buf.pop_front();
        }

    private:
        Impl* m_owner;
    };

    Callback cb{this};

    // ── Periodic flush timer (same pattern as StatsPublisher) ────────
    // A private ACE_Reactor run on its own ACE_Task thread fires every
    // interval and flushes the ring buffer to ds — so daemons don't flush
    // from their own loop / a std::thread.
    LogBuffer*    owner = nullptr;          // back-ref for flush()
    Client*       ds    = nullptr;          // flush target (set in open())
    std::size_t   min_bytes = 0;
    ACE_Reactor*  reactor = nullptr;        // private, owned while running
    long          timer_id = -1;

    struct Pump : ACE_Task<ACE_MT_SYNCH> {
        ACE_Reactor* reactor = nullptr;
        std::atomic<bool> running{false};
        int svc() override {
            reactor->owner(ACE_OS::thr_self());
            reactor->run_reactor_event_loop();
            return 0;
        }
    } pump;

    struct Tick : ACE_Event_Handler {
        Impl* impl = nullptr;
        int handle_timeout(const ACE_Time_Value&, const void*) override {
            if (impl->owner && impl->ds) impl->owner->flush(*impl->ds, impl->min_bytes);
            return 0;
        }
    } tick;
};

LogBuffer::LogBuffer(const std::string& daemon, const std::string& log_key,
                     const std::string& level_key)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->daemon    = daemon;
    m_impl->key       = log_key;
    m_impl->level_key = level_key;
    // NOTE: start() must be called from main() to register the
    // ACE callback — doing it in the constructor would run during
    // static initialisation, before ACE is ready.
}

LogBuffer::LogBuffer(const std::string& daemon, const std::string& log_key)
    : LogBuffer(daemon, log_key, "log.level") {}

void LogBuffer::start() {
    if (!m_impl) return;
    // Enable the callback sink — default flags only have STDERR.
    auto* lm = ACE_Log_Msg::instance();
    lm->set_flags(lm->flags() | ACE_Log_Msg::MSG_CALLBACK);
    lm->msg_callback(&m_impl->cb);
    s_active_cb = &m_impl->cb;   // so worker threads can attach the same sink
}

void LogBuffer::attach_current_thread() {
    if (!s_active_cb) return;
    auto* lm = ACE_Log_Msg::instance();
    lm->set_flags(lm->flags() | ACE_Log_Msg::MSG_CALLBACK);
    lm->msg_callback(s_active_cb);
    // Also pin this thread's level to the configured mask — a freshly
    // spawned reactor thread otherwise defaults to "all levels", leaking
    // DEBUG noise that the INFO default is meant to suppress.
    lm->priority_mask(static_cast<int>(s_active_mask.load()), ACE_Log_Msg::THREAD);
    t_seen_gen = s_mask_gen.load();
}

void LogBuffer::refresh_level() {
    // Cheap (one relaxed atomic load) when nothing changed — safe to call
    // every iteration of a worker thread's reactor loop. Re-pins this thread's
    // per-thread ACE mask when apply_level() has run since we last attached,
    // so a runtime log-level change reaches reactor/worker threads (not just
    // the main thread that handles the watch).
    const unsigned g = s_mask_gen.load(std::memory_order_relaxed);
    if (g == t_seen_gen) return;
    t_seen_gen = g;
    ACE_Log_Msg::instance()->priority_mask(
        static_cast<int>(s_active_mask.load()), ACE_Log_Msg::THREAD);
}

int LogBuffer::open(Client& ds, int interval_sec, std::size_t min_bytes) {
    if (!m_impl || m_impl->timer_id != -1) return 0;   // already open
    if (interval_sec < 1) interval_sec = 1;

    m_impl->owner     = this;
    m_impl->ds        = &ds;
    m_impl->min_bytes = min_bytes;
    m_impl->tick.impl = m_impl.get();
    m_impl->reactor   = new ACE_Reactor();

    ACE_Time_Value iv(interval_sec);
    long id = m_impl->reactor->schedule_timer(&m_impl->tick, nullptr, iv, iv);
    if (id == -1) {
        delete m_impl->reactor; m_impl->reactor = nullptr; m_impl->ds = nullptr;
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D %M %N:%l log flush schedule_timer failed "
                                   "for %C\n"), m_impl->daemon.c_str()),
                         -1);
    }
    m_impl->timer_id = id;

    m_impl->pump.reactor = m_impl->reactor;
    m_impl->pump.running.store(true);
    if (m_impl->pump.activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
        m_impl->reactor->cancel_timer(&m_impl->tick);
        m_impl->timer_id = -1;
        m_impl->pump.running.store(false);
        delete m_impl->reactor; m_impl->reactor = nullptr; m_impl->ds = nullptr;
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D %M %N:%l log flush activate failed for "
                                   "%C errno=%d\n"), m_impl->daemon.c_str(), errno),
                         -1);
    }
    return 0;
}

void LogBuffer::close() {
    if (!m_impl || m_impl->timer_id == -1) return;
    m_impl->reactor->cancel_timer(&m_impl->tick);
    m_impl->timer_id = -1;
    m_impl->reactor->end_reactor_event_loop();
    m_impl->pump.wait();                       // join the flush thread
    m_impl->pump.running.store(false);
    if (m_impl->ds) flush(*m_impl->ds, 0);     // final flush while ds alive
    delete m_impl->reactor; m_impl->reactor = nullptr;
    m_impl->ds = nullptr;
}

LogBuffer::~LogBuffer() {
    close();
    if (m_impl) ACE_Log_Msg::instance()->msg_callback(nullptr);
}

void LogBuffer::flush(Client& ds, std::size_t min_bytes) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        if (m_impl->bytes_since_flush < min_bytes) return;  // below threshold
        for (const auto& line : m_impl->buf) text += line;
        m_impl->bytes_since_flush = 0;
    }
    if (!text.empty()) {
        ds.set(m_impl->key, Value{text}, 200);              // per-daemon log
        // Bump log.version so the cloud UI's long-poll wakes up.
        // Use timestamp in seconds — each flush produces a new value.
        auto ts = static_cast<std::int32_t>(std::time(nullptr));
        ds.set("log.version", Value{ts}, 100);
    }
}

void LogBuffer::set_log_key(const std::string& key) {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->key = key;
}

void LogBuffer::set_level_key(const std::string& key) {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->level_key = key;
}

void LogBuffer::append(const std::string& line) {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->bytes_since_flush += line.size();
    m_impl->buf.push_back(line);
    while (m_impl->buf.size() > Impl::kMaxLines)
        m_impl->buf.pop_front();
}

std::size_t LogBuffer::line_count() const {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    return m_impl->buf.size();
}

void LogBuffer::apply_level(Client& ds) {
    std::vector<Client::GetResult> lg;
    auto ls = ds.get({m_impl->level_key, "log.level"}, lg);
    std::string lvl_str;
    if (ls.ok) {
        for (const auto& g : lg) {
            if (g.has_value) {
                if (auto s = to_string(g.value)) {
                    if (!s->empty()) { lvl_str = *s; break; }
                }
            }
        }
    }
    // Cumulative: a level enables itself AND everything more severe, so e.g.
    // INFO still shows WARNING/ERROR and DEBUG shows INFO. The old mapping
    // enabled only the single level, hiding higher-severity lines (and hiding
    // INFO when set to DEBUG).
    const unsigned long kErr  = LM_ERROR | LM_CRITICAL | LM_ALERT | LM_EMERGENCY;
    const unsigned long kWarn = LM_WARNING | kErr;
    const unsigned long kInfo = LM_INFO | LM_NOTICE | kWarn;
    const unsigned long kDbg  = LM_TRACE | LM_DEBUG | kInfo;
    unsigned long mask = kInfo;   // default: INFO and above
    if (!lvl_str.empty()) {
        for (auto& c : lvl_str) c = static_cast<char>(std::toupper(c));
        if (lvl_str == "DEBUG")        mask = kDbg;
        else if (lvl_str == "INFO")    mask = kInfo;
        else if (lvl_str == "WARNING") mask = kWarn;
        else if (lvl_str == "ERROR")   mask = kErr;
    }
    s_active_mask = mask;            // remembered for attach_current_thread()
    s_mask_gen.fetch_add(1, std::memory_order_relaxed);  // wake refresh_level()
    auto* lm = ACE_Log_Msg::instance();
    // PROCESS seeds threads spawned LATER; THREAD fixes THIS (the calling/main)
    // thread NOW. ACE gates logging on the per-thread mask, so without the
    // THREAD set the main thread keeps its default "all levels" and every
    // ACE_DEBUG it emits prints regardless of the configured level.
    lm->priority_mask(static_cast<int>(mask), ACE_Log_Msg::PROCESS);
    lm->priority_mask(static_cast<int>(mask), ACE_Log_Msg::THREAD);
    t_seen_gen = s_mask_gen.load();
}

}  // namespace data_store