#ifndef __data_store_client_hpp__
#define __data_store_client_hpp__

/// Public client API for the data-store unix-socket service.
///
/// Applications link `libdatastore_client.a` and use this header to
/// talk to the running `ds-server` daemon. The header itself only
/// depends on the C++ standard library — ACE primitives live behind
/// a pimpl in `client.cpp`, so downstream apps don't need ACE on
/// their compile line (only on their link line, transitively from
/// the static lib).
///
/// Wire details live in proto.hpp; the design is in
/// modules/data-store/docs/design.md.
///
/// Threading model
/// ---------------
/// `connect()` spawns an internal listener thread. The listener
/// owns the read side of the socket: every '\n'-terminated line is
/// demuxed into either (a) a notify push (`{"ev":"changed",...}`)
/// fanned out to per-watch callbacks AND a pull-style event queue,
/// or (b) a response (`{"ok":...,"id":...}`) fulfilling the matching
/// pending-request promise.
///
/// Public methods (`set`/`get`/`watch`/`unwatch`/`recv_event`) are
/// safe to call from any thread; the listener thread is the sole
/// reader, request methods just send + wait on their promise.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "data_store/value.hpp"

namespace data_store {

using KV = std::pair<std::string, Value>;

/// Result of a connect() / set() / get() / ... call.
struct Status {
    bool        ok = true;
    int         code = 0;       // errno or 0
    std::string err;
};

class Client {
public:
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    /// Open the connection and start the listener thread. EMP has no
    /// welcome handshake — the server is ready to receive frames as
    /// soon as `connect()` returns ok.
    Status connect(std::string path = "");

    // --- ops --------------------------------------------------------

    /// `set` one or more key/value pairs atomically.
    Status set(const std::vector<KV>& pairs, std::int32_t timeout_ms = 1000);
    Status set(const std::string& k, Value v,
               std::int32_t timeout_ms = 1000) {
        return set(std::vector<KV>{{k, std::move(v)}}, timeout_ms);
    }

    struct GetResult {
        std::string key;
        Value       value;
        bool        has_value = false;
    };
    /// `get` one or more keys. `out` is filled in the same order as
    /// `keys`; missing keys come back with `has_value=false`.
    Status get(const std::vector<std::string>& keys,
               std::vector<GetResult>&         out,
               std::int32_t                    timeout_ms = 1000);

    // --- watch + callback ------------------------------------------

    /// Parsed notification event. `prev_has_value` distinguishes a
    /// real previous value from a freshly-created key.
    struct Event {
        std::string key;
        Value       value;
        Value       prev;
        bool        prev_has_value = false;
    };

    using EventCallback = std::function<void(const Event&)>;

    /// Opaque handle returned by `watch(keys, cb)`. Hand back to
    /// `unwatch(handle)` to drop this specific registration. The
    /// same set of keys can be watched many times (each call gets
    /// its own handle + callback); a key is unregistered on the
    /// wire only when the last local watcher drops it.
    using WatchHandle = std::uint64_t;
    static constexpr WatchHandle kInvalidHandle = 0;

    /// Register `cb` for value-change notifications on `keys`. The
    /// callback fires on the listener thread for every matching
    /// `ev=changed` event the server pushes. Returns the wire-level
    /// ack status; `*out_handle` is set on success.
    ///
    /// The same Client can call watch() multiple times with different
    /// keys + callbacks; all callbacks subscribed to a given key fire
    /// for every change to that key.
    Status watch(const std::vector<std::string>& keys,
                 EventCallback                   cb,
                 WatchHandle*                    out_handle = nullptr,
                 std::int32_t                    timeout_ms = 1000);
    Status watch(const std::string& key,
                 EventCallback      cb,
                 WatchHandle*       out_handle = nullptr,
                 std::int32_t       timeout_ms = 1000) {
        return watch(std::vector<std::string>{key},
                     std::move(cb), out_handle, timeout_ms);
    }

    /// Pull-style watch (no callback). Use `recv_event()` to drain
    /// the queued events. May be combined with callback-style watches
    /// on the same Client — events arrive on both paths.
    Status watch(const std::vector<std::string>& keys,
                 std::int32_t                    timeout_ms = 1000);
    Status watch(const std::string& key, std::int32_t timeout_ms = 1000) {
        return watch(std::vector<std::string>{key}, timeout_ms);
    }

    /// Drop a specific callback-style watch. The server-side `remove`
    /// fires only when this handle was the last local watcher for
    /// any of its keys.
    Status unwatch(WatchHandle handle, std::int32_t timeout_ms = 1000);

    /// Drop a pull-style watch (or any local registration on these
    /// keys, including callback ones if they were the only watchers).
    Status unwatch(const std::vector<std::string>& keys,
                   std::int32_t                    timeout_ms = 1000);
    Status unwatch(const std::string& key, std::int32_t timeout_ms = 1000) {
        return unwatch(std::vector<std::string>{key}, timeout_ms);
    }

    /// Pull one buffered event (filled by the listener thread). Use
    /// alongside the pull-style watch(); callback-style watches do
    /// NOT block this — events go to BOTH paths.
    Status recv_event(Event& out, std::int32_t timeout_ms = 1000);

    /// Close the connection + join the listener thread. Idempotent.
    void close();

    /// Underlying ACE_HANDLE / fd. `-1` when not connected.
    int fd() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace data_store

#endif /* __data_store_client_hpp__ */
