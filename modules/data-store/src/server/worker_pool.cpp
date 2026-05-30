#include "worker_pool.hpp"

#include <ace/Log_Msg.h>

namespace data_store::server {

WorkerPool::WorkerPool(std::shared_ptr<DataStore> store,
                       std::size_t poolSize) {
    m_workers.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        m_workers.emplace_back(
            std::make_unique<Worker>(store, static_cast<int>(i)));
    }
}

WorkerPool::~WorkerPool() {
    close();
}

int WorkerPool::open() {
    if (m_open) return 0;
    for (auto& w : m_workers) {
        if (w->open() == -1) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [Pool:%t] %M %N:%l worker %d failed "
                                "to activate; closing pool\n"),
                       w->id()));
            close();
            return -1;
        }
    }
    m_open = true;
    return 0;
}

int WorkerPool::close() {
    if (!m_open) return 0;
    // Worker::close drains its queue + joins.
    for (auto& w : m_workers) {
        w->close();
    }
    m_workers.clear();
    m_open = false;
    return 0;
}

Worker* WorkerPool::next() {
    if (m_workers.empty()) return nullptr;
    Worker* w = m_workers[m_idx].get();
    m_idx = (m_idx + 1) % m_workers.size();
    return w;
}

} // namespace data_store::server
