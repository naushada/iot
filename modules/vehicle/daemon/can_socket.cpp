#include "can_socket.hpp"

#include <cstring>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>

#include "obd_pid.hpp"

namespace vehicle {

int CanSocket::open(const std::string& iface, ACE_Reactor* reactor) {
    m_fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (m_fd < 0) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [veh] socket(PF_CAN) failed: %m\n")), -1);
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(m_fd, SIOCGIFINDEX, &ifr) < 0) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [veh] SIOCGIFINDEX(%C) failed: %m\n"), iface.c_str()));
        close();
        return -1;
    }

    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(m_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [veh] bind(%C) failed: %m\n"), iface.c_str()));
        close();
        return -1;
    }

    // Only deliver OBD-II ECU responses (0x7E8..0x7EF): id & mask == 0x7E8.
    struct can_filter rf;
    rf.can_id   = obd::kEcuResponseIdBase;
    rf.can_mask = 0x7F8;
    if (::setsockopt(m_fd, SOL_CAN_RAW, CAN_RAW_FILTER, &rf, sizeof(rf)) < 0) {
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [veh] CAN_RAW_FILTER failed: %m (continuing unfiltered)\n")));
    }

    if (reactor->register_handler(this, ACE_Event_Handler::READ_MASK) == -1) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [veh] register_handler(%C) failed\n"), iface.c_str()));
        close();
        return -1;
    }
    return 0;
}

void CanSocket::close() {
    if (m_fd >= 0 && reactor()) {
        reactor()->remove_handler(this,
            ACE_Event_Handler::READ_MASK | ACE_Event_Handler::DONT_CALL);
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

int CanSocket::send(std::uint32_t can_id, const std::array<std::uint8_t, 8>& data) {
    if (m_fd < 0) return -1;
    struct can_frame f;
    std::memset(&f, 0, sizeof(f));
    f.can_id  = can_id;
    f.can_dlc = 8;
    std::memcpy(f.data, data.data(), 8);
    const ssize_t n = ::write(m_fd, &f, sizeof(f));
    return static_cast<int>(n);
}

ACE_HANDLE CanSocket::get_handle() const {
    return static_cast<ACE_HANDLE>(m_fd);
}

int CanSocket::handle_input(ACE_HANDLE) {
    struct can_frame f;
    const ssize_t n = ::read(m_fd, &f, sizeof(f));
    if (n < static_cast<ssize_t>(sizeof(struct can_frame))) {
        return -1;  // short read / error → reactor calls handle_close
    }
    if (m_on_frame) {
        m_on_frame(f.can_id & CAN_EFF_MASK, f.data, f.can_dlc);
    }
    return 0;
}

int CanSocket::handle_close(ACE_HANDLE, ACE_Reactor_Mask) {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    return 0;
}

} // namespace vehicle
