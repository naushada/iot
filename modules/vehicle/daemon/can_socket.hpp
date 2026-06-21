#ifndef __vehicle_can_socket_hpp__
#define __vehicle_can_socket_hpp__

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <ace/Event_Handler.h>

/// Reactor-driven SocketCAN (raw CAN) channel.
///
/// SocketCAN is a Linux netdev with no ACE wrapper, so we open the raw PF_CAN
/// socket ourselves and feed its fd to the ACE_Reactor (the project rule:
/// "never raw POSIX event loops — feed the driver fd to ACE_Reactor"). Each
/// readable burst is one `struct can_frame`; we hand `(id, data, dlc)` to the
/// caller's callback. `send()` transmits an 8-byte OBD request frame.

namespace vehicle {

class CanSocket : public ACE_Event_Handler {
    public:
        /// frame callback: (can_id, data[dlc], dlc).
        using FrameFn = std::function<void(std::uint32_t,
                                           const std::uint8_t*,
                                           std::uint8_t)>;

        explicit CanSocket(FrameFn on_frame) : m_on_frame(std::move(on_frame)) {}
        ~CanSocket() override { close(); }

        /// Open + bind the raw CAN socket on `iface` (e.g. "can0"), install a
        /// filter for the OBD response id range, and register with `reactor`
        /// for READ events. Returns 0 / -1.
        int open(const std::string& iface, ACE_Reactor* reactor);
        void close();

        /// Transmit an 8-byte data frame on `can_id`. Returns bytes sent / -1.
        int send(std::uint32_t can_id, const std::array<std::uint8_t, 8>& data);

        /* ACE_Event_Handler */
        ACE_HANDLE get_handle() const override;
        int handle_input(ACE_HANDLE) override;
        int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;

    private:
        FrameFn m_on_frame;
        int     m_fd = -1;
};

} // namespace vehicle

#endif /*__vehicle_can_socket_hpp__*/
