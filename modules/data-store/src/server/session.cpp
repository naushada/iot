#include "session.hpp"

#include "data_store.hpp"
#include "data_store/proto.hpp"
#include "server.hpp"
#include "worker.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>

namespace data_store::server {

Session::Session(ACE_LSOCK_Stream stream,
                 Worker*          owner,
                 DataStore*       store,
                 Server*          server)
  : m_stream(stream),
    m_owner(owner),
    m_store(store),
    m_server(server) {
}

ACE_HANDLE Session::get_handle() const {
    return const_cast<ACE_LSOCK_Stream&>(m_stream).get_handle();
}

int Session::handle_input(ACE_HANDLE /*fd*/) {
    // Reactor thread: read what's available, extract complete EMP
    // frames (header + payload), and enqueue each as a ProcessRequest
    // for the owner Worker. The worker decodes the header to dispatch
    // — keeping the reactor path bytes-only minimises lock/parse work
    // on the shared thread.
    char buf[4096];
    ssize_t n = m_stream.recv(buf, sizeof(buf));
    if (n <= 0) {
        // EOF or hard error — return -1 to make the reactor invoke
        // handle_close.
        return -1;
    }
    m_recvBuf.append(buf, static_cast<std::size_t>(n));

    for (;;) {
        proto::Header h;
        std::string   payload;
        bool got = false;
        try {
            // Peek at the header without consuming, so we can compute
            // the full frame length, then carve out [header || body]
            // as a single contiguous blob for the worker to redecode.
            if (m_recvBuf.size() < proto::kHeaderSize) break;
            proto::decode_header(m_recvBuf.data(), h);
            if (h.payload_size > proto::kMaxPayloadBytes) {
                throw std::runtime_error("oversized payload");
            }
            const std::size_t need = proto::kHeaderSize + h.payload_size;
            if (m_recvBuf.size() < need) break;
            payload.assign(m_recvBuf.data(), need);
            m_recvBuf.erase(0, need);
            got = true;
        } catch (const std::exception& e) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [Session:%t] %M %N:%l bad EMP frame: "
                                "%C — dropping connection\n"),
                       e.what()));
            return -1;
        }

        if (got && m_owner) {
            auto* msg = new WorkMsg{
                WorkKind::ProcessRequest, this, std::move(payload)};
            if (m_owner->enqueue(msg) == -1) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [Session:%t] %M %N:%l enqueue "
                                    "to worker %d failed\n"),
                           m_owner->id()));
            }
        }
    }
    return 0;
}

int Session::handle_close(ACE_HANDLE /*fd*/, ACE_Reactor_Mask /*mask*/) {
    // Reactor thread: deregister + tell owner Worker to free us
    // after any in-flight requests for this Session finish.
    m_closed_in_reactor = true;
    m_stream.close();
    if (m_server) m_server->note_session_closed(this);
    if (m_owner) {
        auto* msg = new WorkMsg{WorkKind::SessionClosed, this, std::string{}};
        m_owner->enqueue(msg);
    }
    return 0;
}

int Session::send(const std::string& bytes) {
    if (bytes.empty()) return 0;
    ssize_t n = m_stream.send_n(bytes.data(), bytes.size());
    if (n < 0 || static_cast<std::size_t>(n) != bytes.size()) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [Session:%t] %M %N:%l send short "
                            "(%d/%d) errno=%d\n"),
                   static_cast<int>(n),
                   static_cast<int>(bytes.size()),
                   errno));
        return -1;
    }
    return 0;
}

void Session::note_watch(const std::string& k) {
    m_watches.insert(k);
}

void Session::note_unwatch(const std::string& k) {
    m_watches.erase(k);
}

} // namespace data_store::server
