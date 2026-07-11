#include "serial_channel.hpp"

#include <ace/DEV_Connector.h>
#include <ace/DEV_Addr.h>
#include <ace/Log_Msg.h>
#include <ace/Reactor.h>

namespace cellular {

int SerialChannel::open(const std::string& tty_path, ACE_Reactor* reactor) {
    ACE_DEV_Connector connector;
    ACE_DEV_Addr addr(ACE_TEXT(tty_path.c_str()));
    if (connector.connect(m_tty, addr) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cell] open(%C) failed: %m\n"), tty_path.c_str()), -1);
    }

    // 8N1 at the requested baud, blocking 1-char reads, no flow control.
    ACE_TTY_IO::Serial_Params p;
    p.baudrate          = m_baud;
    p.xonlim            = 0;
    p.xofflim           = 0;
    p.readmincharacters = 1;
    p.readtimeoutmsec   = -1;
    p.paritymode        = "NONE";
    p.ctsenb            = 0;
    p.rtsenb            = 0;
    p.xinenb            = 0;
    p.xoutenb           = 0;
    p.modem             = 0;
    p.rcvenb            = 1;
    p.dsrenb            = 0;
    p.dtrdisable        = 0;
    p.databits          = 8;
    p.stopbits          = 1;
    if (m_tty.control(ACE_TTY_IO::SETPARAMS, &p) == -1) {
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [cell] SETPARAMS(%C) failed: %m (continuing)\n"),
            tty_path.c_str()));
    }

    if (reactor->register_handler(this, ACE_Event_Handler::READ_MASK) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cell] register_handler(%C) failed\n"), tty_path.c_str()), -1);
    }
    return 0;
}

void SerialChannel::close() {
    if (reactor()) {
        reactor()->remove_handler(this,
            ACE_Event_Handler::READ_MASK | ACE_Event_Handler::DONT_CALL);
    }
    m_tty.close();
}

int SerialChannel::write_line(const std::string& cmd) {
    const std::string out = cmd + "\r";
    return static_cast<int>(m_tty.send_n(out.data(), out.size()));
}

int SerialChannel::write_raw(const std::string& bytes) {
    return static_cast<int>(m_tty.send_n(bytes.data(), bytes.size()));
}

ACE_HANDLE SerialChannel::get_handle() const {
    return const_cast<ACE_TTY_IO&>(m_tty).get_handle();
}

int SerialChannel::handle_input(ACE_HANDLE) {
    char buf[512];
    const ssize_t n = m_tty.recv(buf, sizeof(buf));
    if (n <= 0) {
        return -1;   // EOF / error → reactor calls handle_close
    }
    for (const auto& line : m_asm.feed(std::string(buf, static_cast<std::size_t>(n)))) {
        if (m_on_line) m_on_line(line);
    }
    return 0;
}

int SerialChannel::handle_close(ACE_HANDLE, ACE_Reactor_Mask) {
    m_tty.close();
    if (m_on_closed) m_on_closed();   // modem gone → let the daemon clear cell.*
    return 0;
}

} // namespace cellular
