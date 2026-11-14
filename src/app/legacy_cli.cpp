// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/legacy_cli.h"
#include "app/host_loop.h"
#include "app/view_loop.h"
#include "common/crypto/access_password.h"
#include "common/crypto/host_identity.h"
#include "common/utils/log.h"
#include "host/encode/video_encoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef VIVORA_WINDOWS
// winsock_socket.h must come first — it includes winsock2.h which must
// precede any windows.h inclusion to avoid winsock.h/winsock2.h clash.
#include "common/net/winsock_socket.h"
#include "app/windows_host_platform.h"
#include "app/windows_view_platform.h"
#endif

#ifdef VIVORA_MACOS
#include "app/mac_host_platform.h"
#include "app/mac_view_platform.h"
#endif

#ifdef VIVORA_LINUX
#include "app/linux_view_platform.h"
#include "app/linux_host_platform.h"
#endif

namespace vivora {

// Match a long-form flag with an attached value: both "--flag VALUE" (next
// argv) and "--flag=VALUE" (single argv with '=') are accepted.  Returns the
// value pointer, or nullptr if the current argv doesn't match the flag.
// Advances `i` past the value when the next argv is consumed.
static const char* flag_value(const char* name, char** argv, int argc, int& i) {
    const char* a = argv[i];
    size_t nlen = std::strlen(name);
    if (std::strncmp(a, name, nlen) == 0) {
        if (a[nlen] == '=') return a + nlen + 1;
        if (a[nlen] == '\0' && i + 1 < argc) return argv[++i];
    }
    return nullptr;
}

// Set by CMake from project(VERSION); the fallback only matters if someone
// compiles this translation unit outside the project's build.
#ifndef VIVORA_VERSION
#define VIVORA_VERSION "0.0.0"
#endif

static void print_usage(const char* prog) {
    // Version from the build, not a literal: the About dialog used to
    // carry its own copy and they would have drifted at 0.1.1.
    std::printf("Vivora v%s — low-latency remote desktop\n\n", VIVORA_VERSION);
    std::printf("Usage:\n");
    std::printf("  %s                             Launch GUI (default)\n", prog);
    std::printf("  %s --host [options]            Headless: start hosting\n", prog);
    std::printf("  %s --view IP [options]         Headless: connect to a host\n", prog);
    std::printf("\nOptions:\n");
    std::printf("  --port PORT       UDP port (default 9876)\n");
    std::printf("  --display N       Display index to capture (host, default 0)\n");
    std::printf("  --bitrate Mbps    Manual encoder bitrate; default is auto from resolution\n");
    std::printf("  --encoder NAME    Force encoder backend: auto|amf|nvenc|qsv|vaapi (default auto)\n");
    std::printf("  --codec NAME      Video codec: h264|hevc (default hevc)\n");
    std::printf("  --stun-server HP  STUN \"host:port\" for reflexive-address discovery\n");
    std::printf("                    (default stun.l.google.com:19302 — use --no-stun to disable)\n");
    std::printf("  --no-stun         Disable STUN discovery (LAN-only)\n");
    std::printf("  --host-key HEX    (view) Host's Curve25519 public key, 64 hex chars\n");
    std::printf("  --rendezvous HP   Rendezvous server \"host:port\"\n");
    std::printf("  --peer HEX        (view) Peer host's pubkey / memorable code\n");
    std::printf("  --relay HP        Relay server \"host:port\"\n");
    std::printf("  --relay-session HEX  64-char hex shared 32-byte session id\n");
    std::printf("  --license PATH    95-byte license token file (Pro relay)\n");
    std::printf("\nUnattended access (a viewer with the password needs no approval):\n");
    std::printf("  --set-access-password    (host) Read a new password from stdin and save it\n");
    std::printf("  --clear-access-password  (host) Turn password access off\n");
    std::printf("  --password-file PATH     (view) Connect with the password in this file\n");
    std::printf("                           (or set VIVORA_ACCESS_PASSWORD)\n");
}

// Reads the first line of `path`, without the line ending.
static bool read_first_line(const char* path, std::string& out) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    char buf[1024];
    const bool ok = std::fgets(buf, sizeof(buf), fp) != nullptr;
    std::fclose(fp);
    if (!ok) return false;
    out = buf;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return true;
}

// --set-access-password / --clear-access-password (VIV-151).  The password
// comes from stdin, never argv, so it stays out of shell history and process
// listings.  Applies to the per-user host key; a running host picks it up on
// its next start.
static int manage_access_password(bool set, const std::string& host_key_path) {
    vivora::crypto::KeyPair id;
    if (!vivora::crypto::load_or_create_host_identity(id, host_key_path)) {
        std::fprintf(stderr, "Error: cannot load the host key\n");
        return 1;
    }
    const std::string path = vivora::crypto::access_psk_path_for(host_key_path);
    if (!set) {
        if (!vivora::crypto::clear_access_psk(path)) {
            std::fprintf(stderr, "Error: cannot remove %s\n", path.c_str());
            return 1;
        }
        std::printf("Password access is off.\n");
        return 0;
    }
    std::fprintf(stderr, "New access password: ");
    char buf[1024];
    if (!std::fgets(buf, sizeof(buf), stdin)) {
        std::fprintf(stderr, "\nError: no password on stdin\n");
        return 1;
    }
    std::string pw = buf;
    std::memset(buf, 0, sizeof(buf));
    while (!pw.empty() && (pw.back() == '\n' || pw.back() == '\r')) pw.pop_back();
    if (pw.size() < vivora::crypto::MIN_ACCESS_PASSWORD_LEN) {
        std::fprintf(stderr, "Error: use at least %zu characters\n",
                     vivora::crypto::MIN_ACCESS_PASSWORD_LEN);
        return 1;
    }
    uint8_t psk[vivora::crypto::ACCESS_PSK_SIZE];
    const bool ok = vivora::crypto::derive_access_psk(pw, id.public_key, psk)
                 && vivora::crypto::save_access_psk(path, id.public_key, psk);
    std::fill(pw.begin(), pw.end(), '\0');
    std::memset(psk, 0, sizeof(psk));
    if (!ok) {
        std::fprintf(stderr, "Error: cannot save %s\n", path.c_str());
        return 1;
    }
    std::printf("Password access is on.\n");
    return 0;
}

int run_legacy_cli(int argc, char** argv) {
    uint16_t port = 9876;
    const char* host_ip = nullptr;
    bool mode_host = false;
    bool mode_view = false;
    uint32_t display_index = 0;
    uint32_t manual_bitrate_bps = 0;
    vivora::EncoderKind encoder_kind = vivora::EncoderKind::Auto;
    vivora::VideoCodec  codec = vivora::VideoCodec::HEVC;
    bool codec_explicit = false;
    const char* stun_server = "stun.l.google.com:19302";
    const char* host_key_hex = nullptr;
    const char* rendezvous_server = nullptr;
    const char* peer_pubkey_hex = nullptr;
    const char* relay_server = nullptr;
    const char* relay_session_hex = nullptr;
    const char* license_file = nullptr;
    const char* password_file = nullptr;
    int access_action = 0;   // 1 = set, 2 = clear

    for (int i = 1; i < argc; ++i) {
        const char* v = nullptr;
        if (std::strcmp(argv[i], "--host") == 0) {
            mode_host = true;
        } else if ((v = flag_value("--display", argv, argc, i)) != nullptr) {
            display_index = static_cast<uint32_t>(std::atoi(v));
        } else if ((v = flag_value("--bitrate", argv, argc, i)) != nullptr) {
            manual_bitrate_bps = static_cast<uint32_t>(std::atoi(v)) * 1'000'000u;
        } else if ((v = flag_value("--encoder", argv, argc, i)) != nullptr) {
            if      (std::strcmp(v, "auto")  == 0) encoder_kind = vivora::EncoderKind::Auto;
            else if (std::strcmp(v, "amf")   == 0) encoder_kind = vivora::EncoderKind::Amf;
            else if (std::strcmp(v, "nvenc") == 0) encoder_kind = vivora::EncoderKind::Nvenc;
            else if (std::strcmp(v, "qsv")   == 0) encoder_kind = vivora::EncoderKind::Qsv;
            else if (std::strcmp(v, "vaapi") == 0) encoder_kind = vivora::EncoderKind::Vaapi;
            else {
                std::fprintf(stderr, "Error: --encoder must be one of: auto, amf, nvenc, qsv, vaapi\n");
                return 1;
            }
        } else if ((v = flag_value("--codec", argv, argc, i)) != nullptr) {
            if      (std::strcmp(v, "h264") == 0 || std::strcmp(v, "avc")  == 0)
                codec = vivora::VideoCodec::H264;
            else if (std::strcmp(v, "hevc") == 0 || std::strcmp(v, "h265") == 0)
                codec = vivora::VideoCodec::HEVC;
            else {
                std::fprintf(stderr, "Error: --codec must be h264 or hevc\n");
                return 1;
            }
            codec_explicit = true;
        } else if (std::strcmp(argv[i], "--view") == 0) {
            mode_view = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                host_ip = argv[++i];
            }
        } else if ((v = flag_value("--port", argv, argc, i)) != nullptr) {
            port = static_cast<uint16_t>(std::atoi(v));
        } else if ((v = flag_value("--stun-server", argv, argc, i)) != nullptr) {
            stun_server = v;
        } else if (std::strcmp(argv[i], "--no-stun") == 0) {
            stun_server = nullptr;
        } else if ((v = flag_value("--host-key", argv, argc, i)) != nullptr) {
            host_key_hex = v;
        } else if ((v = flag_value("--rendezvous", argv, argc, i)) != nullptr) {
            rendezvous_server = v;
        } else if ((v = flag_value("--peer", argv, argc, i)) != nullptr) {
            peer_pubkey_hex = v;
        } else if ((v = flag_value("--relay", argv, argc, i)) != nullptr) {
            relay_server = v;
        } else if ((v = flag_value("--relay-session", argv, argc, i)) != nullptr) {
            relay_session_hex = v;
        } else if ((v = flag_value("--license", argv, argc, i)) != nullptr) {
            license_file = v;
        } else if ((v = flag_value("--password-file", argv, argc, i)) != nullptr) {
            password_file = v;
        } else if (std::strcmp(argv[i], "--set-access-password") == 0) {
            access_action = 1;
        } else if (std::strcmp(argv[i], "--clear-access-password") == 0) {
            access_action = 2;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (mode_host && mode_view) {
        std::fprintf(stderr, "Error: cannot use --host and --view together\n");
        return 1;
    }
    if (access_action) return manage_access_password(access_action == 1, {});

    if (mode_host) {
#ifdef VIVORA_WINDOWS
        vivora::net::WinsockInit wsa;
        if (!wsa.ok) {
            vivora::log::error("HOST", "Failed to init Winsock");
            return 1;
        }
        WindowsHostPlatform platform;
        if (!platform.init(manual_bitrate_bps, encoder_kind, codec,
                           /*stream_fps=*/60, display_index)) return 1;
#endif
#ifdef VIVORA_MACOS
        MacHostPlatform platform;
        if (!platform.init(display_index, manual_bitrate_bps, codec)) return 1;
#endif
#ifdef VIVORA_LINUX
        vivora::VideoCodec linux_codec = codec;
        if (!codec_explicit && codec == vivora::VideoCodec::HEVC) {
            linux_codec = vivora::VideoCodec::H264;
            vivora::log::info("HOST",
                "Defaulting to H.264 on Linux host — pass --codec hevc to force HEVC vaapi.");
        }
        LinuxHostPlatform platform;
        if (!platform.init(manual_bitrate_bps, linux_codec, encoder_kind)) return 1;
#endif
        vivora::HostLoopConfig lcfg;
        lcfg.port = port;
        lcfg.manual_bitrate_bps = manual_bitrate_bps;
        lcfg.encoder_kind = encoder_kind;
#ifdef VIVORA_LINUX
        lcfg.codec = linux_codec;
#else
        lcfg.codec = codec;
#endif
        lcfg.stun_server = stun_server;
        lcfg.rendezvous_server = rendezvous_server;
        lcfg.relay_server = relay_server;
        lcfg.relay_session_hex = relay_session_hex;
        lcfg.license_file = license_file;
        return vivora::run_host_loop(platform, lcfg);
    }

    if (mode_view) {
        if (!host_ip && !(rendezvous_server && peer_pubkey_hex)) {
            std::fprintf(stderr,
                "Error: --view requires either an IP address, "
                "or --rendezvous + --peer\n");
            return 1;
        }
        if (!host_ip) host_ip = "0.0.0.0";
#ifdef VIVORA_WINDOWS
        vivora::net::WinsockInit wsa;
        if (!wsa.ok) {
            vivora::log::error("VIEW", "Failed to init Winsock");
            return 1;
        }
        WindowsViewPlatform platform;
        if (!platform.init(argc, argv, host_ip, port)) return 1;
#endif
#ifdef VIVORA_MACOS
        MacViewPlatform platform;
        if (!platform.init(host_ip, port)) return 1;
#endif
#ifdef VIVORA_LINUX
        LinuxViewPlatform platform;
        if (!platform.init(argc, argv, host_ip, port)) return 1;
#endif
        vivora::ViewLoopConfig vcfg;
        vcfg.host_ip = host_ip;
        vcfg.port = port;
        vcfg.stun_server = stun_server;
        vcfg.host_key_hex = host_key_hex;
        vcfg.rendezvous_server = rendezvous_server;
        vcfg.peer_pubkey_hex = peer_pubkey_hex;
        vcfg.relay_server = relay_server;
        vcfg.relay_session_hex = relay_session_hex;
        vcfg.license_file = license_file;
        if (password_file) {
            if (!read_first_line(password_file, vcfg.access_password)) {
                std::fprintf(stderr, "Error: cannot read %s\n", password_file);
                return 1;
            }
        } else if (const char* pw = std::getenv("VIVORA_ACCESS_PASSWORD")) {
            vcfg.access_password = pw;
        }
        return vivora::run_view_loop(platform, vcfg);
    }

    print_usage(argv[0]);
    return 0;
}

} // namespace vivora
