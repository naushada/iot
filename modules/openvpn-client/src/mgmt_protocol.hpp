#ifndef __openvpn_client_mgmt_protocol_hpp__
#define __openvpn_client_mgmt_protocol_hpp__

/// OpenVPN management-interface protocol parser.
///
/// Pure logic — no sockets, no ACE, no I/O. The reactor-thread
/// wrapper (D6) feeds raw bytes from the mgmt socket; this parser
/// accumulates them into newline-delimited lines, classifies each
/// by the `>EVENT:` / `SUCCESS:` / `ERROR:` / `END` prefixes, and
/// emits one `Event` per fully-parsed line.
///
/// Reference: openvpn(8) man page §"Management interface", plus the
/// protocol notes in OpenVPN's `doc/management-notes.txt`.
///
/// Wire shape (asynchronous events have a `>` prefix; command
/// responses don't):
///
///   >INFO:OpenVPN Management Interface Version 1 -- type 'help' …
///   >STATE:1701420000,CONNECTED,SUCCESS,10.8.0.6,vpn.example.com,1194,,
///   >PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,ifconfig 10.8.0.6 255.255.255.0
///   >BYTECOUNT:12345,67890
///   >LOG:1701420000,N,Tunnel rebuilt
///   >HOLD:Waiting for hold release.
///   SUCCESS: hold release succeeded
///   ERROR: unknown command
///   END
///
/// The parser does NOT interpret semantics — STATE field 1 is just
/// `fields[1]`, PushReply options are just comma-split tokens. The
/// D6 lifecycle FSM is responsible for mapping `fields[1]=="CONNECTED"`
/// → vpn.state, etc. Keeping the parser semantic-free makes it
/// table-driven testable + immune to OpenVPN spec drift.

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openvpn_client::mgmt {

struct Event {
    enum class Kind {
        Banner,        ///< `>INFO:` — first line after connect
        State,         ///< `>STATE:` — periodic state notification
        PushReply,     ///< `>PUSH_REPLY:` — server pushed options
        ByteCount,     ///< `>BYTECOUNT:` — periodic tunnel stats
        Log,           ///< `>LOG:` — log line forwarded from openvpn
        Hold,          ///< `>HOLD:` — daemon paused; needs `hold release`
        Fatal,         ///< `>FATAL:` — terminal error from openvpn
        Echo,          ///< `>ECHO:` — push echo
        Password,      ///< `>PASSWORD:` — request for credentials
        NeedOk,        ///< `>NEED-OK:` — request to confirm something
        Client,        ///< `>CLIENT:` — server-mode client lifecycle
        SuccessReply,  ///< `SUCCESS:` — response to a command
        ErrorReply,    ///< `ERROR:` — response to a command
        EndMarker,     ///< bare `END` line — terminator of a multi-line response
        DataLine,      ///< any other non-prefixed line (response payload)
        Unknown,       ///< didn't match any of the above (caller decides what to do)
    };

    Kind kind = Kind::Unknown;

    /// Everything after the prefix (or the whole line for DataLine /
    /// EndMarker / Unknown). Trailing CR stripped.
    std::string raw;

    /// For State / PushReply / ByteCount: comma-split tokens.
    /// PushReply's tokens still need a second whitespace split to
    /// extract the option name + value — keep that in the consumer
    /// so this struct stays generic.
    std::vector<std::string> fields;
};

class Parser {
public:
    /// Append `bytes` to the internal line buffer. Any complete
    /// newline-delimited lines are immediately classified and
    /// enqueued for `next()`.
    void feed(std::string_view bytes);

    /// Pop the next ready event in FIFO order. Returns nullopt
    /// when nothing is queued (caller waits for more bytes).
    std::optional<Event> next();

    /// Diagnostic: how many bytes are still sitting in the line
    /// buffer waiting for a newline. Mostly useful for tests that
    /// want to assert a partial-feed didn't leak.
    std::size_t buffer_size() const { return m_buf.size(); }

private:
    std::string       m_buf;
    std::deque<Event> m_events;

    /// Classify a single, newline-stripped line and enqueue.
    void emit(std::string line);
};

/// Helper: split a `>PUSH_REPLY:` field (e.g. "ifconfig 10.8.0.6
/// 255.255.255.0") into option name + remaining whitespace-joined
/// value. Returns {name, value} pair; value is empty for bare
/// options like "redirect-gateway".
std::pair<std::string, std::string>
split_push_option(std::string_view option);

} // namespace openvpn_client::mgmt

#endif /* __openvpn_client_mgmt_protocol_hpp__ */
