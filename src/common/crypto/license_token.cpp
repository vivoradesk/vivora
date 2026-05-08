#include "common/crypto/license_token.h"

#include "monocypher.h"

#include <cstring>

namespace deskbeam::crypto {

namespace {
constexpr uint8_t MAGIC[4] = { 'D', 'B', 'L', 'T' };

inline void put_u64_be(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (56 - 8 * i)) & 0xff);
}
inline uint64_t get_u64_be(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Lay out the signed prefix into the buffer.  Returns the number of
// bytes filled (LICENSE_SIGNED_SIZE on success, 0 on bad input).
size_t layout_signed_prefix(const LicenseClaims& claims, uint8_t out[LICENSE_SIGNED_SIZE]) {
    std::memcpy(out, MAGIC, 4);
    out[4] = LICENSE_VERSION;
    out[5] = 0;                                          // reserved
    out[6] = static_cast<uint8_t>(claims.tier);
    put_u64_be(out + 7, static_cast<uint64_t>(claims.exp_unix));
    std::memcpy(out + 15, claims.subject, 16);
    return LICENSE_SIGNED_SIZE;
}
} // namespace

bool sign_license(const LicenseClaims& claims,
                  const uint8_t secret_key[64],
                  uint8_t out_token[LICENSE_TOKEN_SIZE]) {
    if (!out_token) return false;
    layout_signed_prefix(claims, out_token);
    crypto_eddsa_sign(out_token + LICENSE_SIGNED_SIZE,
                      secret_key,
                      out_token, LICENSE_SIGNED_SIZE);
    return true;
}

bool verify_license(const uint8_t token[LICENSE_TOKEN_SIZE],
                    const uint8_t public_key[32],
                    int64_t now_unix,
                    LicenseClaims& out_claims) {
    if (!token) return false;
    if (std::memcmp(token, MAGIC, 4) != 0) return false;
    if (token[4] != LICENSE_VERSION)        return false;

    // Verify signature first — cheap (~50 us) and guards against forged
    // expiry / tier fields.
    if (crypto_eddsa_check(token + LICENSE_SIGNED_SIZE,
                           public_key,
                           token, LICENSE_SIGNED_SIZE) != 0) {
        return false;
    }

    out_claims.tier     = static_cast<LicenseTier>(token[6]);
    out_claims.exp_unix = static_cast<int64_t>(get_u64_be(token + 7));
    std::memcpy(out_claims.subject, token + 15, 16);

    if (out_claims.exp_unix <= now_unix) return false;
    return true;
}

} // namespace deskbeam::crypto
