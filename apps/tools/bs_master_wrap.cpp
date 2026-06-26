/// bs-master-wrap — operator tool to KMS-wrap the zero-touch HKDF master.
///
/// Zero-touch bootstrap (apps/docs/tdd-bs-hkdf-zerotouch.md). Produces the
/// AES-256-GCM envelope that goes into the cloud image's `cloud.bs.master.key`
/// default (via bs_master.lua → gen_bs_master.py). One-time cloud-setup step,
/// run where the operator holds the two secrets:
///
///   - the **master** (HKDF root; also fed to the device flashing tool
///     gen_bs_psk.py — keep it in your manufacturing vault), and
///   - the **KEK** (wraps the master; delivered to the cloud BS server at
///     runtime via IOT_BS_MASTER_KEK / a systemd credential — NEVER baked).
///
/// The output base64 blob is ciphertext: useless without the KEK.
///
/// Usage:
///   bs-master-wrap <kek> <master>
///   bs-master-wrap <kek> --gen-master      # mint a random master, print it
///
/// <kek> and <master> are 64-hex (32-byte) values, or `@/path` to read the
/// first line of a file. Prints the base64 envelope to stdout.

#include <cstdio>
#include <fstream>
#include <string>

#include "psk_gen.hpp"

namespace {

std::string read_arg(const std::string& a) {
    if (!a.empty() && a[0] == '@') {
        std::ifstream f(a.substr(1));
        std::string line;
        if (f.is_open()) std::getline(f, line);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r' ||
                line.back() == ' '  || line.back() == '\t'))
            line.pop_back();
        return line;
    }
    return a;
}

int usage() {
    std::fprintf(stderr,
        "usage: bs-master-wrap <kek> <master|--gen-master>\n"
        "  <kek>/<master>: 64-hex (32-byte), or @/path to read from a file\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return usage();

    const std::string kekHex = read_arg(argv[1]);
    std::string masterHex;
    bool generated = false;
    if (std::string(argv[2]) == "--gen-master") {
        masterHex = iot::generate_psk_hex(32);
        generated = true;
    } else {
        masterHex = read_arg(argv[2]);
    }

    // Fresh random 12-byte nonce per wrap (GCM must never reuse a nonce/key).
    const std::string nonceHex = iot::generate_psk_hex(12);
    const std::string blob = iot::wrap_bs_master(kekHex, masterHex, nonceHex);
    if (blob.empty()) {
        std::fprintf(stderr,
            "bs-master-wrap: wrap failed — KEK must be 64-hex (32 bytes) and "
            "master valid hex\n");
        return 1;
    }

    // Sanity: confirm it unwraps back under the same KEK before emitting.
    auto check = iot::unwrap_bs_master_hex(kekHex, blob);
    if (!check || *check != masterHex) {
        std::fprintf(stderr, "bs-master-wrap: self-check failed\n");
        return 1;
    }

    if (generated)
        std::fprintf(stderr,
            "bs-master-wrap: generated master (SAVE THIS — needed by "
            "gen_bs_psk.py to flash devices):\n%s\n", masterHex.c_str());

    std::printf("%s\n", blob.c_str());   // base64 envelope → bs_master.lua
    return 0;
}
