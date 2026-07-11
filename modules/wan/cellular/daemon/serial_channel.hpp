#ifndef __cellular_serial_channel_hpp__
#define __cellular_serial_channel_hpp__

#include <functional>
#include <string>

#include <ace/Event_Handler.h>
#include <ace/TTY_IO.h>

#include "line_router.hpp"

/**
 * @file serial_channel.hpp
 * @brief Reactor-driven serial (tty) channel for the modem AT / GNSS ports.
 *
 * Opens a tty through ACE_TTY_IO (no raw POSIX), registers its ACE_HANDLE with
 * the ACE_Reactor for READ events, and on each readable burst reassembles
 * complete lines (LineAssembler) and hands them to the caller's `on_line`
 * callback. The daemon points that callback at dispatch_at_line /
 * dispatch_nmea_line (the pure PR-A/PR-C routers). Writes append a CR.
 */

namespace cellular {

class SerialChannel : public ACE_Event_Handler {
    public:
        using LineFn = std::function<void(const std::string&)>;

        explicit SerialChannel(LineFn on_line, int baud = 115200)
            : m_on_line(std::move(on_line)), m_baud(baud) {}
        ~SerialChannel() override = default;

        /// Open `tty_path`, apply 8N1 @ baud, register with `reactor`. 0 / -1.
        int open(const std::string& tty_path, ACE_Reactor* reactor);
        void close();

        /// Invoked once when the tty is torn down by the reactor — i.e. the modem
        /// was unplugged / powered off (recv EOF/error → handle_close). Lets the
        /// daemon clear the now-stale cell.* it published while the modem was up.
        void on_closed(std::function<void()> cb) { m_on_closed = std::move(cb); }

        /// Send `cmd` followed by a carriage return. Returns bytes sent or -1.
        int write_line(const std::string& cmd);

        /// Send raw bytes verbatim (no CR appended) — for the AT+CMGS PDU + the
        /// Ctrl-Z terminator. Returns bytes sent or -1.
        int write_raw(const std::string& bytes);

        /* ACE_Event_Handler */
        ACE_HANDLE get_handle() const override;
        int handle_input(ACE_HANDLE) override;
        int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;

    private:
        ACE_TTY_IO            m_tty;
        LineFn                m_on_line;
        std::function<void()> m_on_closed;
        LineAssembler         m_asm;
        int                   m_baud;
};

} // namespace cellular

#endif /*__cellular_serial_channel_hpp__*/
