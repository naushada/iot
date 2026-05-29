#include "lwm2m_registration.hpp"

namespace lwm2m {

namespace {
std::string default_id(std::uint64_t counter) {
    return std::to_string(counter);
}
} // namespace

std::string ClientRegistry::add(ServerRegistration in,
                                std::chrono::steady_clock::time_point now) {
    auto gen = m_idGen ? m_idGen : default_id;
    std::string loc;
    do {
        loc = "/rd/" + gen(++m_locCounter);
    } while (m_byLocation.find(loc) != m_byLocation.end());

    in.location     = loc;
    in.registeredAt = now;
    in.expiresAt    = now + std::chrono::seconds(in.lifetime);
    m_byLocation[loc] = std::move(in);
    return loc;
}

bool ClientRegistry::update(const std::string& location,
                            std::uint32_t newLifetime,
                            const std::string& newBinding,
                            const std::vector<linkformat::LinkEntry>* newAdvertised,
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
    for (auto& reg : persisted) {
        std::string loc = reg.location;     // preserve persisted IDs
        m_byLocation[loc] = std::move(reg);
    }
}

} // namespace lwm2m
