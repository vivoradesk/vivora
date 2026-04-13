#include "app/host_loop.h"
#include "app/view_loop.h"
#include "common/utils/log.h"

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

static void print_usage(const char* prog) {
    std::printf("DeskBeam v0.1.0 — low-latency remote desktop\n\n");
    std::printf("Usage:\n");
    std::printf("  %s --host [options]            Start hosting (share this screen)\n", prog);
    std::printf("  %s --view IP [options]         Connect to a host\n", prog);
    std::printf("\nOptions:\n");
    std::printf("  --port PORT       UDP port (default 9876)\n");
    std::printf("  --display N       Display index to capture (host, default 0)\n");
    std::printf("  --hdr             Request HDR10 capture if the display supports it\n");
    std::printf("  --bitrate Mbps    Manual encoder bitrate; default is auto from resolution\n");
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
    bool prefer_hdr = false;
    uint32_t manual_bitrate_bps = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            mode_host = true;
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display_index = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--hdr") == 0) {
            prefer_hdr = true;
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            manual_bitrate_bps = static_cast<uint32_t>(std::atoi(argv[++i])) * 1'000'000u;
        } else if (std::strcmp(argv[i], "--view") == 0) {
            mode_view = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                host_ip = argv[++i];
            }
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
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
        if (!platform.init(manual_bitrate_bps)) return 1;
#endif
#ifdef DESKBEAM_MACOS
        MacHostPlatform platform;
        if (!platform.init(display_index, prefer_hdr, manual_bitrate_bps)) return 1;
#endif
        deskbeam::HostLoopConfig lcfg;
        lcfg.port = port;
        lcfg.manual_bitrate_bps = manual_bitrate_bps;
        return deskbeam::run_host_loop(platform, lcfg);
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
        deskbeam::ViewLoopConfig vcfg;
        vcfg.host_ip = host_ip;
        vcfg.port = port;
        return deskbeam::run_view_loop(platform, vcfg);
    }

    print_usage(argv[0]);
    return 0;
}
