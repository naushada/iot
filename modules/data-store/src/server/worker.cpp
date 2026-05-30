#include "worker.hpp"

#include "data_store/proto.hpp"

#include <cstring>

#include <ace/Log_Msg.h>
#include <ace/Message_Block.h>

namespace data_store::server {

namespace {

/// Wrap a heap-allocated WorkRequest* as an ACE_Message_Block payload.
/// Returns nullptr on allocation failure.
ACE_Message_Block* make_block(WorkRequest* req) {
    auto* mb = new ACE_Message_Block(sizeof(WorkRequest*));
    *reinterpret_cast<WorkRequest**>(mb->wr_ptr()) = req;
    mb->wr_ptr(sizeof(WorkRequest*));
    return mb;
}

/// Reverse: pull the pointer back out of the message block.
WorkRequest* unwrap_block(ACE_Message_Block* mb) {
    return *reinterpret_cast<WorkRequest**>(mb->rd_ptr());
}

} // namespace

Worker::Worker(std::shared_ptr<DataStore> store, int id)
  : m_store(std::move(store)), m_id(id) {
}

Worker::~Worker() {
    close();
}

int Worker::open(void* /*args*/) {
    if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d "
                                   "activate failed errno=%d\n"),
                          m_id, errno),
                         -1);
    }
    return 0;
}

int Worker::enqueue(WorkRequest* req) {
    if (!req) return -1;
    ACE_Message_Block* mb = make_block(req);
    if (putq(mb) == -1) {
        mb->release();
        delete req;
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d "
                                   "putq failed errno=%d\n"),
                          m_id, errno),
                         -1);
    }
    return 0;
}

int Worker::svc() {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d started\n"),
               m_id));

    while (true) {
        ACE_Message_Block* mb = nullptr;
        if (getq(mb) == -1) {
            // msg_queue deactivated → graceful shutdown.
            break;
        }
        WorkRequest* req = unwrap_block(mb);
        mb->release();
        process(req);
        delete req;
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d exiting\n"),
               m_id));
    return 0;
}

int Worker::close(u_long /*flags*/) {
    // Drain whatever's left on the queue first — every WorkRequest
    // here owns an open fd, so dropping it would leak the socket.
    msg_queue()->deactivate();
    wait();

    ACE_Message_Block* mb = nullptr;
    while (msg_queue()->dequeue_head(mb,
            const_cast<ACE_Time_Value*>(&ACE_Time_Value::zero)) != -1) {
        if (mb) {
            WorkRequest* req = unwrap_block(mb);
            mb->release();
            if (req) req->stream.close();
            delete req;
        }
    }
    return 0;
}

void Worker::process(WorkRequest* req) {
    // D1: every request is "send welcome + close". D2 splits this
    // into op dispatch (proto::parse_op → switch).
    const std::size_t len = std::strlen(proto::kWelcomeLine);
    ssize_t wrote = req->stream.send_n(proto::kWelcomeLine, len);
    if (wrote < 0 || static_cast<std::size_t>(wrote) != len) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [Worker:%t] %M %N:%l worker %d welcome "
                            "short write (%d/%d) errno=%d\n"),
                   m_id,
                   static_cast<int>(wrote),
                   static_cast<int>(len),
                   errno));
    }
    req->stream.close();
}

} // namespace data_store::server
