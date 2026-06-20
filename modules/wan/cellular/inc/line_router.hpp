#ifndef __cellular_line_router_hpp__
#define __cellular_line_router_hpp__

#include <string>
#include <vector>

#include "cell_state.hpp"
#include "nmea_parser.hpp"

/**
 * @file line_router.hpp
 * @brief Pure byte-stream → line → parser routing for the modem channels.
 *
 * The ACE serial channels (PR-C daemon) read raw bytes off the AT / GNSS ttys
 * and feed them here; this layer reassembles CR/LF-delimited lines and routes
 * each complete line through the PR-A parsers into a CellularState. Keeping it
 * free of ACE/I/O makes the actual response handling host-unit-testable — the
 * ACE handle_input() becomes a one-liner.
 */

namespace cellular {

/// Reassembles complete CR/LF-terminated lines from arbitrary read chunks.
class LineAssembler {
    public:
        /// Append `chunk` and return any complete lines it produced (trimmed,
        /// empties dropped). Partial trailing data is retained for next time.
        std::vector<std::string> feed(const std::string& chunk);

    private:
        std::string m_buf;
};

/// Route one AT response line into `st` (updates signal/operator/reg/ip/iccid).
/// Returns true if the line matched a known URC/response.
bool dispatch_at_line(const std::string& line, CellularState& st);

/// Route one NMEA sentence into `acc` (the running fix) and publish it to `st`.
/// Returns true if the line was a recognised, checksum-valid GGA/RMC sentence.
bool dispatch_nmea_line(const std::string& line, GpsFix& acc, CellularState& st);

} // namespace cellular

#endif /*__cellular_line_router_hpp__*/
