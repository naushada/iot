#include "data_store/log_buffer.hpp"
#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <ace/Log_Msg.h>
#include <ace/Log_Record.h>
#include <ace/Log_Msg_Callback.h>

#include <ctime>
#include <deque>
#include <mutex>
#include <string>

namespace data_store {

struct LogBuffer::Impl {
    std::mutex              mtx;
    std::deque<std::string> buf;
    std::string             key;
    std::string             daemon;  // "cloudd", "httpd", "lwm2m"
    static constexpr std::size_t kMaxLines = 200;

    // Inner callback registered with ACE — lives as long as the Impl.
    class Callback : public ACE_Log_Msg_Callback {
    public:
        explicit Callback(Impl* owner) : m_owner(owner) {}
        void log(ACE_Log_Record& rec) override {
            std::time_t t = static_cast<std::time_t>(rec.time_stamp().sec());
            struct std::tm tm_buf;
            ::localtime_r(&t, &tm_buf);
            char ts[16];
            std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

            const char* lvl = "?";
            switch (rec.type()) {
                case LM_DEBUG:    lvl = "DEBUG"; break;
                case LM_INFO:     lvl = "INFO";  break;
                case LM_NOTICE:   lvl = "NOTE";  break;
                case LM_WARNING:  lvl = "WARN";  break;
                case LM_ERROR:    lvl = "ERROR"; break;
                case LM_CRITICAL: lvl = "CRIT";  break;
            }

            std::lock_guard<std::mutex> lk(m_owner->mtx);
            m_owner->buf.push_back(
                std::string(ts) + " " + lvl + " " + m_owner->daemon + ": " +
                rec.msg_data() + "\n");
            while (m_owner->buf.size() > kMaxLines)
                m_owner->buf.pop_front();
        }

    private:
        Impl* m_owner;
    };

    Callback cb{this};
};

LogBuffer::LogBuffer(const std::string& daemon, const std::string& ds_key)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->daemon = daemon;
    m_impl->key    = ds_key;
    // NOTE: start() must be called from main() to register the
    // ACE callback — doing it in the constructor would run during
    // static initialisation, before ACE is ready.
}

void LogBuffer::start() {
    if (m_impl) ACE_Log_Msg::instance()->msg_callback(&m_impl->cb);
}

LogBuffer::~LogBuffer() {
    if (m_impl) ACE_Log_Msg::instance()->msg_callback(nullptr);
}

void LogBuffer::flush(Client& ds) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        for (const auto& line : m_impl->buf) text += line;
    }
    ds.set(m_impl->key, Value{text}, 200);  // best-effort
}

void LogBuffer::set_key(const std::string& key) {
    std::lock_guard<std::mutex> lk(m_impl->mtx);
    m_impl->key = key;
}

void LogBuffer::apply_level(Client& ds, const std::string& own_key) {
    std::vector<Client::GetResult> lg;
    auto ls = ds.get({own_key, "log.level"}, lg);
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
    unsigned long mask = LM_INFO;
    if (!lvl_str.empty()) {
        for (auto& c : lvl_str) c = static_cast<char>(std::toupper(c));
        if (lvl_str == "DEBUG")       mask = LM_DEBUG;
        else if (lvl_str == "INFO")   mask = LM_INFO;
        else if (lvl_str == "WARNING") mask = LM_WARNING;
        else if (lvl_str == "ERROR")  mask = LM_ERROR | LM_CRITICAL;
    }
    ACE_Log_Msg::instance()->priority_mask(
        static_cast<int>(mask), ACE_Log_Msg::PROCESS);
}

}  // namespace data_store