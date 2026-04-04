#include "common/utils/log.h"
#include <cstdio>
#include <cstring>

#ifdef DESKBEAM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "common/net/winsock_socket.h"
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "host/encode/video_encoder.h"
#include "host/session/host_session.h"
#include "client/net/client_session.h"
#include "client/decode/video_decoder.h"
#include "client/render/stream_window.h"
#endif

#include <QApplication>
#include <QTimer>

static void print_usage(const char* prog) {
    std::printf("DeskBeam v0.1.0 — low-latency remote desktop\n\n");
    std::printf("Usage:\n");
    std::printf("  %s --host [--port PORT]        Start hosting (share this screen)\n", prog);
    std::printf("  %s --view IP [--port PORT]     Connect to a host\n", prog);
    std::printf("\nDefaults: port 9876\n");
}

#ifdef DESKBEAM_WINDOWS

static int run_host(uint16_t port) {
    using namespace deskbeam;

    net::WinsockInit wsa;
    if (!wsa.ok) {
        log::error("HOST", "Failed to init Winsock");
        return 1;
    }

    // Initialize capture
    auto capture = IScreenCapture::create();
    auto* dxgi = dynamic_cast<DxgiCapture*>(capture.get());
    if (!capture || !capture->init(0)) {
        log::error("HOST", "Failed to init capture");
        return 1;
    }
    auto res = capture->get_resolution();
    log::info("HOST", "Capture: %ux%u", res.width, res.height);

    // Initialize encoder
    auto encoder = IVideoEncoder::create();
    EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = 15'000'000;
    cfg.idr_period = 60;
    if (dxgi) cfg.input_format = dxgi->get_capture_format();

    if (!encoder->init(cfg, dxgi ? dxgi->get_device() : nullptr)) {
        log::error("HOST", "Failed to init encoder");
        return 1;
    }

    // Start session
    host::HostSession session;
    session.set_screen_resolution(res.width, res.height);
    if (!session.start(port)) {
        log::error("HOST", "Failed to start session on port %u", port);
        return 1;
    }
    log::info("HOST", "Waiting for client on port %u... (Ctrl+C to stop)", port);

    uint16_t frame_seq = 0;
    uint64_t total_frames = 0;
    auto start = Clock::now();

    // Force initial mouse movement for DXGI
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    while (true) {
        session.poll();

        if (session.state() == host::SessionState::Disconnected &&
            total_frames > 0) {
            log::info("HOST", "Client disconnected");
            break;
        }

        CapturedFrame frame;
        if (!capture->capture_frame(frame, 16))
            continue;

        uint64_t pts = std::chrono::duration_cast<std::chrono::microseconds>(
            frame.capture_time.time_since_epoch()).count();

        // Request IDR when new client connects
        if (session.idr_needed()) {
            encoder->request_idr();
            session.clear_idr_needed();
            log::info("HOST", "IDR requested for new client");
        }

        if (!encoder->encode(frame.texture, pts)) {
            capture->release_frame(frame);
            continue;
        }
        capture->release_frame(frame);

        EncodedPacket pkt;
        while (encoder->get_packet(pkt)) {
            uint32_t timestamp = static_cast<uint32_t>(pkt.pts & 0xFFFFFFFF);

            if (session.state() == host::SessionState::Connected) {
                session.send_frame(pkt.data.data(), pkt.data.size(),
                                   frame_seq, timestamp, pkt.keyframe);
            }

            frame_seq++;
            total_frames++;

            if (total_frames % 60 == 0) {
                auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
                log::info("HOST", "Frames: %llu, FPS: %.1f, RTT: %.1fms, state: %s",
                    (unsigned long long)total_frames,
                    total_frames / elapsed,
                    session.rtt_ms(),
                    session.state() == host::SessionState::Connected ? "connected" :
                    session.state() == host::SessionState::WaitingForClient ? "waiting" :
                    "disconnected");
            }
        }
    }

    session.stop();
    return 0;
}

static int run_view(int argc, char* argv[], const char* host_ip, uint16_t port) {
    using namespace deskbeam;

    // Initialize COM as MTA before Qt (which may set STA)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    net::WinsockInit wsa;
    if (!wsa.ok) {
        log::error("VIEW", "Failed to init Winsock");
        return 1;
    }

    // Init decoder first (before QApplication which may change COM apartment)
    auto decoder = IVideoDecoder::create();
    if (!decoder->init()) {
        log::error("VIEW", "Failed to init decoder");
        return 1;
    }

    // Qt app + window
    QApplication app(argc, argv);

    StreamWindow window;
    window.setWindowTitle(QString("DeskBeam — %1:%2").arg(host_ip).arg(port));
    window.show();

    // Connect session
    client::ClientSession session;
    if (!session.start(host_ip, port)) {
        log::error("VIEW", "Failed to start client session");
        return 1;
    }
    log::info("VIEW", "Connecting to %s:%u...", host_ip, port);

    // Wire input: window events → session → host
    window.set_input_callback([&session](const protocol::InputEvent& ev) {
        session.send_input(ev);
    });

    bool renderer_ready = false;
    bool got_keyframe = false;
    uint64_t frames_decoded = 0;
    auto start = Clock::now();

    // Poll timer — drives network + decode + render at ~1ms intervals
    QTimer poll_timer;
    QObject::connect(&poll_timer, &QTimer::timeout, [&]() {
        session.poll();

        if (session.state() == client::SessionState::Disconnected && frames_decoded > 0) {
            log::info("VIEW", "Disconnected from host");
            app.quit();
            return;
        }

        // Feed received frames to decoder
        net::AssembledFrame net_frame;
        while (session.pop_frame(net_frame)) {
            // Wait for keyframe before feeding to decoder
            if (!got_keyframe) {
                if (net_frame.keyframe) {
                    got_keyframe = true;
                    log::info("VIEW", "Got keyframe seq=%u (%zu bytes), starting decode",
                              net_frame.seq_no, net_frame.data.size());
                } else {
                    continue; // skip until IDR
                }
            }
            bool ok = decoder->decode(net_frame.data.data(), net_frame.data.size(), net_frame.timestamp);
            if (!ok) log::warn("VIEW", "Decoder rejected frame seq=%u", net_frame.seq_no);
        }

        // Get decoded frames and render
        DecodedFrame decoded;
        while (decoder->get_frame(decoded)) {
            if (!renderer_ready && decoded.width > 0 && decoded.height > 0) {
                renderer_ready = window.init_renderer(
                    decoder->get_device(), decoded.width, decoded.height);
                if (renderer_ready) {
                    window.set_host_resolution(decoded.width, decoded.height);
                    log::info("VIEW", "Renderer started: %ux%u", decoded.width, decoded.height);
                }
            }

            if (renderer_ready) {
                window.render_frame(decoded.texture, decoded.subresource);
            }

            // Release texture reference from MF decoder
            if (decoded.texture) decoded.texture->Release();

            frames_decoded++;

            if (frames_decoded % 60 == 0) {
                auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
                log::info("VIEW", "Decoded: %llu, FPS: %.1f",
                    (unsigned long long)frames_decoded, frames_decoded / elapsed);
            }
        }
    });
    poll_timer.start(1); // 1ms polling

    int ret = app.exec();
    session.stop();
    return ret;
}

#endif // DESKBEAM_WINDOWS

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    uint16_t port = 9876;
    const char* host_ip = nullptr;
    bool mode_host = false;
    bool mode_view = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            mode_host = true;
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

#ifdef DESKBEAM_WINDOWS
    if (mode_host) {
        return run_host(port);
    }
    if (mode_view) {
        if (!host_ip) {
            std::fprintf(stderr, "Error: --view requires an IP address\n");
            return 1;
        }
        return run_view(argc, argv, host_ip, port);
    }
#endif

    print_usage(argv[0]);
    return 0;
}
