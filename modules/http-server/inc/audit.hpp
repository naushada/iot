#ifndef __iot_http_audit_hpp__
#define __iot_http_audit_hpp__

/// Operator audit log (multi-tenant cloud, tdd-multi-tenant-cloud.md P5c).
///
/// A capped, newest-first JSON array (`cloud.audit.log`) recording who did
/// what: platform-operator + provisioning actions captured at the iot-httpd
/// db/set layer, where the session (actor + tenant) is known. Pure helpers so
/// the array math is unit-testable with no ds/HTTP.

#include <cstddef>
#include <string>

namespace http_server {

/// One audit record. `ts` is a unix epoch (seconds); the caller stamps it
/// (handlers may use std::time) so this stays pure/testable.
struct AuditEntry {
    long        ts = 0;
    std::string actor;    ///< session username, or "system" when unauthenticated
    std::string tenant;   ///< actor's tenant ("*" = platform operator)
    std::string action;   ///< e.g. "device.provision", "tenant.update"
    std::string target;   ///< the thing acted on (serial, tenant id, …)
    std::string detail;   ///< optional extra context
};

/// Map a mutated ds key to an audit action label. Returns "" when the key is
/// not auditable (the common case — most db/set writes aren't operator actions).
/// Used by the db/set handler to decide whether to record an entry.
std::string audit_action_for_key(const std::string& key);

/// Prepend `e` to the `cloud.audit.log` JSON array string (newest-first) and
/// trim to at most `cap` entries. A non-array / unparseable input is treated as
/// an empty log (so a first write or a corrupt value self-heals). Returns the
/// new array as a compact JSON string.
std::string append_audit(const std::string& array_json, const AuditEntry& e,
                         std::size_t cap = 500);

}  // namespace http_server

#endif  // __iot_http_audit_hpp__
