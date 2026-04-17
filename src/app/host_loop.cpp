#include "app/host_loop.h"
#include "common/audio/audio_capture.h"
#include "common/codec/bitrate_controller.h"
#include "common/utils/log.h"
#include "common/utils/types.h"
#include "host/audio/audio_sender.h"
#include "host/session/host_session.h"

namespace deskbeam {

int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg) {
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

    // Start session.
    host::HostSession session;
    session.set_screen_resolution(platform.input_width(), platform.input_height());
    if (!session.start(cfg.port)) {
        log::error("HOST", "Failed to start session on port %u", cfg.port);
        return 1;
    }
    log::info("HOST", "Waiting for client on port %u... (Ctrl+C to stop)", cfg.port);

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
    auto last_idr_time = Clock::now();
    static constexpr int64_t IDR_INTERVAL_MS = 2000;

    // Retx-rate tracking: fed into bitrate controller as a congestion
    // signal in addition to client-reported FEC loss.
    uint64_t last_retx_sample = 0;
    uint64_t last_pkts_sample = 0;

    // Deadband: compare proposed bitrate against the last one we actually
    // applied to the encoder (not the controller's internal current), so
    // small accumulating drift still eventually crosses the 5% threshold.
    uint32_t last_applied_br = bitrate_ctl.current();
    bool had_clients = false;

    while (true) {
        session.poll();

        // Exit when all clients disconnect after we've had at least one.
        if (session.state() == host::SessionState::Disconnected && had_clients) {
            log::info("HOST", "All clients disconnected");
            break;
        }

        // Handle new client connections.
        if (session.consume_new_client_flag()) {
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
            if (changed) {
                uint32_t delta = br > last_applied_br
                                 ? br - last_applied_br
                                 : last_applied_br - br;
                if (delta * 20 >= last_applied_br) {
                    platform.set_bitrate(br);
                    last_applied_br = br;
                    log::info("HOST", "Bitrate changed -> %u kbps", br / 1000);
                }
            }
        }

        // IDR on client (re)connect.
        bool force_encode = false;
        if (session.idr_needed()) {
            platform.request_idr();
            session.clear_idr_needed();
            last_idr_time = Clock::now();
            force_encode = true;
            log::info("HOST", "IDR requested for new client");
        }

        // Periodic IDR for loss recovery.
        if (session.state() == host::SessionState::Connected) {
            auto since_idr = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - last_idr_time).count();
            if (since_idr >= IDR_INTERVAL_MS) {
                platform.request_idr();
                last_idr_time = Clock::now();
                force_encode = true;
            }
        }

        // Capture + encode.
        uint64_t pts_us = 0;
        bool content_changed = false;
        bool got_frame = platform.capture_and_encode(pts_us, content_changed, force_encode);

        if (!got_frame) {
            platform.on_idle();
            continue;
        }

        // Drain encoded packets and send.
        HostPlatform::EncodedPacketView pkt;
        while (platform.get_encoded_packet(pkt)) {
            uint32_t timestamp = static_cast<uint32_t>(pkt.pts & 0xFFFFFFFF);

            if (session.state() == host::SessionState::Connected) {
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
