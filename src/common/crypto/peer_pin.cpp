// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/crypto/peer_pin.h"
#include "common/crypto/host_identity.h"   // hex_encode

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

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

// Local copies of the path helpers from host_identity.cpp.  Keeping them
// private here avoids leaking the helpers into the public crypto API and
// makes peer_pin self-contained for downstream packagers that strip
// individual TUs.
#if defined(_WIN32)
std::string join_path(const std::string& a, const char* b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}
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

bool parent_dir_for(const std::string& full, std::string& out) {
#if defined(_WIN32)
    size_t slash = full.find_last_of("\\/");
#else
    size_t slash = full.find_last_of('/');
#endif
    if (slash == std::string::npos) return false;
    out = full.substr(0, slash);
    return true;
}

// Trim leading + trailing ASCII whitespace.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

} // namespace

std::string default_peer_pins_path() {
#if defined(_WIN32)
    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        return join_path(join_path(appdata, "Vivora"), "known_peers.txt");
    }
    return "known_peers.txt";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (!home) {
        if (struct passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
    }
    if (!home) return "known_peers.txt";
    std::string p = join_path(home, "Library");
    p = join_path(p, "Application Support");
    p = join_path(p, "Vivora");
    return join_path(p, "known_peers.txt");
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
        if (!home) return "known_peers.txt";
        base = join_path(home, ".config");
    }
    base = join_path(base, "vivora");
    return join_path(base, "known_peers.txt");
#endif
}

PinResult check_or_pin_peer(const std::string& code,
                            const uint8_t pubkey[32],
                            const std::string& path) {
    const std::string p = path.empty() ? default_peer_pins_path() : path;

    switch (query_peer_pin(code, pubkey, nullptr, p)) {
        case PinQuery::Match:    return PinResult::Match;
        case PinQuery::Mismatch: return PinResult::Mismatch;
        case PinQuery::Unknown:  break;
    }

    // Not found — append.  Ensure parent dir exists (chmod 0700 on POSIX).
    std::string parent;
    if (parent_dir_for(p, parent) && !parent.empty()) {
        ensure_dir(parent);
    }
    std::ofstream out(p, std::ios::app);
    if (!out) return PinResult::IoError;
    out << code << "  " << hex_encode(pubkey, 32) << "\n";
    if (!out) return PinResult::IoError;
    return PinResult::NewlyPinned;
}

PinQuery query_peer_pin(const std::string& code,
                        const uint8_t pubkey[32],
                        std::string* stored_hex_out,
                        const std::string& path) {
    const std::string p = path.empty() ? default_peer_pins_path() : path;

    // Scan existing entries for this code.  Linear scan is fine — the
    // file rarely has more than a handful of pinned peers and we hit
    // this exactly once per connect.
    std::ifstream in(p);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') continue;
            std::istringstream iss(t);
            std::string entry_code, entry_hex;
            if (!(iss >> entry_code >> entry_hex)) continue;
            if (entry_code != code) continue;
            // Found this code — compare pubkeys.
            const std::string actual_hex = hex_encode(pubkey, 32);
            if (entry_hex == actual_hex) return PinQuery::Match;
            if (stored_hex_out) *stored_hex_out = entry_hex;
            return PinQuery::Mismatch;
        }
    }
    return PinQuery::Unknown;
}

// Shared rewrite helper: copy the pin file line-by-line, dropping every
// non-comment entry `drop` says to (matched on the parsed code + hex),
// then append `append_line` when non-empty.  Comments and unrelated
// lines are preserved verbatim so hand-edits survive.
static bool rewrite_pin_file(const std::string& p,
                             const std::function<bool(const std::string& code,
                                                      const std::string& hex)>& drop,
                             const std::string& append_line) {
    std::vector<std::string> kept;
    {
        std::ifstream in(p);
        std::string line;
        while (in && std::getline(in, line)) {
            const std::string t = trim(line);
            if (!t.empty() && t[0] != '#') {
                std::istringstream iss(t);
                std::string entry_code, entry_hex;
                if ((iss >> entry_code >> entry_hex) && drop(entry_code, entry_hex))
                    continue;
            }
            kept.push_back(line);
        }
    }

    std::string parent;
    if (parent_dir_for(p, parent) && !parent.empty()) {
        ensure_dir(parent);
    }
    std::ofstream out(p, std::ios::trunc);
    if (!out) return false;
    for (const auto& l : kept) out << l << "\n";
    if (!append_line.empty()) out << append_line << "\n";
    return static_cast<bool>(out);
}

bool pin_peer(const std::string& code,
              const uint8_t pubkey[32],
              const std::string& path) {
    const std::string p = path.empty() ? default_peer_pins_path() : path;
    return rewrite_pin_file(
        p,
        [&](const std::string& entry_code, const std::string&) {
            return entry_code == code;   // replace any previous pin for the code
        },
        code + "  " + hex_encode(pubkey, 32));
}

bool forget_peer_pin(const std::string& code,
                     const std::string& pubkey_hex,
                     const std::string& path) {
    const std::string p = path.empty() ? default_peer_pins_path() : path;
    if (code.empty() && pubkey_hex.empty()) return true;
    // Missing file → nothing to forget.
    if (!std::ifstream(p)) return true;
    return rewrite_pin_file(
        p,
        [&](const std::string& entry_code, const std::string& entry_hex) {
            return (!code.empty() && entry_code == code)
                || (!pubkey_hex.empty() && entry_hex == pubkey_hex);
        },
        std::string());
}

} // namespace vivora::crypto
