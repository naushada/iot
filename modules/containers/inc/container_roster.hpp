#ifndef __iot_container_roster_hpp__
#define __iot_container_roster_hpp__

#include <string>
#include <vector>

/// Pure parser for the `container.instances` roster document the daemon
/// publishes to the data-store. On restart iot-containerd reads this back to
/// rehydrate its in-memory map (the data-store is the source of truth — the
/// daemon keeps no separate on-disk state), then reconciles each entry against
/// live crun state. Kept ACE/data-store-free so it is host-unit-testable.
///
/// See apps/docs/tdd-device-containers.md — "restart / rehydrate".

namespace containers {

/// One container as recorded in `container.instances`. Config fields
/// (image/net/subnet/mem/cpus/entrypoint/cmd) plus the last-published live view
/// (state/ip/gateway); enough to fully reconstruct a ContainerInstance so a
/// rehydrated container can still be Run/Stopped identically.
struct RosterEntry {
    std::string name;
    std::string image;       ///< "image" (ref)
    std::string image_id;    ///< "imageId"
    std::string size;        ///< "size"
    std::string net;         ///< "net" — "host" | "bridge"
    std::string subnet;      ///< "subnet"
    std::string mem;         ///< "mem"
    std::string cpus;        ///< "cpus"
    std::string entrypoint;  ///< "entrypoint"
    std::string cmd;         ///< "cmd"
    std::string state;       ///< "state"
    std::string ip;          ///< "ip"
    std::string gateway;     ///< "gateway"
};

/// Parse the `container.instances` JSON array into `out`. Returns false only on
/// malformed JSON (a valid empty array "[]" yields an empty vector + true).
/// Missing object fields default to "".
bool parse_roster(const std::string& json, std::vector<RosterEntry>& out);

} // namespace containers

#endif /* __iot_container_roster_hpp__ */
