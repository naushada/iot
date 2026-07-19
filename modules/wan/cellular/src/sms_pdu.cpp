#include "sms_pdu.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace cellular {

namespace {

// ── hex → bytes ──────────────────────────────────────────────────────────────

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parse an even-length hex string into bytes. False on odd length / bad digit.
bool hex_to_bytes(const std::string& hex, std::vector<std::uint8_t>& out) {
    if (hex.empty() || (hex.size() % 2) != 0) return false;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

// ── UTF-8 output ─────────────────────────────────────────────────────────────

void utf8_append(std::string& s, std::uint32_t cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ── GSM 03.38 7-bit default alphabet ─────────────────────────────────────────

/// Basic character set: 7-bit value (0..127) → Unicode code point.
const std::uint16_t kGsm7Basic[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, // 00-07
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5, // 08-0F
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, // 10-17
    0x03A3, 0x0398, 0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9, // 18-1F  (1B=ESC)
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027, // 20-27
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, // 28-2F
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, // 30-37
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, // 38-3F
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, // 40-47
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F, // 48-4F
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, // 50-57
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7, // 58-5F
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, // 60-67
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, // 68-6F
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, // 70-77
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0, // 78-7F
};

/// Extension table (a septet following the 0x1B escape). 0 = not defined.
std::uint32_t gsm7_ext(std::uint8_t v) {
    switch (v) {
        case 0x0A: return 0x000C;   // form feed
        case 0x14: return 0x005E;   // ^
        case 0x28: return 0x007B;   // {
        case 0x29: return 0x007D;   // }
        case 0x2F: return 0x005C;   // backslash
        case 0x3C: return 0x005B;   // [
        case 0x3D: return 0x007E;   // ~
        case 0x3E: return 0x005D;   // ]
        case 0x40: return 0x007C;   // |
        case 0x65: return 0x20AC;   // euro
        default:   return 0;
    }
}

/// Unpack `count` GSM 7-bit septets from `data`, starting `skip_bits` into the
/// stream (LSB-first packing per TS 23.038). Out-of-range reads stop early.
std::vector<std::uint8_t> gsm7_unpack(const std::uint8_t* data, std::size_t nbytes,
                                      std::size_t skip_bits, int count) {
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(count > 0 ? count : 0));
    std::size_t bitpos = skip_bits;
    for (int i = 0; i < count; ++i) {
        const std::size_t byte_index = bitpos / 8;
        const std::size_t bit_offset = bitpos % 8;
        if (byte_index >= nbytes) break;
        unsigned value = static_cast<unsigned>(data[byte_index]) >> bit_offset;
        const std::size_t avail = 8 - bit_offset;
        if (avail < 7 && byte_index + 1 < nbytes) {
            value |= static_cast<unsigned>(data[byte_index + 1]) << avail;
        }
        out.push_back(static_cast<std::uint8_t>(value & 0x7F));
        bitpos += 7;
    }
    return out;
}

/// Map a run of GSM 7-bit septets (post-unpack) to UTF-8, honouring the escape.
std::string gsm7_to_utf8(const std::vector<std::uint8_t>& septets) {
    std::string out;
    for (std::size_t i = 0; i < septets.size(); ++i) {
        const std::uint8_t v = septets[i];
        if (v == 0x1B) {                       // escape → extension table
            if (i + 1 < septets.size()) {
                const std::uint32_t cp = gsm7_ext(septets[++i]);
                utf8_append(out, cp ? cp : 0x0020);   // undefined → space
            }
            continue;
        }
        utf8_append(out, kGsm7Basic[v]);
    }
    return out;
}

// ── addresses, timestamp, DCS ────────────────────────────────────────────────

/// Decode a swapped-BCD numeric address (`+E.164` when international).
std::string decode_number(const std::uint8_t* p, int semi_octets, bool international) {
    std::string digits;
    const int octets = (semi_octets + 1) / 2;
    for (int i = 0; i < octets; ++i) {
        digits.push_back(static_cast<char>('0' + (p[i] & 0x0F)));
        digits.push_back(static_cast<char>('0' + (p[i] >> 4)));
    }
    if (static_cast<int>(digits.size()) > semi_octets) digits.resize(semi_octets);  // drop 'F' pad
    return (international ? "+" : "") + digits;
}

/// Decode an alphanumeric address (TON 5): the octets are 7-bit packed text.
std::string decode_alpha(const std::uint8_t* p, int semi_octets) {
    const int octets = (semi_octets + 1) / 2;
    const int septets = (semi_octets * 4) / 7;
    return gsm7_to_utf8(gsm7_unpack(p, static_cast<std::size_t>(octets), 0, septets));
}

/// Decode the 7-octet TP-SCTS (swapped BCD) to "20YY-MM-DDTHH:MM:SS".
std::string decode_scts(const std::uint8_t* p) {
    auto bcd = [](std::uint8_t b) { return (b & 0x0F) * 10 + (b >> 4); };
    char buf[24];
    std::snprintf(buf, sizeof(buf), "20%02d-%02d-%02dT%02d:%02d:%02d",
                  bcd(p[0]), bcd(p[1]), bcd(p[2]), bcd(p[3]), bcd(p[4]), bcd(p[5]));
    return std::string(buf);
}

enum class Alphabet { Gsm7, Eight, Ucs2 };

/// Map TP-DCS (TS 23.038 §4) to the body alphabet.
Alphabet dcs_alphabet(std::uint8_t dcs) {
    if ((dcs & 0xC0) == 0x00) {                    // general data coding
        switch ((dcs >> 2) & 0x03) {
            case 1:  return Alphabet::Eight;
            case 2:  return Alphabet::Ucs2;
            default: return Alphabet::Gsm7;        // 0 and reserved 3 → GSM7
        }
    }
    if ((dcs & 0xF0) == 0xE0) return Alphabet::Ucs2;      // message-waiting, UCS2
    if ((dcs & 0xF0) == 0xF0) return (dcs & 0x04) ? Alphabet::Eight : Alphabet::Gsm7;
    return Alphabet::Gsm7;
}

/// Parse a User-Data-Header for the concatenation IE (0x00 8-bit / 0x08 16-bit
/// reference). Sets ref/part/total; other IEs are skipped. `udh` excludes the
/// leading UDHL octet.
void parse_udh_concat(const std::uint8_t* udh, int udhl, SmsMessage& out) {
    int i = 0;
    while (i + 2 <= udhl) {
        const std::uint8_t iei = udh[i];
        const int iedl = udh[i + 1];
        const int data = i + 2;
        if (data + iedl > udhl) break;             // malformed IE — stop
        if (iei == 0x00 && iedl == 3) {            // concat, 8-bit reference
            out.ref   = udh[data];
            out.total = udh[data + 1];
            out.part  = udh[data + 2];
        } else if (iei == 0x08 && iedl == 4) {     // concat, 16-bit reference
            out.ref   = (udh[data] << 8) | udh[data + 1];
            out.total = udh[data + 2];
            out.part  = udh[data + 3];
        }
        i = data + iedl;
    }
}

} // namespace

bool decode_sms_deliver(const std::string& pdu_hex, SmsMessage& out) {
    std::vector<std::uint8_t> b;
    if (!hex_to_bytes(pdu_hex, b)) return false;

    std::size_t pos = 0;
    auto need = [&](std::size_t n) { return pos + n <= b.size(); };

    // 1. SMSC info: length octet, then that many octets (type + number). Skip.
    if (!need(1)) return false;
    const std::size_t smsc_len = b[pos++];
    if (!need(smsc_len)) return false;
    pos += smsc_len;

    // 2. First TPDU octet: TP-MTI (bits 0-1) must be 00 (SMS-DELIVER).
    if (!need(1)) return false;
    const std::uint8_t first = b[pos++];
    if ((first & 0x03) != 0x00) return false;      // not a DELIVER PDU
    const bool udhi = (first & 0x40) != 0;

    // 3. TP-OA: length (semi-octets), type-of-address, address value.
    if (!need(2)) return false;
    const int oa_len = b[pos++];
    const std::uint8_t toa = b[pos++];
    const int ton = (toa >> 4) & 0x07;
    const bool alpha = (ton == 5);
    const int oa_octets = alpha ? ((((oa_len * 4) / 7) * 7 + 7) / 8)  // packed septets → octets
                                : ((oa_len + 1) / 2);
    if (!need(static_cast<std::size_t>(oa_octets))) return false;
    out.sender = alpha ? decode_alpha(&b[pos], oa_len)
                       : decode_number(&b[pos], oa_len, ton == 1);
    pos += oa_octets;

    // 4-6. TP-PID, TP-DCS, TP-SCTS (7 octets).
    if (!need(1 + 1 + 7)) return false;
    pos += 1;                                       // TP-PID (unused)
    const std::uint8_t dcs = b[pos++];
    out.scts = decode_scts(&b[pos]);
    pos += 7;

    // 7. TP-UDL + TP-UD.
    if (!need(1)) return false;
    const int udl = b[pos++];
    const Alphabet alphabet = dcs_alphabet(dcs);

    // User-data byte length: septet-packed for GSM7, raw octets otherwise.
    const std::size_t ud_bytes = (alphabet == Alphabet::Gsm7)
        ? static_cast<std::size_t>((udl * 7 + 7) / 8)
        : static_cast<std::size_t>(udl);
    if (!need(ud_bytes)) return false;
    const std::uint8_t* ud = b.data() + pos;

    // UDH (optional): UDHL octet + header; sets concat ref/part/total.
    std::size_t udh_octets = 0;      // whole header incl. the UDHL octet
    if (udhi) {
        if (ud_bytes < 1) return false;
        const int udhl = ud[0];
        udh_octets = static_cast<std::size_t>(1 + udhl);
        if (udh_octets > ud_bytes) return false;
        parse_udh_concat(ud + 1, udhl, out);
    }

    // Body: skip the UDH, then decode per alphabet.
    switch (alphabet) {
        case Alphabet::Gsm7: {
            // GSM7 septets must start on a septet boundary after the UDH, so the
            // header is padded with fill bits up to the next multiple of 7.
            const std::size_t skip_bits = udh_octets * 8;
            const std::size_t fill = (7 - (skip_bits % 7)) % 7;
            const int skip_septets = static_cast<int>((skip_bits + fill) / 7);
            const int msg_septets = udl - skip_septets;
            if (msg_septets > 0) {
                out.text = gsm7_to_utf8(
                    gsm7_unpack(ud, ud_bytes, skip_bits + fill, msg_septets));
            }
            break;
        }
        case Alphabet::Ucs2: {
            // UTF-16BE code units → UTF-8 (with surrogate-pair combining).
            std::uint32_t high = 0;
            for (std::size_t i = udh_octets; i + 1 < ud_bytes; i += 2) {
                const std::uint32_t u = (static_cast<std::uint32_t>(ud[i]) << 8) | ud[i + 1];
                if (u >= 0xD800 && u <= 0xDBFF) { high = u; continue; }
                if (u >= 0xDC00 && u <= 0xDFFF && high) {
                    utf8_append(out.text, 0x10000 + ((high - 0xD800) << 10) + (u - 0xDC00));
                    high = 0;
                } else {
                    utf8_append(out.text, u);
                }
            }
            break;
        }
        case Alphabet::Eight:
            // 8-bit / binary — pass the octets through as raw bytes.
            for (std::size_t i = udh_octets; i < ud_bytes; ++i) {
                out.text.push_back(static_cast<char>(ud[i]));
            }
            break;
    }
    return true;
}

// ── SMS-SUBMIT encode (MO send) ──────────────────────────────────────────────

namespace {

/// Decode UTF-8 `s` into Unicode code points. Invalid bytes pass through as
/// U+FFFD-ish (the byte value) — good enough for the SMS body path.
std::vector<std::uint32_t> utf8_decode(const std::string& s) {
    std::vector<std::uint32_t> cps;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp; int n;
        if (c < 0x80)        { cp = c;          n = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; n = 4; }
        else { cps.push_back(c); ++i; continue; }
        if (i + static_cast<std::size_t>(n) > s.size()) { cps.push_back(c); ++i; continue; }
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { cps.push_back(c); ++i; continue; }
        cps.push_back(cp);
        i += static_cast<std::size_t>(n);
    }
    return cps;
}

/// Map a Unicode code point to a GSM 7-bit septet (or 0x1B + extension septet).
/// Returns false if `cp` is not representable in the GSM 7-bit alphabet.
bool gsm7_septets_for(std::uint32_t cp, std::vector<std::uint8_t>& out) {
    for (int i = 0; i < 128; ++i)
        if (kGsm7Basic[i] == cp && i != 0x1B) { out.push_back(static_cast<std::uint8_t>(i)); return true; }
    // extension table (reverse of gsm7_ext)
    struct E { std::uint8_t sept; std::uint32_t cp; };
    static const E kExt[] = {
        {0x0A,0x000C},{0x14,0x005E},{0x28,0x007B},{0x29,0x007D},{0x2F,0x005C},
        {0x3C,0x005B},{0x3D,0x007E},{0x3E,0x005D},{0x40,0x007C},{0x65,0x20AC},
    };
    for (const auto& e : kExt)
        if (e.cp == cp) { out.push_back(0x1B); out.push_back(e.sept); return true; }
    return false;
}

/// Pack GSM 7-bit septets LSB-first into octets (reverse of gsm7_unpack).
std::vector<std::uint8_t> gsm7_pack(const std::vector<std::uint8_t>& septets) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int nbits = 0;
    for (std::uint8_t s : septets) {
        acc |= static_cast<std::uint32_t>(s & 0x7F) << nbits;
        nbits += 7;
        while (nbits >= 8) { out.push_back(static_cast<std::uint8_t>(acc & 0xFF)); acc >>= 8; nbits -= 8; }
    }
    if (nbits > 0) out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
    return out;
}

void put_hex(std::string& s, std::uint8_t b) {
    static const char* H = "0123456789ABCDEF";
    s.push_back(H[b >> 4]);
    s.push_back(H[b & 0x0F]);
}

} // namespace

bool encode_sms_submit(const std::string& to, const std::string& utf8_text,
                       std::string& pdu_hex, int& tpdu_len) {
    // Recipient digits + type-of-address.
    bool intl = false;
    std::string digits;
    for (char c : to) {
        if (c == '+') intl = true;
        else if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.empty()) return false;

    std::vector<std::uint8_t> tpdu;
    tpdu.push_back(0x31);                       // SUBMIT, VPF=relative, TP-SRR set
    tpdu.push_back(0x00);                       // TP-MR (modem overrides w/ +CMGS mr)
    tpdu.push_back(static_cast<std::uint8_t>(digits.size()));   // TP-DA length (digits)
    tpdu.push_back(intl ? 0x91 : 0x81);         // TP-TOA
    for (std::size_t i = 0; i < digits.size(); i += 2) {        // swapped BCD
        const std::uint8_t lo = static_cast<std::uint8_t>(digits[i] - '0');
        const std::uint8_t hi = (i + 1 < digits.size())
            ? static_cast<std::uint8_t>(digits[i + 1] - '0') : 0x0F;
        tpdu.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    tpdu.push_back(0x00);                        // TP-PID

    // Body: GSM 7-bit if every char maps, else UCS2.
    const std::vector<std::uint32_t> cps = utf8_decode(utf8_text);
    std::vector<std::uint8_t> septets;
    bool gsm7 = true;
    for (std::uint32_t cp : cps)
        if (!gsm7_septets_for(cp, septets)) { gsm7 = false; break; }

    if (gsm7) {
        tpdu.push_back(0x00);                    // TP-DCS = GSM7
        tpdu.push_back(0xAA);                    // TP-VP relative (~4 days)
        tpdu.push_back(static_cast<std::uint8_t>(septets.size()));   // TP-UDL (septets)
        for (std::uint8_t b : gsm7_pack(septets)) tpdu.push_back(b);
    } else {
        std::vector<std::uint8_t> ud;            // UTF-16BE
        for (std::uint32_t cp : cps) {
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                const std::uint16_t hi = static_cast<std::uint16_t>(0xD800 + (cp >> 10));
                const std::uint16_t lo = static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF));
                ud.push_back(hi >> 8); ud.push_back(hi & 0xFF);
                ud.push_back(lo >> 8); ud.push_back(lo & 0xFF);
            } else {
                ud.push_back(static_cast<std::uint8_t>(cp >> 8));
                ud.push_back(static_cast<std::uint8_t>(cp & 0xFF));
            }
        }
        tpdu.push_back(0x08);                    // TP-DCS = UCS2
        tpdu.push_back(0xAA);                    // TP-VP
        tpdu.push_back(static_cast<std::uint8_t>(ud.size()));        // TP-UDL (octets)
        for (std::uint8_t b : ud) tpdu.push_back(b);
    }

    tpdu_len = static_cast<int>(tpdu.size());    // AT+CMGS length excludes the SMSC octet
    pdu_hex.clear();
    put_hex(pdu_hex, 0x00);                       // SMSC = use the SIM default
    for (std::uint8_t b : tpdu) put_hex(pdu_hex, b);
    return true;
}

bool decode_status_report(const std::string& pdu_hex, SmsStatusReport& out) {
    std::vector<std::uint8_t> b;
    if (!hex_to_bytes(pdu_hex, b)) return false;

    std::size_t i = 0;
    // SMSC address: length octet counts the following type + digit octets.
    if (i >= b.size()) return false;
    i += 1 + b[i];
    // First octet — TP-MTI (bits 0-1) == 0b10 marks an SMS-STATUS-REPORT.
    if (i >= b.size() || (b[i] & 0x03) != 0x02) return false;
    ++i;
    if (i >= b.size()) return false;
    out.mr = b[i++];                              // TP-MR (matches +CMGS mr)
    // TP-RA (recipient address): semi-octet count → (n+1)/2 digit octets, plus
    // the type-of-address octet. We don't need the digits, just skip past them.
    if (i >= b.size()) return false;
    const std::size_t ra_digits = b[i++];
    i += 1 + (ra_digits + 1) / 2;                 // TOA + BCD digits
    i += 7;                                       // TP-SCTS (service-centre time)
    i += 7;                                       // TP-DT (discharge time)
    if (i >= b.size()) return false;
    out.status    = b[i];                         // TP-ST
    out.delivered = out.status < 0x20;            // 0x00-0x1F: completed
    out.pending   = out.status >= 0x20 && out.status < 0x40;
    out.failed    = out.status >= 0x40;           // permanent error / SC gave up
    return true;
}

std::string status_report_text(const SmsStatusReport& rpt) {
    char st[8];
    std::snprintf(st, sizeof st, "0x%02X",
                  rpt.status < 0 ? 0 : rpt.status & 0xFF);
    if (rpt.delivered) return "delivered";
    if (rpt.pending)   return std::string("pending (SC retrying, ST=") + st + ")";
    if (rpt.failed)    return std::string("failed: undeliverable (ST=") + st + ")";
    return std::string("unknown (ST=") + st + ")";
}

} // namespace cellular
