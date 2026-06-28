#ifndef __iot_http_tenant_scope_hpp__
#define __iot_http_tenant_scope_hpp__

/// Multi-tenant console scoping helpers (apps/docs/tdd-multi-tenant-cloud.md).
/// Shared by the cloud-API and the generic db/get paths so the operator only
/// ever sees endpoints in their own tenant.

#include <map>
#include <string>

namespace http_server {

class SessionStore;

/// The tenant a request acts as: the validated session's tenant, or "default"
/// when there is no/invalid session. Returns "*" (platform operator → no
/// filtering) when `auth` is null, preserving legacy behaviour where auth isn't
/// wired.
std::string request_tenant(const std::map<std::string, std::string>& headers,
                           SessionStore* auth);

/// Filter a `cloud.endpoints` JSON-array *string* to rows whose tenant ==
/// `tenant` (a row's absent/empty tenant counts as "default"). Returns the
/// input unchanged when `tenant` is "*" or the value isn't a JSON array — so a
/// platform operator and non-endpoint values pass through untouched.
std::string scope_endpoints_json(const std::string& value,
                                 const std::string& tenant);

} // namespace http_server

#endif /* __iot_http_tenant_scope_hpp__ */
