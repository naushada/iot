#ifndef __data_store_server_worker_hpp__
#define __data_store_server_worker_hpp__

/// Active-object worker. xpmile MicroService shape: ACE_Task with
/// its own thread + msg_queue; reactor thread enqueues, worker
/// thread consumes.
///
/// D2/D3/D5 carry three message kinds — a request line from a
/// session (parse + dispatch + reply), a notify delivery for a
/// session this worker owns (write the bytes), and a session-closed
/// notification (clean up watches + delete session).

#include <cstdint>
#include <memory>
#include <string>

#include <ace/Synch_Traits.h>
#include <ace/Task.h>

namespace data_store::server {

class DataStore;
class Session;

enum class WorkKind : std::uint8_t {
    ProcessRequest,   ///< session sent a line; parse + dispatch + reply
    DeliverNotify,    ///< write `payload` to `session` (owner-only)
    SessionClosed,    ///< reactor saw EOF; unwatch + delete session
};

struct WorkMsg {
    WorkKind     kind;
    Session*     session = nullptr;
    std::string  payload;
};

class Worker : public ACE_Task<ACE_MT_SYNCH> {
public:
    Worker(std::shared_ptr<DataStore> store, int id);
    ~Worker() override;

    int open(void* args = nullptr) override;

    /// Reactor-thread caller (or peer worker): hand off a
    /// heap-allocated WorkMsg. Worker takes ownership.
    int enqueue(WorkMsg* msg);

    int svc() override;
    int close(u_long flags = 0) override;

    int id() const { return m_id; }

private:
    void handle_process_request(WorkMsg* msg);
    void handle_deliver_notify(WorkMsg* msg);
    void handle_session_closed(WorkMsg* msg);

    std::shared_ptr<DataStore> m_store;
    int                        m_id;
};

} // namespace data_store::server

#endif /* __data_store_server_worker_hpp__ */
