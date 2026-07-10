#include "line_router.hpp"

#include "at_parser.hpp"

namespace cellular {

namespace {
    bool starts_with(const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    }
    std::string trim(const std::string& s) {
        std::size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
        return s.substr(a, b - a);
    }
}

std::vector<std::string> LineAssembler::feed(const std::string& chunk) {
    std::vector<std::string> lines;
    m_buf += chunk;
    std::size_t start = 0;
    for (std::size_t i = 0; i < m_buf.size(); ++i) {
        if (m_buf[i] == '\n' || m_buf[i] == '\r') {
            std::string line = trim(m_buf.substr(start, i - start));
            if (!line.empty()) lines.push_back(line);
            start = i + 1;
        }
    }
    m_buf.erase(0, start);   // keep the partial tail
    return lines;
}

bool dispatch_at_line(const std::string& line, CellularState& st) {
    if (starts_with(line, "+CSQ:")) {
        st.set_signal(parse_csq(line));
        return true;
    }
    if (starts_with(line, "+COPS:")) {
        st.set_operator(parse_cops(line));
        return true;
    }
    if (starts_with(line, "+CREG:") || starts_with(line, "+CGREG:") ||
        starts_with(line, "+CEREG:")) {
        st.set_reg(parse_creg(line));
        return true;
    }
    if (starts_with(line, "+CGPADDR:")) {
        const std::string ip = parse_cgpaddr(line);
        if (!ip.empty()) st.set_ip(ip);
        return true;
    }
    if (starts_with(line, "+CGCONTRDP:")) {
        const std::string dns = parse_cgcontrdp_dns(line);
        if (!dns.empty()) st.set_dns(dns);
        return true;
    }
    // Quectel answers "+QCCID:", the 3GPP form is "+CCID:", and the Sierra WP7702
    // answers AT+ICCID with a BARE "ICCID: <digits>" — no leading '+'. Missing the
    // bare form left cell.iccid permanently empty on every WP module.
    if (starts_with(line, "+QCCID:") || starts_with(line, "+CCID:") ||
        starts_with(line, "+ICCID:") || starts_with(line, "ICCID:")) {
        st.set_iccid(parse_iccid(line));
        return true;
    }
    if (starts_with(line, "+QGPSLOC:")) {
        // GPS over the AT channel (AT+QGPSLOC=2) — one complete fix per line.
        GpsFix fix;
        if (parse_qgpsloc(line, fix)) st.set_gps(fix);
        return true;
    }
    return false;
}

bool dispatch_nmea_line(const std::string& line, GpsFix& acc, CellularState& st) {
    bool ok = false;
    if (line.find("GGA") != std::string::npos) {
        ok = parse_gga(line, acc);
    } else if (line.find("RMC") != std::string::npos) {
        ok = parse_rmc(line, acc);
    }
    if (ok) st.set_gps(acc);
    return ok;
}

} // namespace cellular
