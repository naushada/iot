#ifndef __cellular_sms_pdu_hpp__
#define __cellular_sms_pdu_hpp__

#include <string>

/**
 * @file sms_pdu.hpp
 * @brief Pure GSM 03.40 SMS-DELIVER PDU decoder (mobile-terminated SMS).
 *
 * The mangOH Yellow's WP module delivers received SMS as a hex TPDU string
 * (`AT+CMGF=0` PDU mode) — via `AT+CMGR=<i>` after a `+CMTI` URC, an `AT+CMGL`
 * listing, or a direct `+CMT` deliver. `decode_sms_deliver` turns that hex into
 * a typed message: originating address, decoded UTF-8 body (GSM 7-bit default
 * alphabet or UCS2 per TP-DCS), the service-centre timestamp, and — when the
 * message carries a User-Data-Header concatenation IE — the multipart
 * (ref/part/total) fields so a higher layer can reassemble it.
 *
 * Pure/ACE-free and host-unit-testable: no serial I/O, no modem. The daemon
 * (SmsReceiver, PR-B) owns the URC state machine and feeds the hex here.
 *
 * References: 3GPP TS 23.040 (TPDU layout), TS 23.038 (DCS + 7-bit alphabet).
 */

namespace cellular {

/// A decoded mobile-terminated SMS. For a single-part message `total` is 0; for
/// one part of a concatenated message `ref`/`part`/`total` carry the UDH IE and
/// `text` is just that part's fragment (reassembly is the caller's job).
struct SmsMessage {
    std::string sender;         ///< originating address, "+E.164" (or alpha tag)
    std::string text;           ///< decoded body as UTF-8
    std::string scts;           ///< service-centre timestamp, "20YY-MM-DDTHH:MM:SS"
    int         index = -1;     ///< storage slot for AT+CMGD; -1 = direct-deliver
    int         ref   = 0;      ///< concat reference (UDH IE 0x00/0x08); 0 if none
    int         part  = 0;      ///< 1-based part number; 0 if single-part
    int         total = 0;      ///< total parts; 0 if single-part
};

/// Decode a hex SMS-DELIVER TPDU (the string after `+CMGR:`/`+CMGL:`/`+CMT:`)
/// into `out`. Leaves `out.index` untouched (the receiver sets it). Returns
/// false on malformed/short/truncated input or a non-DELIVER PDU.
bool decode_sms_deliver(const std::string& pdu_hex, SmsMessage& out);

/// Encode an SMS-SUBMIT PDU for `AT+CMGS` (mobile-originated send). `to` is the
/// recipient in "+E.164" (leading '+' → international) or bare national digits;
/// `utf8_text` is the body (GSM 7-bit default alphabet when every character
/// maps, else UCS2). Sets `pdu_hex` (uppercase, SMSC field = "00" = use the
/// SIM default) and `tpdu_len` = the octet count EXCLUDING that SMSC field, which
/// is the number `AT+CMGS=<tpdu_len>` expects. Returns false on an empty/invalid
/// recipient. See apps/docs/tdd-mangoh-cellular-sms.md.
bool encode_sms_submit(const std::string& to, const std::string& utf8_text,
                       std::string& pdu_hex, int& tpdu_len);

} // namespace cellular

#endif /*__cellular_sms_pdu_hpp__*/
