#include "http_server/worker.hpp"

namespace http_server {

WorkerPool::WorkerPool(std::size_t threads) : m_threads(threads) {}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
    if (m_threads == 0 || m_started) return;
    m_started = true;
    m_pool.reserve(m_threads);
    for (std::size_t i = 0; i < m_threads; ++i) {
        m_pool.emplace_back([this] { run(); });
    }
}

void WorkerPool::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop) return;
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_pool) {
        if (t.joinable()) t.join();
    }
    m_pool.clear();
}

void WorkerPool::submit(Job job) {
    // No pool (or not started) → run inline so the caller is agnostic.
    if (m_threads == 0 || !m_started) {
        job();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop) {           // shutting down — best-effort inline.
            job();
            return;
        }
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
}

void WorkerPool::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
            if (m_stop && m_jobs.empty()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }
        job();
    }
}

} // namespace http_server
