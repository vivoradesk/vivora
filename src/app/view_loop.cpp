#include "app/view_loop.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/stream_info.h"
#include "common/utils/log.h"
#include "common/utils/types.h"
#include "client/net/client_session.h"

#include <chrono>
#include <thread>

namespace deskbeam {

int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg) {
    // Connect session.
    client::ClientSession session;
    if (!session.start(cfg.host_ip, cfg.port)) {
        log::error("VIEW", "Failed to start client session");
        return 1;
    }
    log::info("VIEW", "Connecting to %s:%u...", cfg.host_ip, cfg.port);

    // Wire input: window events → session → host.
    platform.set_input_callback([&session](const protocol::InputEvent& ev) {
        session.send_input(ev);
    });

    // Unified cooldown for both drop-triggered and no-keyframe-yet IDR requests.
    // Same clock for both so one block's request suppresses the other's until
    // the host has had time to produce and send the new IDR.
    constexpr int IDR_RETRY_MS = 250;

    bool got_keyframe = false;
    uint64_t frames_decoded = 0;
    uint64_t last_drops = 0;
    auto last_idr_request = TimePoint{};
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;

    while (true) {
        // Platform event pump (Qt processEvents / Cocoa pump / etc.).
        // Returns false when the window is closed.
        if (!platform.pump_events())
            break;

        session.poll();

        // Once the handshake completes we know the host codec — spin up
        // the decoder now, and also open the audio output device.  Both
        // idempotent after first success.
        static bool decoder_ready = false;
        if (!decoder_ready && session.state() == client::SessionState::Connected) {
            if (platform.init_decoder(session.host_codec())) {
                decoder_ready = true;
            }
        }
        static bool audio_started = false;
        if (!audio_started && session.state() == client::SessionState::Connected) {
            if (session.start_audio()) {
                log::info("VIEW", "Audio playback started");
            }
            audio_started = true;
        }

        if (session.state() == client::SessionState::Disconnected &&
            frames_decoded > 0) {
            log::info("VIEW", "Disconnected from host");
            break;
        }

        // Detect frame drops and request IDR for recovery.
        uint64_t drops = session.frames_dropped();
        if (drops > last_drops) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            if (since_idr_req > IDR_RETRY_MS) {
                session.reset_video_stream();
                session.request_idr();
                last_idr_request = now;
                got_keyframe = false;
                platform.flush_decoder();
                log::warn("VIEW", "Frame loss detected (%llu dropped), dropped buffered + requested IDR + flush",
                    (unsigned long long)(drops - last_drops));
            }
            last_drops = drops;
        }

        // No-keyframe-yet retry: a completely lost keyframe (all UDP
        // fragments dropped in one WiFi burst) is invisible to the
        // assembler's gap detection, so drops-triggered IDR above never
        // fires.  Shares last_idr_request with the drop block so a fresh
        // drop-triggered request suppresses this one until the new IDR
        // has had time to land.
        if (!got_keyframe && session.state() == client::SessionState::Connected) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            if (since_idr_req > IDR_RETRY_MS) {
                session.request_idr();
                last_idr_request = now;
                log::warn("VIEW", "No keyframe yet, requesting IDR");
            }
        }

        // Feed received frames to decoder (with keyframe gating).
        int frames_fed = 0;
        net::AssembledFrame net_frame;
        while (session.pop_frame(net_frame)) {
            if (!got_keyframe) {
                if (net_frame.keyframe) {
                    got_keyframe = true;
                    log::info("VIEW", "Got keyframe seq=%u (%zu bytes), starting decode",
                              net_frame.seq_no, net_frame.data.size());
                } else {
                    log::info("VIEW", "Pre-keyframe: dropping P-frame seq=%u", net_frame.seq_no);
                    continue;
                }
            }
            if (platform.decode(net_frame.data.data(), net_frame.data.size(),
                               net_frame.timestamp, net_frame.keyframe,
                               net_frame.seq_no))
                frames_fed++;
        }

        // Cursor sync: pull any fresh shape, then push current position
        // every tick so a moving cursor stays responsive even when the
        // underlying video is static (no encoded frame this tick).
        if (session.state() == client::SessionState::Connected) {
            protocol::StreamInfoMessage info;
            if (session.take_new_stream_info(info)) {
                platform.set_stream_size(info.width, info.height);
                log::info("VIEW", "Stream size: %ux%u (crop)",
                          info.width, info.height);
            }
            protocol::CursorShapeMessage new_shape;
            if (session.take_new_cursor_shape(new_shape)) {
                platform.upload_cursor_shape(new_shape);
            }
            platform.update_cursor_position(session.cursor_position());
        }

        // Render decoded output.  On platforms where decode() already
        // renders (e.g. macOS AVSampleBuffer), render() returns 0 and
        // we count fed frames instead.
        int rendered = platform.render();
        frames_decoded += rendered > 0 ? rendered : frames_fed;

        // FPS logging.
        if (frames_decoded > 0 && frames_decoded - last_log_frames >= 60) {
            auto now = Clock::now();
            double window_sec = std::chrono::duration<double>(now - last_log_time).count();
            double inst_fps = window_sec > 0
                ? (frames_decoded - last_log_frames) / window_sec
                : 0.0;
            last_log_time = now;
            last_log_frames = frames_decoded;
            log::info("VIEW", "Decoded: %llu, FPS: %.1f, RTT: %.1fms",
                (unsigned long long)frames_decoded, inst_fps, session.rtt_ms());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    session.stop();
    platform.shutdown();
    return 0;
}

} // namespace deskbeam
