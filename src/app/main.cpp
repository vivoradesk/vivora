#include "common/utils/log.h"
#include "common/codec/bitrate_controller.h"
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
#include "host/capture/mac_screen_capture.h"
#include "host/encode/mac_videotoolbox_encoder.h"
#include "host/session/host_session.h"
#include "common/utils/types.h"
#include <thread>
#include <chrono>
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

#ifdef DESKBEAM_WINDOWS

static int run_host(uint16_t port, uint32_t manual_bitrate_bps) {
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

    // Bitrate controller — picks a sensible default from resolution, allows
    // a manual override via --bitrate, and exposes hooks for future
    // congestion-control feedback (RTT / loss / bandwidth estimate).
    codec::BitrateController bitrate_ctl(
        codec::default_bitrate_for(res.width, res.height, 60));
    if (manual_bitrate_bps != 0) {
        bitrate_ctl.set_manual_target(manual_bitrate_bps);
        bitrate_ctl.tick();
    }
    log::info("HOST", "Initial bitrate: %u kbps (%s)",
              bitrate_ctl.current() / 1000,
              manual_bitrate_bps ? "manual" : "auto");

    // Initialize encoder
    auto encoder = IVideoEncoder::create();
    EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = bitrate_ctl.current();
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
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;
    auto last_idr_time = Clock::now();
    static constexpr int64_t IDR_INTERVAL_MS = 2000; // Force IDR every 2 seconds

    // Retx-rate tracking: fed into bitrate controller as a congestion
    // signal in addition to client-reported FEC loss.  Committed in
    // windows of >=20 packets to filter single-iter noise.
    uint64_t last_retx_sample = 0;
    uint64_t last_pkts_sample = 0;

    // Deadband: compare proposed bitrate against the last one we actually
    // applied to the encoder (not the controller's internal current), so
    // small accumulating drift still eventually crosses the 5% threshold.
    uint32_t last_applied_br = bitrate_ctl.current();

    host::SessionState prev_state = session.state();

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

        // Detect WaitingForClient -> Connected and arm warm-up ramp.
        // Push the warm-up start bitrate to the encoder immediately so
        // the first IDR goes out cold at low bitrate, then ramps up.
        if (prev_state != host::SessionState::Connected &&
            session.state() == host::SessionState::Connected) {
            bitrate_ctl.notify_client_connected();
            encoder->set_bitrate(bitrate_ctl.current());
            last_applied_br = bitrate_ctl.current();
            log::info("HOST", "Warmup start -> %u kbps", last_applied_br / 1000);
            last_retx_sample = session.sender() ? session.sender()->retransmits() : 0;
            last_pkts_sample = session.sender() ? session.sender()->packets_sent() : 0;
        }
        prev_state = session.state();

        // Feed BW probe result to bitrate controller (once).
        if (session.probe_bw_bps() > 0 && !session.probe_pending()) {
            uint32_t raw = session.probe_bw_bps();
            bitrate_ctl.set_probe_bandwidth(raw);
        }

        // Feed telemetry to bitrate controller and apply if it changed.
        // Loss signal combines:
        //   (a) client-reported FEC loss (channel loss before recovery)
        //   (b) host-observed retx rate (packets we had to resend)
        // The max of the two drives congestion response, so either an
        // unhappy client or a busy retx loop can trigger bitrate cuts.
        bitrate_ctl.on_rtt(session.rtt_ms());
        {
            double loss_signal = session.last_loss_rate();
            if (session.sender()) {
                uint64_t cur_retx = session.sender()->retransmits();
                uint64_t cur_pkts = session.sender()->packets_sent();
                uint64_t d_retx = cur_retx - last_retx_sample;
                uint64_t d_pkts = cur_pkts - last_pkts_sample;
                if (d_pkts >= 20) {
                    double retx_ratio = static_cast<double>(d_retx)
                                      / static_cast<double>(d_pkts);
                    if (retx_ratio > loss_signal) loss_signal = retx_ratio;
                    last_retx_sample = cur_retx;
                    last_pkts_sample = cur_pkts;
                }
            }
            bitrate_ctl.on_loss_ratio(loss_signal);

            bool changed = false;
            uint32_t br = bitrate_ctl.tick(&changed);
            // End-of-grace one-shot: rebaseline retx counters so that
            // bursts accumulated during grace don't land in the first
            // post-grace adaptation sample.
            if (bitrate_ctl.consume_grace_ended_flag() && session.sender()) {
                last_retx_sample = session.sender()->retransmits();
                last_pkts_sample = session.sender()->packets_sent();
            }
            if (changed) {
                // Deadband: skip encoder reconfig for sub-5% changes vs
                // last applied — oscillation noise at the floor would
                // otherwise cause a GOP disturbance on every tick.
                uint32_t delta = br > last_applied_br
                                 ? br - last_applied_br
                                 : last_applied_br - br;
                if (delta * 20 >= last_applied_br) {
                    encoder->set_bitrate(br);
                    last_applied_br = br;
                    log::info("HOST", "Bitrate changed -> %u kbps", br / 1000);
                }
            }
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
                auto now = Clock::now();
                double window_sec = std::chrono::duration<double>(now - last_log_time).count();
                double inst_fps = window_sec > 0
                    ? (total_frames - last_log_frames) / window_sec
                    : 0.0;
                last_log_time = now;
                last_log_frames = total_frames;
                uint64_t retx = session.sender() ? session.sender()->retransmits() : 0;
                uint8_t fec_k = session.sender() ? session.sender()->fec_group_size() : 0;
                log::info("HOST", "Frames: %llu, FPS: %.1f, RTT: %.1fms, retx: %llu, fec_k: %d, state: %s",
                    (unsigned long long)total_frames,
                    inst_fps,
                    session.rtt_ms(),
                    (unsigned long long)retx,
                    (int)fec_k,
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
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;

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
                decoder->flush();     // Clear stale reference frames
                log::warn("VIEW", "Frame loss detected (%llu dropped), requested IDR + flush",
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
                auto now = Clock::now();
                double window_sec = std::chrono::duration<double>(now - last_log_time).count();
                double inst_fps = window_sec > 0
                    ? (frames_decoded - last_log_frames) / window_sec
                    : 0.0;
                last_log_time = now;
                last_log_frames = frames_decoded;
                log::info("VIEW", "Decoded: %llu, FPS: %.1f",
                    (unsigned long long)frames_decoded, inst_fps);
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

    view.set_input_callback([&session](const protocol::InputEvent& ev) {
        session.send_input(ev);
    });

    auto last_log_time = Clock::now();
    uint64_t frames_received = 0;
    uint64_t frames_rendered = 0;
    uint64_t bytes_received = 0;
    uint64_t last_logged_frames = 0;
    uint64_t last_drops = 0;
    auto last_idr_request = TimePoint{};
    bool got_keyframe = false;

    while (!view.should_close()) {
        view.pump_events();
        session.poll();

        if (session.state() == client::SessionState::Disconnected && frames_received > 0) {
            log::info("VIEW", "Disconnected from host");
            break;
        }

        // Drop detection → IDR request for recovery (rate-limited).
        uint64_t drops = session.frames_dropped();
        if (drops > last_drops) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            if (since_idr_req > 500) {
                session.request_idr();
                last_idr_request = now;
                got_keyframe = false;
                log::warn("VIEW", "Frame loss detected (%llu dropped), requested IDR",
                    (unsigned long long)(drops - last_drops));
            }
            last_drops = drops;
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
            auto now = Clock::now();
            double window_sec = std::chrono::duration<double>(now - last_log_time).count();
            double inst_fps = window_sec > 0
                ? (frames_received - last_logged_frames) / window_sec
                : 0.0;
            last_log_time = now;
            last_logged_frames = frames_received;
            log::info("VIEW", "RX: %llu (%.1f fps, %.2f MB, %llu dropped), rendered: %llu, RTT: %.1fms",
                (unsigned long long)frames_received,
                inst_fps,
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

static int run_host_mac(uint16_t port, uint32_t display_index, bool prefer_hdr,
                        uint32_t manual_bitrate_bps) {
    using namespace deskbeam;

    // Enumerate displays for the log.
    auto displays = host::MacScreenCapture::enumerate_displays();
    if (displays.empty()) {
        log::error("HOST", "No displays found (check Screen Recording permission)");
        return 1;
    }
    log::info("HOST", "Available displays:");
    for (const auto& d : displays) {
        log::info("HOST", "  [%u] %s %s", d.index, d.name.c_str(),
                  d.hdr_capable ? "(HDR capable)" : "");
    }
    if (display_index >= displays.size()) {
        log::error("HOST", "Display index %u out of range", display_index);
        return 1;
    }

    host::MacScreenCapture capture;
    host::MacCaptureConfig ccfg;
    ccfg.display_index = display_index;
    ccfg.fps = 60;
    ccfg.show_cursor = false;
    ccfg.prefer_hdr = prefer_hdr;
    if (!capture.init(ccfg)) {
        log::error("HOST", "Failed to init capture");
        return 1;
    }
    if (!capture.start()) {
        log::error("HOST", "Failed to start capture");
        return 1;
    }

    codec::BitrateController bitrate_ctl(
        codec::default_bitrate_for(capture.width(), capture.height(), 60));
    if (manual_bitrate_bps != 0) {
        bitrate_ctl.set_manual_target(manual_bitrate_bps);
        bitrate_ctl.tick();
    }
    log::info("HOST", "Initial bitrate: %u kbps (%s)",
              bitrate_ctl.current() / 1000,
              manual_bitrate_bps ? "manual" : "auto");

    host::MacVideoToolboxEncoder encoder;
    host::MacEncoderConfig ecfg;
    ecfg.width = capture.width();
    ecfg.height = capture.height();
    ecfg.fps = 60;
    ecfg.bitrate_bps = bitrate_ctl.current();
    ecfg.idr_period = 120;
    ecfg.hdr = capture.hdr_active();
    if (!encoder.init(ecfg)) {
        log::error("HOST", "Failed to init encoder");
        capture.stop();
        return 1;
    }

    host::HostSession session;
    // Input injection happens in the display's "points" coordinate space
    // (CGEventPost operates in points, not backing pixels), so we send the
    // points dimensions to the injector — not the capture pixel dimensions,
    // which on Retina are 2x larger.
    session.set_screen_resolution(capture.points_width(), capture.points_height());
    if (!session.start(port)) {
        log::error("HOST", "Failed to start session on port %u", port);
        return 1;
    }
    log::info("HOST", "Waiting for client on port %u...", port);

    uint16_t frame_seq = 0;
    uint64_t total_frames = 0;
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;
    auto last_idr_time = Clock::now();
    static constexpr int64_t IDR_INTERVAL_MS = 2000;

    // Retx-rate tracking for congestion response (see Windows host loop).
    uint64_t last_retx_sample = 0;
    uint64_t last_pkts_sample = 0;
    uint32_t last_applied_br  = bitrate_ctl.current();

    host::SessionState prev_state = session.state();

    while (true) {
        session.poll();

        if (session.state() == host::SessionState::Disconnected && total_frames > 0) {
            log::info("HOST", "Client disconnected");
            break;
        }

        // Arm warm-up ramp on client (re)connect — see Windows host loop.
        if (prev_state != host::SessionState::Connected &&
            session.state() == host::SessionState::Connected) {
            bitrate_ctl.notify_client_connected();
            encoder.set_bitrate(bitrate_ctl.current());
            last_applied_br = bitrate_ctl.current();
            log::info("HOST", "Warmup start -> %u kbps", last_applied_br / 1000);
            last_retx_sample = session.sender() ? session.sender()->retransmits() : 0;
            last_pkts_sample = session.sender() ? session.sender()->packets_sent() : 0;
        }
        prev_state = session.state();

        // Feed BW probe result to bitrate controller (once).
        if (session.probe_bw_bps() > 0 && !session.probe_pending()) {
            uint32_t raw = session.probe_bw_bps();
            bitrate_ctl.set_probe_bandwidth(raw);
        }

        // Feed telemetry to bitrate controller and apply if it changed.
        // Loss signal = max(client-reported FEC loss, host retx rate).
        bitrate_ctl.on_rtt(session.rtt_ms());
        {
            double loss_signal = session.last_loss_rate();
            if (session.sender()) {
                uint64_t cur_retx = session.sender()->retransmits();
                uint64_t cur_pkts = session.sender()->packets_sent();
                uint64_t d_retx = cur_retx - last_retx_sample;
                uint64_t d_pkts = cur_pkts - last_pkts_sample;
                if (d_pkts >= 20) {
                    double retx_ratio = static_cast<double>(d_retx)
                                      / static_cast<double>(d_pkts);
                    if (retx_ratio > loss_signal) loss_signal = retx_ratio;
                    last_retx_sample = cur_retx;
                    last_pkts_sample = cur_pkts;
                }
            }
            bitrate_ctl.on_loss_ratio(loss_signal);

            bool changed = false;
            uint32_t br = bitrate_ctl.tick(&changed);
            if (bitrate_ctl.consume_grace_ended_flag() && session.sender()) {
                last_retx_sample = session.sender()->retransmits();
                last_pkts_sample = session.sender()->packets_sent();
            }
            if (changed) {
                uint32_t delta = br > last_applied_br
                                 ? br - last_applied_br
                                 : last_applied_br - br;
                if (delta * 20 >= last_applied_br) {
                    encoder.set_bitrate(br);
                    last_applied_br = br;
                    log::info("HOST", "Bitrate changed -> %u kbps", br / 1000);
                }
            }
        }

        // Request IDR on client (re)connect.
        if (session.idr_needed()) {
            encoder.request_idr();
            session.clear_idr_needed();
            last_idr_time = Clock::now();
            log::info("HOST", "IDR requested for new client");
        }

        // Periodic IDR for loss recovery.
        if (session.state() == host::SessionState::Connected) {
            auto since_idr = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - last_idr_time).count();
            if (since_idr >= IDR_INTERVAL_MS) {
                encoder.request_idr();
                last_idr_time = Clock::now();
            }
        }

        // Pull newest captured frame (drop-oldest).
        uint64_t pts_us = 0;
        CVPixelBufferRef pb = capture.try_get_frame(&pts_us);
        if (pb) {
            encoder.encode(pb, pts_us);  // takes ownership
        }

        // Drain encoder output.
        host::MacEncodedPacket pkt;
        while (encoder.get_packet(pkt)) {
            uint32_t timestamp = static_cast<uint32_t>(pkt.pts & 0xFFFFFFFF);
            if (session.state() == host::SessionState::Connected) {
                session.send_frame(pkt.data.data(), pkt.data.size(),
                                   frame_seq, timestamp, pkt.keyframe);
            }
            frame_seq++;
            total_frames++;

            if (total_frames % 60 == 0) {
                auto now = Clock::now();
                double window_sec = std::chrono::duration<double>(now - last_log_time).count();
                double inst_fps = window_sec > 0
                    ? (total_frames - last_log_frames) / window_sec
                    : 0.0;
                last_log_time = now;
                last_log_frames = total_frames;
                uint64_t retx = session.sender() ? session.sender()->retransmits() : 0;
                uint8_t fec_k = session.sender() ? session.sender()->fec_group_size() : 0;
                log::info("HOST", "Frames: %llu, FPS: %.1f, RTT: %.1fms, retx: %llu, fec_k: %d, state: %s",
                    (unsigned long long)total_frames,
                    inst_fps,
                    session.rtt_ms(),
                    (unsigned long long)retx,
                    (int)fec_k,
                    session.state() == host::SessionState::Connected ? "connected" :
                    session.state() == host::SessionState::WaitingForClient ? "waiting" :
                    "disconnected");
            }
        }

        if (!pb) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    session.stop();
    encoder.shutdown();
    capture.stop();
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
    uint32_t display_index = 0;
    bool prefer_hdr = false;
    uint32_t manual_bitrate_bps = 0;   // 0 = auto from resolution

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0) {
            mode_host = true;
        } else if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display_index = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--hdr") == 0) {
            prefer_hdr = true;
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            // Manual bitrate override in Mbps (overrides resolution-based default).
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

#ifdef DESKBEAM_WINDOWS
    if (mode_host) {
        return run_host(port, manual_bitrate_bps);
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
        return run_host_mac(port, display_index, prefer_hdr, manual_bitrate_bps);
    }
#endif

    print_usage(argv[0]);
    return 0;
}
