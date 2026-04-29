#include "app/host_loop.h"
#include "common/audio/audio_capture.h"
#include "common/codec/bitrate_controller.h"
#include "common/net/socket.h"
#include "common/protocol/cursor_message.h"
#include "common/utils/log.h"
#include "common/utils/thread_priority.h"
#include "common/utils/types.h"
#include "host/audio/audio_sender.h"
#include "host/session/host_session.h"

#include <cstdlib>
#include <cstring>

namespace deskbeam {

int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg) {
    // Capture → encode → fragment → send all runs single-threaded on this
    // loop; a background compile or Windows Update scan preempting it adds
    // straight jitter to end-to-end latency.
    utils::boost_current_thread_priority();
    const uint32_t cap_w = platform.capture_width();
    const uint32_t cap_h = platform.capture_height();

    // Bitrate controller — picks a sensible default from resolution, allows
    // a manual override via --bitrate, and exposes hooks for future
    // congestion-control feedback (RTT / loss / bandwidth estimate).
    codec::BitrateController bitrate_ctl(
        codec::default_bitrate_for(cap_w, cap_h, 60));
    if (cfg.manual_bitrate_bps != 0) {
        bitrate_ctl.set_manual_target(cfg.manual_bitrate_bps);
        bitrate_ctl.tick();
    }
    log::info("HOST", "Initial bitrate: %u kbps (%s)",
              bitrate_ctl.current() / 1000,
              cfg.manual_bitrate_bps ? "manual" : "auto");

    // Diagnostic env-var: raise the hard warmup/recovery ceiling above
    // 10 Mbps so a real channel-capacity test can ramp further.
    if (const char* env = std::getenv("DESKBEAM_MAX_BPS")) {
        uint32_t max_bps = static_cast<uint32_t>(std::atoll(env));
        if (max_bps > 0) {
            bitrate_ctl.set_ceiling_override(max_bps);
            log::info("HOST", "DESKBEAM_MAX_BPS=%u -> ceiling override %u kbps",
                      max_bps, max_bps / 1000);
        }
    }

    // Start session.
    host::HostSession session;
    session.set_screen_resolution(platform.input_width(), platform.input_height());
    session.set_codec(platform.actual_codec());
    if (cfg.stun_server && *cfg.stun_server) {
        net::SocketAddr stun = net::resolve_host_port(cfg.stun_server);
        if (stun.ip == 0) {
            log::warn("HOST", "Could not resolve STUN server '%s' — skipping discovery",
                      cfg.stun_server);
        } else {
            session.set_stun_server(stun);
        }
    }
    if (!session.start(cfg.port)) {
        log::error("HOST", "Failed to start session on port %u", cfg.port);
        return 1;
    }
    log::info("HOST", "Waiting for client on port %u... (Ctrl+C to stop)", cfg.port);

    // Diagnostic env-var: freeze FEC M at a fixed value, disabling
    // loss-adaptive and RTT-lock behavior. For probing the real channel
    // capacity with known redundancy (e.g. M=10 at K=10 = 50%).
    if (const char* env = std::getenv("DESKBEAM_FEC_M")) {
        int m = std::atoi(env);
        if (m > 0 && m <= 32 && session.sender()) {
            session.sender()->set_force_m(static_cast<uint8_t>(m));
            log::info("HOST", "DESKBEAM_FEC_M=%d -> force M (adaptive disabled)", m);
        }
    }

    // Start system audio loopback capture and pipe into the session's
    // AudioSender. Packets go out only once the sender has a registered
    // destination (client connects with audio port). Non-fatal on failure.
    std::unique_ptr<audio::AudioCapture> audio_capture =
        audio::create_default_loopback_capture();
    if (audio_capture) {
        host::AudioSender* asend = session.audio_sender();
        if (asend) {
            audio_capture->start([asend](const float* pcm, uint32_t frames,
                                         uint32_t rate, uint16_t ch) {
                asend->feed(pcm, frames, rate, ch);
            });
        } else {
            audio_capture.reset();
        }
    } else {
        log::warn("HOST", "No audio loopback capture available on this platform");
    }

    uint16_t frame_seq = 0;
    uint64_t total_frames = 0;
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;
    // No periodic IDR. Encoders run with continuous intra refresh
    // (NVENC intraRefresh* / QSV IntRefType=HORIZONTAL), so the picture
    // self-heals every ~1s worth of frames without the packet burst of a
    // traditional IDR. New-client IDR and loss-triggered IDR from view
    // layer cover cold-start and edge-case recovery.

    // Cursor shape retry queue: DXGI reports shape changes once, we send
    // the packet a few times over the next ~250ms so UDP loss doesn't
    // leave the client with a stale shape for long.
    protocol::CursorShapeMessage pending_shape;
    int shape_sends_remaining = 0;
    auto last_shape_send = Clock::now() - std::chrono::seconds(1);
    static constexpr int64_t SHAPE_RETRY_MS = 100;

    // Retx-rate tracking: fed into bitrate controller as a congestion
    // signal in addition to client-reported FEC loss.
    uint64_t last_retx_sample = 0;
    uint64_t last_pkts_sample = 0;

    // Deadband: compare proposed bitrate against the last one we actually
    // applied to the encoder (not the controller's internal current), so
    // small accumulating drift still eventually crosses the 5% threshold.
    uint32_t last_applied_br = bitrate_ctl.current();
    bool had_clients = false;

    // Adaptive framerate throttle.  Clients send PerfReport once per second
    // with their sustainable target_fps; the host paces capture+encode to
    // the slowest client.  EWMA over the last few samples avoids reacting
    // to single-interval spikes, and we log every applied change so we can
    // see the throttle live in the host log alongside bitrate adjustments.
    auto     last_capture_time   = TimePoint{};
    uint16_t applied_target_fps  = 60;
    float    target_fps_ewma     = 60.0f;
    int64_t  min_frame_interval_us = 16667;  // 60 fps default

    while (true) {
        session.poll();

        // Exit when all clients disconnect after we've had at least one.
        if (session.state() == host::SessionState::Disconnected && had_clients) {
            log::info("HOST", "All clients disconnected");
            break;
        }

        // Handle new client connections.
        if (session.consume_new_client_flag()) {
            // Tell the new client the real (pre-padding) frame size so
            // its renderer crops encoder-alignment padding and its mouse
            // mapping matches the host screen.  Repeated on every
            // keyframe below for loss resilience.
            session.send_stream_info(static_cast<uint16_t>(cap_w),
                                     static_cast<uint16_t>(cap_h));
            if (!had_clients) {
                // First client: arm warm-up ramp (cold start).
                bitrate_ctl.notify_client_connected();
                platform.set_bitrate(bitrate_ctl.current());
                last_applied_br = bitrate_ctl.current();
                log::info("HOST", "First client connected, warmup -> %u kbps",
                          last_applied_br / 1000);
                last_retx_sample = session.sender() ? session.sender()->retransmits() : 0;
                last_pkts_sample = session.sender() ? session.sender()->packets_sent() : 0;
            } else {
                // Additional client: cut bitrate proportionally so total
                // wire rate doesn't spike (N clients share the link).
                size_t n = session.client_count();
                bitrate_ctl.set_client_count(n);
                uint32_t new_br = bitrate_ctl.current() * (n - 1) / n;
                if (new_br < 1'000'000) new_br = 1'000'000;
                bitrate_ctl.force_bitrate(new_br);
                platform.set_bitrate(new_br);
                last_applied_br = new_br;
                log::info("HOST", "New client connected (%zu total), bitrate -> %u kbps",
                          n, new_br / 1000);
            }
            had_clients = true;
        }

        // Keep bitrate controller aware of client count so it can
        // divide the ceiling (total wire = bitrate × clients).
        bitrate_ctl.set_client_count(session.client_count());

        // Feed BW probe result to bitrate controller (once).
        if (session.probe_bw_bps() > 0 && !session.probe_pending()) {
            bitrate_ctl.set_probe_bandwidth(session.probe_bw_bps());
        }

        // Feed telemetry to bitrate controller and apply if it changed.
        // Loss signal combines:
        //   (a) client-reported FEC loss (channel loss before recovery)
        //   (b) host-observed retx rate (packets we had to resend)
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
            // Convert "total wire budget" → "encoder bitrate" by carving
            // out room for FEC parity.  When M grows (loss adaptation in
            // VideoSender), the encoder shrinks instead of the wire load
            // ballooning — keeping total channel utilisation constant.
            // Without this, M=4 → 40% extra wire on top of `br`, which
            // saturates WiFi and causes the loss-spirals we observed.
            uint32_t encoder_bps = br;
            if (auto* s = session.sender()) {
                uint8_t k = s->fec_group_size();
                uint8_t m = s->fec_parity_count();
                if (k > 0) encoder_bps = static_cast<uint32_t>(
                    static_cast<uint64_t>(br) * k / (k + m));
            }
            // Apply whenever encoder_bps moves by more than the 5%
            // deadband — this catches both wire-budget changes (`changed`
            // from `tick()`) and FEC-parity-count changes (M growing
            // shrinks encoder_bps without `changed` firing).
            (void)changed;  // signal absorbed into encoder_bps now
            uint32_t delta = encoder_bps > last_applied_br
                             ? encoder_bps - last_applied_br
                             : last_applied_br - encoder_bps;
            if (delta * 20 >= last_applied_br) {
                platform.set_bitrate(encoder_bps);
                last_applied_br = encoder_bps;
                log::info("HOST", "Encoder bitrate -> %u kbps (wire %u, FEC overhead carved)",
                          encoder_bps / 1000, br / 1000);
            }
        }

        // IDR on client (re)connect or client-requested recovery.
        // No host-side cooldown: view_loop already gates IDR retries to
        // 600ms; adding another cooldown here only creates a window where
        // a lost recovery IDR can't be re-requested. Tried 2026-04-29,
        // saw 48 client retries result in 0 host IDR fires post-cold-start.
        bool force_encode = false;
        if (session.idr_needed()) {
            platform.request_idr();
            session.clear_idr_needed();
            force_encode = true;
            log::info("HOST", "IDR requested for new client");
        }

        // Adaptive framerate gate: skip this iteration's capture if it
        // would arrive sooner than the negotiated min frame interval.
        // Update the smoothed target every iteration but only re-arm
        // the interval when EWMA crosses ±2 fps from the applied value
        // (prevents single-spike whiplash).
        {
            uint16_t want = session.min_perf_target_fps();
            target_fps_ewma = 0.7f * target_fps_ewma + 0.3f * static_cast<float>(want);
            uint16_t smoothed = static_cast<uint16_t>(target_fps_ewma + 0.5f);
            int diff = static_cast<int>(smoothed) - static_cast<int>(applied_target_fps);
            if (diff >= 2 || diff <= -2) {
                applied_target_fps = smoothed;
                min_frame_interval_us = 1'000'000 / std::max<uint16_t>(smoothed, 1);
                log::info("HOST", "Adaptive framerate -> %u fps (interval %lld us)",
                          applied_target_fps,
                          static_cast<long long>(min_frame_interval_us));
            }
        }
        if (last_capture_time.time_since_epoch().count() != 0 && !force_encode) {
            auto since_us = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - last_capture_time).count();
            if (since_us < min_frame_interval_us) {
                platform.on_idle();
                continue;
            }
        }

        // Capture + encode.
        uint64_t pts_us = 0;
        bool content_changed = false;
        bool got_frame = platform.capture_and_encode(pts_us, content_changed, force_encode);
        if (got_frame) last_capture_time = Clock::now();

        // Cursor sync runs regardless of whether we produced an encoded
        // frame — the cursor can move over static content.
        if (session.state() == host::SessionState::Connected) {
            HostPlatform::CursorShapeView shape_view;
            if (platform.take_cursor_shape(shape_view)) {
                pending_shape.shape_id = shape_view.id;
                pending_shape.width    = shape_view.width;
                pending_shape.height   = shape_view.height;
                pending_shape.hotspot_x= shape_view.hotspot_x;
                pending_shape.hotspot_y= shape_view.hotspot_y;
                pending_shape.bgra     = std::move(shape_view.bgra);
                shape_sends_remaining  = 3;
                last_shape_send        = Clock::now() - std::chrono::seconds(1);
                log::info("HOST", "Cursor shape change: id=%u %ux%u",
                          pending_shape.shape_id,
                          (unsigned)pending_shape.width,
                          (unsigned)pending_shape.height);
            }

            if (shape_sends_remaining > 0) {
                auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - last_shape_send).count();
                if (since >= SHAPE_RETRY_MS) {
                    session.send_cursor_shape(pending_shape);
                    last_shape_send = Clock::now();
                    shape_sends_remaining--;
                }
            }

            HostPlatform::CursorState cstate;
            if (platform.get_cursor_state(cstate)) {
                protocol::CursorPositionMessage pos;
                pos.x_norm   = cstate.x_norm;
                pos.y_norm   = cstate.y_norm;
                pos.visible  = cstate.visible;
                pos.shape_id = cstate.shape_id;
                session.send_cursor_position(pos);
            }
        }

        if (!got_frame) {
            platform.on_idle();
            continue;
        }

        // Drain encoded packets and send.
        HostPlatform::EncodedPacketView pkt;
        while (platform.get_encoded_packet(pkt)) {
            uint32_t timestamp = static_cast<uint32_t>(pkt.pts & 0xFFFFFFFF);

            if (session.state() == host::SessionState::Connected) {
                if (pkt.keyframe) {
                    // Re-send StreamInfo on every keyframe so the client
                    // catches up quickly after a lost initial packet or
                    // a mid-session reconnect.
                    session.send_stream_info(static_cast<uint16_t>(cap_w),
                                             static_cast<uint16_t>(cap_h));
                }
                session.send_frame(pkt.data, pkt.len,
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
                log::info("HOST", "Frames: %llu, FPS: %.1f, RTT: %.1fms, retx: %llu, fec_k: %d, clients: %zu",
                    (unsigned long long)total_frames,
                    inst_fps,
                    session.rtt_ms(),
                    (unsigned long long)retx,
                    (int)fec_k,
                    session.client_count());
            }
        }
    }

    if (audio_capture) audio_capture->stop();
    session.stop();
    platform.shutdown();
    return 0;
}

} // namespace deskbeam
