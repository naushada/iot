#ifndef __data_store_server_worker_pool_hpp__
#define __data_store_server_worker_pool_hpp__

/// Pool of N (default 5) Worker active objects, round-robin
/// dispatch. Owns the lifetime of each worker — `open()` activates
/// every worker thread; `close()` deactivates the queues and joins.
///
/// Matches the xpmile MicroService pool: a single shared pool is
/// constructed once at startup, every accepted connection picks the
/// next worker via `next()`, no per-connection thread.

#include <cstddef>
#include <memory>
#include <vector>

#include "data_store.hpp"
#include "worker.hpp"

namespace data_store::server {

class WorkerPool {
public:
    static constexpr std::size_t kDefaultSize = 5;

    WorkerPool(std::shared_ptr<DataStore> store,
               std::size_t poolSize = kDefaultSize);
    ~WorkerPool();

    int  open();
    int  close();

    /// Returns the next worker in round-robin order. Never returns
    /// nullptr once open() has been called successfully.
    Worker* next();

    std::size_t size() const { return m_workers.size(); }

private:
    std::vector<std::unique_ptr<Worker>> m_workers;
    std::size_t                          m_idx = 0;
    bool                                 m_open = false;
};

} // namespace data_store::server

#endif /* __data_store_server_worker_pool_hpp__ */
