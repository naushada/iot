#ifndef __net_router_apply_hpp__
#define __net_router_apply_hpp__

/// Wrapper around `nft -f <file>` for atomic ruleset apply.
///
/// Pure-ish: the NftApply callable type is injectable so tests can
/// stub it. The default impl writes the ruleset to a tempfile in
/// $TMPDIR (or /tmp) and invokes `nft` via popen() with combined
/// stdout+stderr captured so error messages survive into the daemon
/// log on rejection.

#include <functional>
#include <string>

namespace net_router::apply {

/// Returns true if nft exited 0. On non-zero exit (or spawn failure)
/// `*err` (when non-null) receives the captured combined output of
/// nft so the daemon can ACE_ERROR the parser diagnostic verbatim.
using NftApply = std::function<bool(const std::string& ruleset,
                                    std::string*       err)>;

/// Default impl: writes ruleset to a tempfile, runs
/// `<nft_path> -f <tempfile> 2>&1`, captures combined output,
/// unlinks the tempfile (best-effort). Returns true on exit code 0.
NftApply default_nft_apply(const std::string& nft_path = "nft");

} // namespace net_router::apply

#endif /* __net_router_apply_hpp__ */
