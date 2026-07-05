#include "ddns/provider.hpp"

namespace ddns {

// Defined in the per-provider translation units.
std::unique_ptr<ProviderBackend> make_dyndns2_backend();
std::unique_ptr<ProviderBackend> make_duckdns_backend();
std::unique_ptr<ProviderBackend> make_cloudflare_backend();
std::unique_ptr<ProviderBackend> make_route53_backend();

std::unique_ptr<ProviderBackend> make_backend(const std::string& provider) {
    if (provider == "dyndns2")    return make_dyndns2_backend();
    if (provider == "duckdns")    return make_duckdns_backend();
    if (provider == "cloudflare") return make_cloudflare_backend();
    if (provider == "route53")    return make_route53_backend();
    return nullptr;
}

} // namespace ddns
