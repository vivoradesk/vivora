// deskbeam-license-mint: tiny CLI tool for the manual side of the
// licensing flow.  Two modes:
//
//   keygen   — generate an Ed25519 keypair, write the 64-byte expanded
//              secret to <out>.sk and the 32-byte public key to <out>.pk.
//              The .pk file is what `deskbeam-relay --require-license`
//              consumes; the .sk file feeds back into this tool's `mint`.
//
//   mint     — take a .sk file plus claim parameters (tier, exp, subject)
//              and write a 95-byte signed license token to <out>.
//
// Both modes are deterministic except for keygen, which uses libc's
// random_device-equivalent through monocypher's seed expansion (we
// supply a 32-byte seed read from the OS RNG).
//
// This is a developer tool — the real production minting will live in
// the closed-source deskbeam-cloud service backed by a Paddle webhook.
// For now it lets us prepare a few test tokens by hand.

#include "common/crypto/license_token.h"
#include "common/crypto/random.h"
#include "monocypher.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage:\n"
        "  %s keygen <out_basename>\n"
        "      writes <out>.sk (64B secret) and <out>.pk (32B public)\n"
        "  %s mint --key <secret.sk> --out <token.bin>\n"
        "                [--tier trial|pro] [--exp UNIX] [--subject HEX32]\n"
        "      defaults: tier=pro, exp=now+90d, subject=zeros\n",
        argv0, argv0);
}

bool write_file(const char* path, const uint8_t* data, size_t len) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return f.good();
}

bool read_file_exact(const char* path, uint8_t* out, size_t n) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

bool hex_decode(const char* s, uint8_t* out, size_t out_len) {
    if (std::strlen(s) != out_len * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = nib(s[i * 2]);
        const int lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

int do_keygen(int argc, char** argv) {
    if (argc < 1) { usage("deskbeam-license-mint"); return 1; }
    const std::string base = argv[0];
    uint8_t seed[32];
    deskbeam::crypto::random_bytes(seed, 32);
    uint8_t sk[64];
    uint8_t pk[32];
    crypto_eddsa_key_pair(sk, pk, seed);   // monocypher consumes the seed

    const std::string sk_path = base + ".sk";
    const std::string pk_path = base + ".pk";
    if (!write_file(sk_path.c_str(), sk, 64)) {
        std::fprintf(stderr, "write %s failed\n", sk_path.c_str());
        return 1;
    }
    if (!write_file(pk_path.c_str(), pk, 32)) {
        std::fprintf(stderr, "write %s failed\n", pk_path.c_str());
        return 1;
    }
    std::printf("wrote %s (secret, 64B) and %s (public, 32B)\n",
                sk_path.c_str(), pk_path.c_str());
    return 0;
}

int do_mint(int argc, char** argv) {
    const char* key_path = nullptr;
    const char* out_path = nullptr;
    const char* tier_str = "pro";
    int64_t     exp_unix = 0;
    const char* subject_hex = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--key")    == 0 && i + 1 < argc) key_path = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (std::strcmp(argv[i], "--tier") == 0 && i + 1 < argc) tier_str = argv[++i];
        else if (std::strcmp(argv[i], "--exp")  == 0 && i + 1 < argc) exp_unix = std::atoll(argv[++i]);
        else if (std::strcmp(argv[i], "--subject") == 0 && i + 1 < argc) subject_hex = argv[++i];
    }
    if (!key_path || !out_path) {
        std::fprintf(stderr, "mint requires --key and --out\n");
        return 1;
    }

    uint8_t sk[64];
    if (!read_file_exact(key_path, sk, 64)) {
        std::fprintf(stderr, "failed to read 64B secret from %s\n", key_path);
        return 1;
    }
    deskbeam::crypto::LicenseClaims claims;
    if      (std::strcmp(tier_str, "pro")   == 0) claims.tier = deskbeam::crypto::LicenseTier::Pro;
    else if (std::strcmp(tier_str, "trial") == 0) claims.tier = deskbeam::crypto::LicenseTier::Trial;
    else { std::fprintf(stderr, "unknown --tier %s\n", tier_str); return 1; }
    claims.exp_unix = exp_unix > 0 ? exp_unix
                                   : (std::time(nullptr) + 90 * 86400);
    if (subject_hex) {
        if (!hex_decode(subject_hex, claims.subject, 16)) {
            std::fprintf(stderr, "bad --subject (need 32 hex chars = 16 bytes)\n");
            return 1;
        }
    }

    uint8_t token[deskbeam::crypto::LICENSE_TOKEN_SIZE];
    if (!deskbeam::crypto::sign_license(claims, sk, token)) {
        std::fprintf(stderr, "sign_license failed\n");
        return 1;
    }
    if (!write_file(out_path, token, sizeof(token))) {
        std::fprintf(stderr, "write %s failed\n", out_path);
        return 1;
    }
    std::printf("wrote %s (95B token, tier=%s, exp=%lld)\n",
                out_path, tier_str, static_cast<long long>(claims.exp_unix));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "keygen") return do_keygen(argc - 2, argv + 2);
    if (cmd == "mint")   return do_mint(argc - 2, argv + 2);
    usage(argv[0]);
    return 1;
}
