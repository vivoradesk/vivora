#include "app/host_loop.h"
#include "app/view_loop.h"
#include "common/utils/log.h"
#include "host/encode/video_encoder.h"

#include <cstdio>
#include <cstring>

#ifdef DESKBEAM_WINDOWS
// winsock_socket.h must come first — it includes winsock2.h which must
// precede any windows.h inclusion to avoid winsock.h/winsock2.h clash.
#include "common/net/winsock_socket.h"
#include "app/windows_host_platform.h"
#include "app/windows_view_platform.h"
#endif

#ifdef DESKBEAM_MACOS
#include "app/mac_host_platform.h"
#include "app/mac_view_platform.h"
#endif

#ifdef DESKBEAM_LINUX
#include "app/linux_view_platform.h"
#endif

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

static void print_usage(const char* prog) {
    std::printf("DeskBeam v0.1.0 — low-latency remote desktop\n\n");
    std::printf("Usage:\n");
    std::printf("  %s --host [options]            Start hosting (share this screen)\n", prog);
    std::printf("  %s --view IP [options]         Connect to a host\n", prog);
    std::printf("\nOptions:\n");
    std::printf("  --port PORT       UDP port (default 9876)\n");
    std::printf("  --display N       Display index to capture (host, default 0)\n");
    std::printf("  --bitrate Mbps    Manual encoder bitrate; default is auto from resolution\n");
    std::printf("  --encoder NAME    Force encoder backend: auto|amf|nvenc|qsv (default auto)\n");
    std::printf("  --codec NAME      Video codec: h264|hevc (default hevc)\n");
    std::printf("  --stun-server HP  STUN \"host:port\" for reflexive-address discovery\n");
    std::printf("                    (default stun.l.google.com:19302 — use --no-stun to disable)\n");
    std::printf("  --no-stun         Disable STUN discovery (LAN-only)\n");
    std::printf("  --host-key HEX    (view) Host's Curve25519 public key, 64 hex chars\n");
    std::printf("                    — printed by the host on startup; required by Noise_NK\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    uint16_t port = 9876;
    const char* host_ip = nullptr;
    bool mode_host = false;
    bool mode_view = false;
    uint32_t display_index = 0;
    uint32_t manual_bitrate_bps = 0;
    deskbeam::EncoderKind encoder_kind = deskbeam::EncoderKind::Auto;
    deskbeam::VideoCodec  codec = deskbeam::VideoCodec::HEVC;
    // Google's public STUN server is the unofficial WebRTC default and is
    // reliable enough to use out-of-the-box.  Users can override for privacy
    // or run their own (coturn) in production.
    const char* stun_server = "stun.l.google.com:19302";
    const char* host_key_hex = nullptr;

    for (int i = 1; i < argc; ++i) {
        const char* v = nullptr;
        if (std::strcmp(argv[i], "--host") == 0) {
            mode_host = true;
        } else if ((v = flag_value("--display", argv, argc, i)) != nullptr) {
            display_index = static_cast<uint32_t>(std::atoi(v));
        } else if ((v = flag_value("--bitrate", argv, argc, i)) != nullptr) {
            manual_bitrate_bps = static_cast<uint32_t>(std::atoi(v)) * 1'000'000u;
        } else if ((v = flag_value("--encoder", argv, argc, i)) != nullptr) {
            if      (std::strcmp(v, "auto")  == 0) encoder_kind = deskbeam::EncoderKind::Auto;
            else if (std::strcmp(v, "amf")   == 0) encoder_kind = deskbeam::EncoderKind::Amf;
            else if (std::strcmp(v, "nvenc") == 0) encoder_kind = deskbeam::EncoderKind::Nvenc;
            else if (std::strcmp(v, "qsv")   == 0) encoder_kind = deskbeam::EncoderKind::Qsv;
            else {
                std::fprintf(stderr, "Error: --encoder must be one of: auto, amf, nvenc, qsv\n");
                return 1;
            }
        } else if ((v = flag_value("--codec", argv, argc, i)) != nullptr) {
            if      (std::strcmp(v, "h264") == 0 || std::strcmp(v, "avc")  == 0)
                codec = deskbeam::VideoCodec::H264;
            else if (std::strcmp(v, "hevc") == 0 || std::strcmp(v, "h265") == 0)
                codec = deskbeam::VideoCodec::HEVC;
            else {
                std::fprintf(stderr, "Error: --codec must be h264 or hevc\n");
                return 1;
            }
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
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (mode_host && mode_view) {
        std::fprintf(stderr, "Error: cannot use --host and --view together\n");
        return 1;
    }

    if (mode_host) {
#ifdef DESKBEAM_WINDOWS
        deskbeam::net::WinsockInit wsa;
        if (!wsa.ok) {
            deskbeam::log::error("HOST", "Failed to init Winsock");
            return 1;
        }
        WindowsHostPlatform platform;
        if (!platform.init(manual_bitrate_bps, encoder_kind, codec)) return 1;
#endif
#ifdef DESKBEAM_MACOS
        MacHostPlatform platform;
        if (!platform.init(display_index, manual_bitrate_bps, codec)) return 1;
#endif
#ifdef DESKBEAM_LINUX
        std::fprintf(stderr,
            "Error: --host is not yet implemented on Linux. "
            "Linux client (--view) works against macOS/Windows hosts; "
            "Linux host support is L3 on the roadmap.\n");
        return 1;
#else
        deskbeam::HostLoopConfig lcfg;
        lcfg.port = port;
        lcfg.manual_bitrate_bps = manual_bitrate_bps;
        lcfg.encoder_kind = encoder_kind;
        lcfg.codec = codec;
        lcfg.stun_server = stun_server;
        return deskbeam::run_host_loop(platform, lcfg);
#endif
    }

    if (mode_view) {
        if (!host_ip) {
            std::fprintf(stderr, "Error: --view requires an IP address\n");
            return 1;
        }
#ifdef DESKBEAM_WINDOWS
        deskbeam::net::WinsockInit wsa;
        if (!wsa.ok) {
            deskbeam::log::error("VIEW", "Failed to init Winsock");
            return 1;
        }
        WindowsViewPlatform platform;
        if (!platform.init(argc, argv, host_ip, port)) return 1;
#endif
#ifdef DESKBEAM_MACOS
        MacViewPlatform platform;
        if (!platform.init(host_ip, port)) return 1;
#endif
#ifdef DESKBEAM_LINUX
        LinuxViewPlatform platform;
        if (!platform.init(argc, argv, host_ip, port)) return 1;
#endif
        deskbeam::ViewLoopConfig vcfg;
        vcfg.host_ip = host_ip;
        vcfg.port = port;
        vcfg.stun_server = stun_server;
        vcfg.host_key_hex = host_key_hex;
        return deskbeam::run_view_loop(platform, vcfg);
    }

    print_usage(argv[0]);
    return 0;
}
