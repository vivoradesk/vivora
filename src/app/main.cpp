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
#include <QApplication>
#include <QTimer>
#endif

#ifdef DESKBEAM_MACOS
#include "client/net/client_session.h"
#include "client/render/mac_video_view.h"
#include "common/utils/types.h"
#include <thread>
#include <chrono>
#endif

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
    auto last_idr_time = Clock::now();
    static constexpr int64_t IDR_INTERVAL_MS = 2000; // Force IDR every 2 seconds

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

        bool force_encode = false;

        // Request IDR when new client connects
        if (session.idr_needed()) {
            encoder->request_idr();
            session.clear_idr_needed();
            last_idr_time = Clock::now();
            force_encode = true;
            log::info("HOST", "IDR requested for new client");
        }

        // Periodic IDR for recovery from packet loss
        if (session.state() == host::SessionState::Connected) {
            auto since_idr = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - last_idr_time).count();
            if (since_idr >= IDR_INTERVAL_MS) {
                encoder->request_idr();
                last_idr_time = Clock::now();
                force_encode = true;
            }
        }

        // Skip encoding if screen content didn't change (cursor-only update)
        if (!frame.content_changed && !force_encode) {
            capture->release_frame(frame);
            continue;
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
    uint64_t last_drops = 0;
    auto last_idr_request = TimePoint{};
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

        // Detect frame drops and request IDR for recovery
        uint64_t drops = session.frames_dropped();
        if (drops > last_drops) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            // Rate-limit IDR requests to at most once per 500ms
            if (since_idr_req > 500) {
                session.request_idr();
                last_idr_request = now;
                got_keyframe = false; // Wait for new keyframe before decoding
                log::warn("VIEW", "Frame loss detected (%llu dropped), requested IDR",
                    (unsigned long long)(drops - last_drops));
            }
            last_drops = drops;
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
            if (!renderer_ready && decoded.width > 0 && decoded.height > 0 && decoded.texture) {
                // Query texture format to detect HDR (P010) vs SDR (NV12)
                D3D11_TEXTURE2D_DESC tex_desc = {};
                decoded.texture->GetDesc(&tex_desc);
                renderer_ready = window.init_renderer(
                    decoder->get_device(), decoded.width, decoded.height, tex_desc.Format);
                if (renderer_ready) {
                    window.set_host_resolution(decoded.width, decoded.height);
                    log::info("VIEW", "Renderer started: %ux%u, format=%u",
                              decoded.width, decoded.height, tex_desc.Format);
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

#ifdef DESKBEAM_MACOS

static int run_view(int /*argc*/, char** /*argv*/, const char* host_ip, uint16_t port) {
    using namespace deskbeam;

    MacVideoView view;
    if (!view.create_window("DeskBeam", 1280, 720)) {
        log::error("VIEW", "Failed to create window");
        return 1;
    }

    client::ClientSession session;
    if (!session.start(host_ip, port)) {
        log::error("VIEW", "Failed to start client session");
        return 1;
    }
    log::info("VIEW", "Connecting to %s:%u", host_ip, port);

    auto start = Clock::now();
    uint64_t frames_received = 0;
    uint64_t frames_rendered = 0;
    uint64_t bytes_received = 0;
    uint64_t last_logged_frames = 0;
    bool got_keyframe = false;

    while (!view.should_close()) {
        view.pump_events();
        session.poll();

        if (session.state() == client::SessionState::Disconnected && frames_received > 0) {
            log::info("VIEW", "Disconnected from host");
            break;
        }

        net::AssembledFrame frame;
        while (session.pop_frame(frame)) {
            frames_received++;
            bytes_received += frame.data.size();

            if (!got_keyframe) {
                if (frame.keyframe) {
                    got_keyframe = true;
                    log::info("VIEW", "Got keyframe seq=%u (%zu bytes), starting render",
                              frame.seq_no, frame.data.size());
                } else {
                    continue;
                }
            }

            if (view.submit_frame(frame.data.data(), frame.data.size(),
                                  frame.timestamp, frame.keyframe)) {
                frames_rendered++;
            }
        }

        if (frames_received >= last_logged_frames + 60) {
            last_logged_frames = frames_received;
            auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            log::info("VIEW", "RX: %llu (%.1f fps, %.2f MB, %llu dropped), rendered: %llu, RTT: %.1fms",
                (unsigned long long)frames_received,
                frames_received / elapsed,
                bytes_received / 1024.0 / 1024.0,
                (unsigned long long)session.frames_dropped(),
                (unsigned long long)frames_rendered,
                session.rtt_ms());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    session.stop();
    return 0;
}

#endif // DESKBEAM_MACOS

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
#endif
#if defined(DESKBEAM_WINDOWS) || defined(DESKBEAM_MACOS)
    if (mode_view) {
        if (!host_ip) {
            std::fprintf(stderr, "Error: --view requires an IP address\n");
            return 1;
        }
        return run_view(argc, argv, host_ip, port);
    }
#endif
#ifdef DESKBEAM_MACOS
    if (mode_host) {
        std::fprintf(stderr, "Error: host mode not yet implemented on macOS\n");
        return 1;
    }
#endif

    print_usage(argv[0]);
    return 0;
}
