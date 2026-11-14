// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/crypto/access_password.h"
#include "common/crypto/host_identity.h"

extern "C" {
#include "monocypher.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace vivora::crypto {

namespace {

constexpr char     kSaltPrefix[] = "vivora-access-v1";   // 16 bytes, no NUL
constexpr char     kFileMagic[4] = {'V', 'A', 'P', '1'};
constexpr size_t   kFileSize     = 4 + 32 + ACCESS_PSK_SIZE;
// 64 MiB, 3 passes, 1 lane: OWASP's second Argon2id profile, and about
// 0.2-0.5 s on a laptop.  Changing these changes every derived key, so a
// change needs a new salt prefix too.
constexpr uint32_t kBlocks = 64 * 1024;
constexpr uint32_t kPasses = 3;

} // namespace

bool derive_access_psk(const std::string& password, const uint8_t host_pk[32],
                       uint8_t out[ACCESS_PSK_SIZE]) {
    std::vector<uint8_t> work;
    try {
        work.resize(static_cast<size_t>(kBlocks) * 1024);
    } catch (...) {
        return false;
    }

    uint8_t salt[sizeof(kSaltPrefix) - 1 + 32];
    std::memcpy(salt, kSaltPrefix, sizeof(kSaltPrefix) - 1);
    std::memcpy(salt + sizeof(kSaltPrefix) - 1, host_pk, 32);

    crypto_argon2_config config;
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = kBlocks;
    config.nb_passes = kPasses;
    config.nb_lanes  = 1;

    crypto_argon2_inputs inputs;
    inputs.pass      = reinterpret_cast<const uint8_t*>(password.data());
    inputs.pass_size = static_cast<uint32_t>(password.size());
    inputs.salt      = salt;
    inputs.salt_size = sizeof(salt);

    crypto_argon2(out, ACCESS_PSK_SIZE, work.data(), config, inputs,
                  crypto_argon2_no_extras);
    crypto_wipe(work.data(), work.size());
    return true;
}

std::string access_psk_path_for(const std::string& host_key_path) {
    const std::string key = host_key_path.empty() ? default_host_key_path()
                                                  : host_key_path;
    const size_t slash = key.find_last_of("\\/");
    if (slash == std::string::npos) return "access_psk";
    return key.substr(0, slash + 1) + "access_psk";
}

bool load_access_psk(const std::string& path, const uint8_t host_pk[32],
                     uint8_t out[ACCESS_PSK_SIZE]) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint8_t buf[kFileSize + 1];
    const size_t got = std::fread(buf, 1, sizeof(buf), fp);
    std::fclose(fp);
    bool ok = got == kFileSize
           && std::memcmp(buf, kFileMagic, 4) == 0
           && crypto_verify32(buf + 4, host_pk) == 0;
    if (ok) std::memcpy(out, buf + 4 + 32, ACCESS_PSK_SIZE);
    crypto_wipe(buf, sizeof(buf));
    return ok;
}

bool save_access_psk(const std::string& path, const uint8_t host_pk[32],
                     const uint8_t psk[ACCESS_PSK_SIZE]) {
    uint8_t buf[kFileSize];
    std::memcpy(buf, kFileMagic, 4);
    std::memcpy(buf + 4, host_pk, 32);
    std::memcpy(buf + 4 + 32, psk, ACCESS_PSK_SIZE);

    const std::string tmp = path + ".tmp";
#if !defined(_WIN32)
    // Create it owner-only from the start rather than chmod after writing.
    const mode_t old_mask = umask(077);
#endif
    FILE* fp = std::fopen(tmp.c_str(), "wb");
#if !defined(_WIN32)
    umask(old_mask);
#endif
    if (!fp) {
        crypto_wipe(buf, sizeof(buf));
        return false;
    }
    const size_t wrote = std::fwrite(buf, 1, sizeof(buf), fp);
    const bool closed = std::fclose(fp) == 0;
    crypto_wipe(buf, sizeof(buf));
    if (wrote != sizeof(buf) || !closed) {
        std::remove(tmp.c_str());
        return false;
    }
#if defined(_WIN32)
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::remove(tmp.c_str());
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
#endif
    return true;
}

bool clear_access_psk(const std::string& path) {
    if (std::remove(path.c_str()) == 0) return true;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return true;   // already absent
    std::fclose(fp);
    return false;
}

} // namespace vivora::crypto
