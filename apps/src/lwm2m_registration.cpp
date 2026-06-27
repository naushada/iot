#include "lwm2m_registration.hpp"

#include <algorithm>

namespace lwm2m {

namespace {
std::string default_id(std::uint64_t counter) {
    return std::to_string(counter);
}
} // namespace

std::string ClientRegistry::add(ServerRegistration in,
                                std::chrono::steady_clock::time_point now) {
    // Reuse a stable /rd/{location} per endpoint. A device that re-registers —
    // reboot, even after its prior registration EXPIRED or was deregistered —
    // lands on the SAME location instead of minting a new one and orphaning the
    // old. The endpoint→location index outlives the registration record (it is
    // not cleared on expire()/remove()), so the mapping survives a long offline
    // gap; load_from() rebuilds it across a server restart. Only a genuinely new
    // endpoint mints a fresh id. This kills the location churn that otherwise
    // leaves duplicate cloud.lwm2m.registrations rows where a STALE row can win
    // the last-writer reconcile (e.g. the device's recorded ISP IP stops
    // tracking).
    std::string loc;
    if (!in.endpoint.empty()) {
        auto it = m_locByEndpoint.find(in.endpoint);
        if (it != m_locByEndpoint.end()) loc = it->second;
    }
    if (loc.empty()) {
        auto gen = m_idGen ? m_idGen : default_id;
        do {
            loc = "/rd/" + gen(++m_locCounter);
        } while (m_byLocation.find(loc) != m_byLocation.end());
        if (!in.endpoint.empty()) m_locByEndpoint[in.endpoint] = loc;
    }

    in.location     = loc;
    in.registeredAt = now;
    in.expiresAt    = now + std::chrono::seconds(in.lifetime);
    m_byLocation[loc] = std::move(in);   // replace-or-insert (refreshes peer)
    return loc;
}

bool ClientRegistry::update(const std::string& location,
                            std::uint32_t newLifetime,
                            const std::string& newBinding,
                            const std::vector<linkformat::LinkEntry>* newAdvertised,
                            const std::string& peerHost,
                            std::uint16_t peerPort,
                            std::chrono::steady_clock::time_point now) {
    auto it = m_byLocation.find(location);
    if (it == m_byLocation.end()) return false;
    auto& reg = it->second;

    if (newLifetime > 0) {
        reg.lifetime = newLifetime;
    }
    if (!newBinding.empty()) {
        reg.binding = newBinding;
    }
    if (newAdvertised) {
        reg.advertisedSet = *newAdvertised;
    }
    // Refresh the public/ISP peer on every Update (the registration Update
    // doubles as the NAT keepalive, so it arrives from the device's current
    // public address). Only when known — the plain-UDP dispatch path passes an
    // empty peer and must not clobber a good address captured at Register.
    if (!peerHost.empty()) {
        reg.peerHost = peerHost;
        reg.peerPort = peerPort;
    }
    reg.expiresAt = now + std::chrono::seconds(reg.lifetime);
    return true;
}

bool ClientRegistry::remove(const std::string& location) {
    return m_byLocation.erase(location) > 0;
}

const ServerRegistration* ClientRegistry::find(const std::string& location) const {
    auto it = m_byLocation.find(location);
    return it == m_byLocation.end() ? nullptr : &it->second;
}

std::vector<std::string> ClientRegistry::expire(
    std::chrono::steady_clock::time_point now) {
    std::vector<std::string> expired;
    for (auto it = m_byLocation.begin(); it != m_byLocation.end(); ) {
        if (it->second.expired(now)) {
            expired.push_back(it->first);
            it = m_byLocation.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

void ClientRegistry::load_from(std::vector<ServerRegistration> persisted) {
    m_byLocation.clear();
    m_locByEndpoint.clear();
    std::uint64_t maxCounter = 0;
    for (auto& reg : persisted) {
        const std::string loc = reg.location;   // preserve persisted IDs
        // Rebuild the endpoint→location index so a re-register after a server
        // restart reuses the persisted location, not a fresh one.
        if (!reg.endpoint.empty()) m_locByEndpoint[reg.endpoint] = loc;
        // Advance the mint counter past any numeric "/rd/<n>" so a later mint
        // for a NEW endpoint never reissues a remembered location whose record
        // is currently absent (device offline at restore time).
        if (loc.compare(0, 4, "/rd/") == 0) {
            const std::string id = loc.substr(4);
            if (!id.empty() &&
                id.find_first_not_of("0123456789") == std::string::npos) {
                try {
                    maxCounter = std::max(maxCounter,
                                          static_cast<std::uint64_t>(std::stoull(id)));
                } catch (...) {}
            }
        }
        m_byLocation[loc] = std::move(reg);
    }
    m_locCounter = std::max(m_locCounter, maxCounter);
}

} // namespace lwm2m
