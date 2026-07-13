#include "common/crypto/host_identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <errno.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace vivora::crypto {

namespace {

#if defined(_WIN32)
std::string join_path(const std::string& a, const char* b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

// Recursively create a directory tree.  Mirrors `mkdir -p`.
bool ensure_dir(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    if (err == ERROR_PATH_NOT_FOUND) {
        size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos) return false;
        if (!ensure_dir(path.substr(0, slash))) return false;
        return CreateDirectoryA(path.c_str(), nullptr) ||
               GetLastError() == ERROR_ALREADY_EXISTS;
    }
    return false;
}
#else
std::string join_path(const std::string& a, const char* b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0700) == 0) return true;
    if (errno == EEXIST) return true;
    if (errno == ENOENT) {
        size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash == 0) return false;
        if (!ensure_dir(path.substr(0, slash))) return false;
        return mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
    }
    return false;
}
#endif

} // namespace

std::string default_host_key_path() {
#if defined(_WIN32)
    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return join_path(join_path(appdata, "Vivora"), "host_key");
    }
    return "host_key";  // Fallback: current dir.  Better than nothing.
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) {
        if (struct passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    if (!home) return "host_key";
    std::string p = join_path(home, "Library");
    p = join_path(p, "Application Support");
    p = join_path(p, "Vivora");
    return join_path(p, "host_key");
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            if (struct passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
        }
        if (!home) return "host_key";
        base = join_path(home, ".config");
    }
    base = join_path(base, "vivora");
    return join_path(base, "host_key");
#endif
}

bool load_or_create_host_identity(KeyPair& out, const std::string& path) {
    const std::string p = path.empty() ? default_host_key_path() : path;

    // Try to read first — if it exists and has the right size, we're done.
    if (FILE* fp = std::fopen(p.c_str(), "rb")) {
        uint8_t buf[64];
        size_t got = std::fread(buf, 1, sizeof(buf), fp);
        std::fclose(fp);
        if (got == sizeof(buf)) {
            std::memcpy(out.secret_key, buf, 32);
            std::memcpy(out.public_key, buf + 32, 32);
            return true;
        }
        // Size mismatch: corrupted/truncated.  Refuse to silently overwrite.
        return false;
    }

    // Not present — generate a fresh keypair and persist it.
    if (!generate_x25519_keypair(out)) return false;

    // Ensure parent directory exists.
#if defined(_WIN32)
    size_t slash = p.find_last_of("\\/");
#else
    size_t slash = p.find_last_of('/');
#endif
    if (slash != std::string::npos) {
        if (!ensure_dir(p.substr(0, slash))) return false;
    }

    FILE* fp = std::fopen(p.c_str(), "wb");
    if (!fp) return false;
    uint8_t buf[64];
    std::memcpy(buf, out.secret_key, 32);
    std::memcpy(buf + 32, out.public_key, 32);
    size_t wrote = std::fwrite(buf, 1, sizeof(buf), fp);
    std::fclose(fp);
    if (wrote != sizeof(buf)) return false;

#if !defined(_WIN32)
    // Lock down file perms.  On Windows we rely on user-profile ACLs instead.
    chmod(p.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

std::string hex_encode(const uint8_t* bytes, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = hex[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[bytes[i] & 0xF];
    }
    return out;
}

bool hex_decode_32(const std::string& hex, uint8_t out[32]) {
    if (hex.size() != 64) return false;
    auto nibble = [](char c, uint8_t& v) -> bool {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    for (size_t i = 0; i < 32; ++i) {
        uint8_t hi, lo;
        if (!nibble(hex[i * 2], hi))     return false;
        if (!nibble(hex[i * 2 + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string key_fingerprint(const uint8_t pubkey[32]) {
    return key_fingerprint_hex(hex_encode(pubkey, 32));
}

std::string key_fingerprint_hex(const std::string& pubkey_hex) {
    // First 16 hex chars, uppercased, grouped in fours: "6D2E 0C4A 7F3B 9E11".
    // Tolerates short/garbage input by grouping whatever is there.
    std::string out;
    out.reserve(19);
    const size_t n = pubkey_hex.size() < 16 ? pubkey_hex.size() : size_t(16);
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && (i % 4) == 0) out += ' ';
        char c = pubkey_hex[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out += c;
    }
    return out;
}

} // namespace vivora::crypto
