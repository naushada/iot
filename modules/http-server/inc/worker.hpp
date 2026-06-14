#ifndef __http_server_worker_hpp__
#define __http_server_worker_hpp__

/// Fixed-size thread pool for off-loading HTTP handler work from the
/// reactor thread (L18 / FUP-L18-1).
///
/// The reactor thread parses requests and submits a job per request; a
/// worker runs the (possibly blocking, e.g. long-poll) handler and posts
/// the response back to the reactor for sending. Socket / TLS I/O stays on
/// the reactor thread — workers only compute. See session.cpp.

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace http_server {

class WorkerPool {
public:
    using Job = std::function<void()>;

    /// `threads == 0` makes submit() run the job inline (no pool) — the
    /// original single-threaded behaviour. Otherwise start() spins up
    /// `threads` workers.
    explicit WorkerPool(std::size_t threads);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Spawn the worker threads (no-op when threads == 0).
    void start();

    /// Stop accepting jobs, drain nothing, wake + join all workers.
    /// Idempotent; also called by the destructor.
    void stop();

    /// Hand a job to the pool. When threads == 0 the job runs inline on
    /// the caller's thread (so callers need no special-casing).
    void submit(Job job);

    /// Queue a callback to be executed on the reactor thread. A worker that
    /// has finished computing a response uses this to deliver it (socket /
    /// TLS I/O is reactor-thread-only). The reactor loop must call
    /// drain_reactor() once per iteration to run the queued callbacks.
    ///
    /// This deliberately replaces the old `reactor()->notify(this,
    /// EXCEPT_MASK)` handoff: that dispatch dropped ~half its wakeups under
    /// load, leaving the response unsent and the client hung. A mutex-guarded
    /// queue drained by the reactor is reliable; an optional bare notify()
    /// only nudges the reactor awake so the drain runs without tick latency.
    void post_to_reactor(Job fn);

    /// Run every callback queued via post_to_reactor() since the last call.
    /// MUST be called only from the reactor thread (the callbacks touch
    /// sockets / delete handlers).
    void drain_reactor();

    std::size_t size() const { return m_threads; }

private:
    void run();

    const std::size_t       m_threads;
    std::vector<std::thread> m_pool;
    std::queue<Job>          m_jobs;
    std::mutex               m_mtx;
    std::condition_variable  m_cv;
    bool                     m_stop = false;
    bool                     m_started = false;

    // Reactor-thread completion queue (worker → reactor response handoff).
    std::vector<Job> m_reactor_jobs;
    std::mutex       m_reactor_mtx;
};

} // namespace http_server

#endif
