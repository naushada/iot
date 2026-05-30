#ifndef __data_store_server_worker_hpp__
#define __data_store_server_worker_hpp__

/// Active-object worker for the ds-server reactor. Same shape as
/// xpmile's MicroService: an ACE_Task with its own thread that pulls
/// `WorkRequest*` messages off its msg_queue and processes them.
///
/// One Worker can serve many sessions sequentially; the WorkerPool
/// round-robins accepted connections across N workers (default 5)
/// so the reactor thread is never blocked on a per-request handler.
///
/// D1 scope: each WorkRequest carries an accepted ACE_LSOCK_Stream;
/// the worker writes the welcome line and closes the stream. D2+
/// extends the request to carry a parsed op + key vector.

#include <atomic>
#include <memory>

#include <ace/LSOCK_Stream.h>
#include <ace/Synch_Traits.h>
#include <ace/Task.h>

#include "data_store.hpp"

namespace data_store::server {

/// Per-accepted-connection request handed to a worker. The worker
/// takes ownership of `stream` and closes it when done.
///
/// Pointers are passed through the worker's msg_queue inside an
/// `ACE_Message_Block` whose payload is a raw `WorkRequest*` — the
/// canonical ACE producer/consumer idiom; see xpmile webservice.cpp.
struct WorkRequest {
    ACE_LSOCK_Stream stream;
    // D2+ adds: std::string requestBytes; proto::Op op; ...
};

class Worker : public ACE_Task<ACE_MT_SYNCH> {
public:
    Worker(std::shared_ptr<DataStore> store, int id);
    ~Worker() override;

    /// Activate the worker thread. Returns 0 on success, -1 if
    /// ACE_Task::activate fails.
    int open(void* args = nullptr) override;

    /// Reactor-thread caller: hand off a heap-allocated WorkRequest.
    /// Worker takes ownership. Returns 0 on success.
    int enqueue(WorkRequest* req);

    /// Worker thread body: getq → process → release.
    int svc() override;

    /// Drain + join. Reactor thread calls this during shutdown.
    int close(u_long flags = 0) override;

    int id() const { return m_id; }

private:
    void process(WorkRequest* req);

    std::shared_ptr<DataStore> m_store;
    int                        m_id;
};

} // namespace data_store::server

#endif /* __data_store_server_worker_hpp__ */
