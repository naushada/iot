#ifndef __wifi_client_ctrl_hpp__
#define __wifi_client_ctrl_hpp__

/// wpa_supplicant local control protocol wrapper (L15/D4).
///
/// Two halves:
///
///   - `ctrl::Parser` — pure logic. Classifies one wpa_supplicant
///     CTRL-EVENT line into a typed CtrlEvent. No sockets, no I/O,
///     no ACE. Unit-tested via canned event strings.
///   - `ctrl::Client` — thin ACE wrapper around the unix-DGRAM
///     socket wpa_supplicant exposes at <ctrl_dir>/<iface>. Owns
///     the socket lifetime + the bind/connect dance every
///     wpa_ctrl client does. Wire-tested in log/L15/smoke.sh
///     against fake-wpa.sh (D8).
///
/// Protocol reference: wpa_supplicant's own `wpa_ctrl.c`. The
/// socket is SOCK_DGRAM, not SOCK_STREAM — the plan's mention of
/// LSOCK_Stream was a misread; ACE_LSOCK_Dgram is the right
/// primitive. Each request = one datagram out, one datagram in.
/// Unsolicited events arrive as additional datagrams once the
/// client has sent `ATTACH`.
///
/// Event lines look like:
///
///   <3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff completed [id=0 id_str=]
///   <3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:dd:ee:ff reason=3
///   <3>CTRL-EVENT-SCAN-RESULTS
///   <3>CTRL-EVENT-SCAN-STARTED
///   <3>CTRL-EVENT-ASSOC-REJECT bssid=aa:bb:cc:dd:ee:ff status_code=17
///   <3>CTRL-EVENT-AUTH-REJECT bssid=aa:bb:cc:dd:ee:ff auth_type=0 auth_transaction=2 status_code=15
///   <3>CTRL-EVENT-TERMINATING
///
/// The leading `<N>` is a syslog-like priority indicator wpa_ctrl
/// inserts; the parser strips it before classification.

#include <optional>
#include <string>
#include <string_view>

namespace wifi_client::ctrl {

/// One classified event line. `raw` holds the full stripped-prefix
/// line for log + debug; the typed fields are populated where the
/// event line carries them. Unrecognised lines map to `Unknown` so
/// callers don't have to defend against malformed input crashing
/// the loop.
struct CtrlEvent {
    enum class Kind {
        ScanStarted,       ///< CTRL-EVENT-SCAN-STARTED
        ScanResults,       ///< CTRL-EVENT-SCAN-RESULTS
        Connected,         ///< CTRL-EVENT-CONNECTED (handshake done)
        Disconnected,      ///< CTRL-EVENT-DISCONNECTED
        AssocReject,       ///< CTRL-EVENT-ASSOC-REJECT
        AuthReject,        ///< CTRL-EVENT-AUTH-REJECT
        Terminating,       ///< CTRL-EVENT-TERMINATING (wpa_supplicant exiting)
        Unknown,           ///< didn't match any known kind
    };

    Kind        kind = Kind::Unknown;
    std::string raw;            ///< the full event line (priority stripped)
    std::string ssid;           ///< Connected: SSID if the event carries it
    std::string bssid;          ///< Connected/*Reject/Disconnected: peer BSSID
    std::string reason;         ///< Disconnected/*Reject: reason/status code
};

/// Pure parser. No I/O, no state — classify(line) is a function.
class Parser {
public:
    /// Classify one event line. The input MAY have a leading
    /// "<N>" priority indicator (wpa_ctrl prepends it on real
    /// events); the parser strips it before matching.
    /// Trailing "\r" / "\n" stripped too.
    static CtrlEvent classify(std::string_view line);

private:
    /// Split "key=value" tokens out of the post-prefix tail of an
    /// event line. Test-helper only — header keeps it private.
    friend struct ParserAccess;
};

// Forward declaration for the (optional) socket wrapper. The
// header keeps the type incomplete so callers that only need the
// parser don't have to include ACE headers transitively. The full
// definition lives in ctrl.cpp.
class Client;

/// Open a unix-DGRAM socket to wpa_supplicant's control path, do
/// the bind/connect/ATTACH dance, and surface a request/recv API.
/// Implementation in ctrl.cpp; declared here so the Supervisor can
/// forward-declare without pulling ACE into its own header.
///
/// The class is INTENTIONALLY non-virtual + concrete: there's no
/// reason a Supervisor test needs to mock it — the protocol is
/// stable, the harder thing is generating realistic event traces,
/// which is exactly what fake-wpa.sh does at D8.
class Client {
public:
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    /// Open + bind the local DGRAM socket, connect to
    /// `<server_sock_path>`, send "ATTACH\n" so wpa_supplicant
    /// starts forwarding unsolicited events. Returns true on
    /// every step succeeding. On failure leaves the socket
    /// closed; `connected()` reflects the new state.
    bool connect(const std::string& server_sock_path);

    bool connected() const;

    /// Send `cmd` (terminator added if missing); receive the
    /// reply datagram and store its trimmed contents in `reply`.
    /// Returns true on a reply that isn't `FAIL`; false on a
    /// FAIL reply OR a socket error. On socket error, the error
    /// path leaves the client connected — caller can retry; on
    /// repeated failure the caller closes + reconnects.
    bool request(const std::string& cmd, std::string& reply);

    /// Receive one event datagram with a deadline. Returns
    /// nullopt on timeout. Caller drives this in a poll loop.
    /// Both unsolicited events AND command replies that arrive
    /// out-of-band (rare; usually only on ATTACH) flow through
    /// here; the parser classifies whatever it sees.
    std::optional<CtrlEvent> recv_event(int timeout_ms);

    /// Close the socket + unlink the local bind path. Idempotent.
    void close();

private:
    struct Impl;
    Impl* m_impl;  // raw ptr so the header doesn't need <memory>
};

} // namespace wifi_client::ctrl

#endif /* __wifi_client_ctrl_hpp__ */
