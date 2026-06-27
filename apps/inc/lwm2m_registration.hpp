#ifndef __lwm2m_registration_hpp__
#define __lwm2m_registration_hpp__

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lwm2m_codec_linkformat.hpp"

/**
 * @file lwm2m_registration.hpp
 * @brief Server-side registration record + ClientRegistry.
 *
 * L3 building block per apps/docs/lwm2m-design.md §5.4 and
 * apps/docs/lwm2m-rdd.md REQ-REG-009, REQ-REG-011.
 *
 * Decisions baked in:
 *   D2 — Every record carries `shortServerId` from day one even though v1
 *        ships single-server.
 *   D3 — The registry mirror (lwm2m_registry_mirror.hpp) consumes
 *        RegistryEvent records produced when this registry mutates.
 */

namespace lwm2m {

/// One client registration as seen from the server side.
struct ServerRegistration {
    std::string                                  endpoint;          ///< ep=...
    std::string                                  location;          ///< "/rd/abc123"
    std::uint16_t                                shortServerId{0};  ///< D2
    std::uint32_t                                lifetime{86400};   ///< seconds
    std::string                                  binding{"U"};      ///< U / T / UQ / TQ (LwM2M 1.1 §6.2.2)
    std::string                                  smsNumber;
    std::string                                  lwm2mVersion{"1.1"};
    std::string                                  peerHost;
    std::uint16_t                                peerPort{0};
    std::vector<linkformat::LinkEntry>           advertisedSet;

    std::chrono::steady_clock::time_point        registeredAt{};
    std::chrono::steady_clock::time_point        expiresAt{};

    bool expired(std::chrono::steady_clock::time_point now) const {
        return now >= expiresAt;
    }
};

/// In-memory authoritative registration table per D3.
class ClientRegistry {
public:
    ClientRegistry() = default;

    /// Insert a registration, reusing a **stable `/rd/{location}` per endpoint**.
    /// A device that re-registers (reboot) lands on the SAME location it had
    /// before — even if its prior registration had already expired or been
    /// deregistered — instead of minting a fresh one and orphaning the old. The
    /// reuse is backed by an endpoint→location index that OUTLIVES the
    /// registration record (it is not erased on expire/remove), so the mapping
    /// survives a long offline gap; `load_from()` rebuilds it so it also
    /// survives a server restart. Only a genuinely new endpoint mints a fresh
    /// identifier. This prevents location churn and the duplicate
    /// `cloud.lwm2m.registrations` rows where a stale row could win the
    /// last-writer reconcile. `location` is populated on the stored record;
    /// callers read the returned value to build the Location-Path response. The
    /// current clock + `lifetime` define `expiresAt`. Returns the location.
    std::string add(ServerRegistration in,
                    std::chrono::steady_clock::time_point now =
                        std::chrono::steady_clock::now());

    /// Refresh lifetime / advertised set / binding on an existing entry, and
    /// (when `peerHost` is non-empty) the public/ISP peer address+port — so a
    /// device that keeps the same location but reconnects from a new NAT
    /// address (the Update doubles as the keepalive) updates its recorded
    /// peer. Returns true on success, false if the location is unknown.
    bool update(const std::string& location,
                std::uint32_t newLifetime,
                const std::string& newBinding,
                const std::vector<linkformat::LinkEntry>* newAdvertised,
                const std::string& peerHost = std::string(),
                std::uint16_t peerPort = 0,
                std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now());

    /// DELETE /rd/{loc}. Returns true on success.
    bool remove(const std::string& location);

    const ServerRegistration* find(const std::string& location) const;

    /// Idle-tick callback (driven by the reactor). Removes every entry
    /// whose `expiresAt <= now`. Returns the locations that were removed
    /// so the caller can fire whatever ClientGone hook applies (D3 mirror,
    /// L7 observe cleanup, …).
    std::vector<std::string> expire(std::chrono::steady_clock::time_point now);

    /// Replace the entire registry with `persisted`. Used by the
    /// RegistryMirror after rebuilding from MongoDB at startup; no event
    /// is fired (would loop). Existing entries are dropped.
    void load_from(std::vector<ServerRegistration> persisted);

    /// All registrations keyed by location, ascending.
    const std::map<std::string, ServerRegistration>& all() const { return m_byLocation; }
    std::size_t size() const { return m_byLocation.size(); }

    /// Test seam: override the location-id generator. Default emits
    /// monotonically increasing decimal IDs (`"1"`, `"2"`, …) which is
    /// good enough for v1 — Leshan uses random base64; we may switch in L9
    /// after the interop pass.
    using IdGen = std::string (*)(std::uint64_t counter);
    void set_id_generator(IdGen g) { m_idGen = g; }

private:
    std::map<std::string, ServerRegistration> m_byLocation;
    /// endpoint → its stable /rd/{location}. Persists across expire()/remove()
    /// so a re-register after the record is gone reuses the same location.
    /// Rebuilt by load_from(). Bounded by fleet size.
    std::map<std::string, std::string>        m_locByEndpoint;
    std::uint64_t                             m_locCounter{0};
    IdGen                                     m_idGen{nullptr};
};

} // namespace lwm2m

#endif /*__lwm2m_registration_hpp__*/
