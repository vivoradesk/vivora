#include "app/view_loop.h"
#include "common/crypto/host_identity.h"
#include "common/net/socket.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/stream_info.h"
#include "common/utils/log.h"
#include "common/utils/peer_code.h"
#include "common/utils/thread_priority.h"
#include "common/utils/types.h"
#include "client/net/client_session.h"

#include <fstream>

#include <chrono>
#include <thread>

namespace deskbeam {

int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg) {
    // Poll → FEC recover → NACK → decode → render all runs on this loop;
    // preemption here shows up directly as render jitter.
    utils::boost_current_thread_priority();
    // Connect session.
    client::ClientSession session;

    // The host's static pubkey is mandatory — Noise_NK won't run without it.
    // Paste it via --host-key (hex) at the CLI; the host prints its own key
    // on startup.
    // --host-key remains optional when --peer is a memorable code: the
    // rendezvous response carries the pubkey for us in that flow.  When
    // --host-key is present, it acts as an explicit pin and the lookup
    // result is verified against it.
    const bool has_peer_code = cfg.rendezvous_server && cfg.peer_pubkey_hex
        && *cfg.peer_pubkey_hex
        && !peer_code::looks_like_hex_pubkey(cfg.peer_pubkey_hex);
    if (cfg.host_key_hex && *cfg.host_key_hex) {
        uint8_t host_pk[32];
        if (!crypto::hex_decode_32(cfg.host_key_hex, host_pk)) {
            log::error("VIEW", "Invalid --host-key — expected 64 lowercase hex chars");
            return 1;
        }
        session.set_host_key(host_pk);
    } else if (!has_peer_code) {
        log::error("VIEW", "Missing --host-key HEX (64 hex chars) or --peer code. "
                           "Get either from the host's startup log.");
        return 1;
    }

    if (cfg.stun_server && *cfg.stun_server) {
        net::SocketAddr stun = net::resolve_host_port(cfg.stun_server);
        if (stun.ip == 0) {
            log::warn("VIEW", "Could not resolve STUN server '%s' — skipping discovery",
                      cfg.stun_server);
        } else {
            session.set_stun_server(stun);
        }
    }
    if (cfg.relay_server && *cfg.relay_server) {
        net::SocketAddr rly = net::resolve_host_port(cfg.relay_server);
        if (rly.ip == 0) {
            log::warn("VIEW", "Could not resolve relay '%s' — disabling", cfg.relay_server);
        } else {
            uint8_t sid[32]{};
            bool have_sid = false;
            if (cfg.relay_session_hex && *cfg.relay_session_hex) {
                if (!crypto::hex_decode_32(cfg.relay_session_hex, sid)) {
                    log::error("VIEW", "Invalid --relay-session — expected 64 lowercase hex chars");
                    return 1;
                }
                have_sid = true;
            }
            if (have_sid) {
                session.set_relay(rly, sid);
                log::info("VIEW", "Relay: %s (session pinned via --relay-session)", cfg.relay_server);
            } else {
                session.set_relay_endpoint(rly);
                log::info("VIEW", "Relay: %s (session will come from rendezvous)", cfg.relay_server);
            }
            if (cfg.license_file && *cfg.license_file) {
                std::ifstream lf(cfg.license_file, std::ios::binary);
                uint8_t token[95];
                if (lf && (lf.read(reinterpret_cast<char*>(token), 95),
                           lf.gcount() == 95)) {
                    session.set_relay_license(token);
                    log::info("VIEW", "License attached (%s)", cfg.license_file);
                } else {
                    log::warn("VIEW", "Could not read 95-byte license from %s",
                              cfg.license_file);
                }
            }
        }
    }
    if (cfg.rendezvous_server && *cfg.rendezvous_server
        && cfg.peer_pubkey_hex && *cfg.peer_pubkey_hex) {
        net::SocketAddr rdv = net::resolve_host_port(cfg.rendezvous_server);
        if (rdv.ip == 0) {
            log::warn("VIEW", "Could not resolve rendezvous '%s' — disabling",
                      cfg.rendezvous_server);
        } else {
            session.set_rendezvous(rdv);
            // --peer accepts either a 64-char hex pubkey OR a memorable
            // code from the host's startup line (e.g. "swift-tiger-4271").
            // Hex pin is more paranoid (no rendezvous trust); code is
            // the easier-to-share path with TOFU on the rdv response.
            if (peer_code::looks_like_hex_pubkey(cfg.peer_pubkey_hex)) {
                uint8_t peer_pk[32];
                if (!crypto::hex_decode_32(cfg.peer_pubkey_hex, peer_pk)) {
                    log::error("VIEW", "Invalid --peer hex");
                    return 1;
                }
                session.set_peer_pubkey(peer_pk);
                log::info("VIEW", "Rendezvous lookup (by pubkey): %s", cfg.rendezvous_server);
            } else if (peer_code::is_well_formed(cfg.peer_pubkey_hex)) {
                session.set_peer_code(cfg.peer_pubkey_hex);
                log::info("VIEW", "Rendezvous lookup (by code '%s'): %s",
                          cfg.peer_pubkey_hex, cfg.rendezvous_server);
            } else {
                log::error("VIEW",
                    "--peer must be a 64-char hex pubkey OR an 'adjective-noun-NNNN' code");
                return 1;
            }
        }
    }
    if (!session.start(cfg.host_ip ? cfg.host_ip : "0.0.0.0", cfg.port)) {
        log::error("VIEW", "Failed to start client session");
        return 1;
    }
    // ClientSession::start() already logged the resolved (post-rendezvous)
    // host endpoint — no need to print the placeholder cfg.host_ip here.

    // Wire input: window events → session → host.
    platform.set_input_callback([&session](const protocol::InputEvent& ev) {
        session.send_input(ev);
    });

    // Global IDR rate-limit. Both the drop-triggered block and the
    // no-keyframe-yet block share `last_idr_request`, so one trigger
    // fully suppresses the other until this interval elapses.
    //
    // At ~100KB per keyframe on WiFi, an IDR storm with 8+ requests
    // over 4s dumps ~1MB of keyframe traffic into an already-congested
    // channel and finishes the job loss started. 1500ms caps that at
    // ~3 requests per 4s worst case.
    // 600ms cooldown — enough to let the requested IDR make a round trip
    // (RTT + encode time ≈ 50-200ms typically, leaving headroom) and
    // prevent runaway storms, but ~2.5× faster recovery than the old
    // 1500ms.  When the IDR itself is lost in a burst, the second retry
    // fires while the original 1500ms-window freeze would still be on.
    constexpr int MIN_IDR_INTERVAL_MS = 600;

    bool got_keyframe = false;
    uint64_t frames_decoded = 0;
    uint64_t last_drops = 0;
    auto last_idr_request = TimePoint{};
    auto last_log_time = Clock::now();
    uint64_t last_log_frames = 0;
    uint64_t last_arrived_count = 0;
    float    last_arrived_fps   = 0.0f;

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
        //
        // Critical: only act on drops while a keyframe is currently in
        // play (got_keyframe=true). While we're waiting for the IDR we
        // already asked for (got_keyframe=false), drops keep climbing
        // because the host is still sending P-frames that can't be
        // decoded — retriggering IDR here would cascade: each request
        // dumps another ~200KB keyframe onto an already-congested link,
        // which causes more loss, which drops_increase, which requests
        // another IDR... The "No keyframe yet" block below owns recovery
        // while we're in that waiting state.
        uint64_t drops = session.frames_dropped();
        if (drops > last_drops) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            if (got_keyframe && since_idr_req > MIN_IDR_INTERVAL_MS) {
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
        // fires. Shares last_idr_request with the drop block via
        // MIN_IDR_INTERVAL_MS so a fresh drop-triggered request
        // suppresses this one until the new IDR has had time to land.
        if (!got_keyframe && session.state() == client::SessionState::Connected) {
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request).count();
            if (since_idr_req > MIN_IDR_INTERVAL_MS) {
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
                    // Flush decoder DPB before feeding the keyframe.  macOS
                    // VideoToolbox can emit CRA+RASL with non-resetting POC,
                    // which leaves stale references in libavcodec's DPB and
                    // produces "Could not find ref with POC X" warnings on
                    // every subsequent P-frame, manifesting as flicker.
                    // Flushing forces a clean restart from the keyframe.
                    platform.flush_decoder();
                    log::info("VIEW", "Got keyframe seq=%u (%zu bytes), starting decode",
                              net_frame.seq_no, net_frame.data.size());
                } else {
                    log::info("VIEW", "Pre-keyframe: dropping P-frame seq=%u", net_frame.seq_no);
                    continue;
                }
            }
            if (platform.decode(net_frame.data.data(), net_frame.data.size(),
                               net_frame.timestamp, net_frame.keyframe,
                               net_frame.seq_no)) {
                frames_fed++;
                // Count heartbeat in accepted too: assembler can't tell a
                // dropped heartbeat from a dropped real frame, so drops
                // accumulate even on a quiet stream. If we excluded
                // heartbeat from accepted, drop_pct = drops/(0+drops) =
                // 100% on any single heartbeat loss → adaptive ratchets
                // target_fps down hard. Including heartbeat in the
                // denominator keeps the ratio honest.
                session.note_decoder_accepted();
            } else if (net_frame.heartbeat) {
                // Decoder didn't like a heartbeat frame: harmless, the
                // next heartbeat ~18ms later will replace it. Skip the
                // IDR-cycle / metric pollution that a real-frame reject
                // would trigger.
                continue;
            } else {
                session.note_decoder_rejected();
                // Decoder rejected the frame — same recovery path as a
                // network-level drop: dump everything buffered and wait
                // for the next IDR.  Project rule: any sign of corruption
                // means we drop to a clean restart rather than risk
                // displaying half-decoded artifacts.
                //
                // Always switch to "waiting for IDR" mode and stop the
                // batch, even if the IDR request itself is rate-limited.
                // Otherwise the next pop_frame of the same batch would
                // re-enter decode() (which is still corrupt-latched),
                // count another false rejection, and the perf-report
                // metric would treat one corruption event as a sustained
                // overload — driving target_fps down for nothing.
                got_keyframe = false;
                platform.flush_decoder();
                auto now = Clock::now();
                auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_idr_request).count();
                if (since_idr_req > MIN_IDR_INTERVAL_MS) {
                    session.reset_video_stream();
                    session.request_idr();
                    last_idr_request = now;
                    log::warn("VIEW", "Decoder rejected frame seq=%u, dropped buffered + requested IDR + flush",
                              net_frame.seq_no);
                }
                break;  // stop feeding this batch — we just trashed state
            }
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
            // Only push cursor state if the host actually sent at least
            // one CursorPosition packet. Otherwise the default-constructed
            // message (visible=false, shape_id=0) would hijack the system
            // cursor on hosts that don't sync cursors (Linux PipeWire).
            if (session.has_cursor_position()) {
                platform.update_cursor_position(session.cursor_position());
            }
        }

        // Render decoded output.  On platforms where decode() already
        // renders (e.g. macOS AVSampleBuffer), render() returns 0 and
        // we count fed frames instead.
        int rendered = platform.render();
        frames_decoded += rendered > 0 ? rendered : frames_fed;

        // FPS logging + HUD stats update.  Runs once per second by wall
        // clock — earlier "every 60 decoded frames" version made the HUD
        // appear frozen on static screens (low decode rate => long gaps
        // between updates).  1 Hz keeps the displayed numbers fresh
        // regardless of FPS.
        auto now_check = Clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now_check - last_log_time).count() >= 1000) {
            auto now = now_check;
            double window_sec = std::chrono::duration<double>(now - last_log_time).count();
            double inst_fps = window_sec > 0
                ? (frames_decoded - last_log_frames) / window_sec
                : 0.0;
            last_log_time = now;
            last_log_frames = frames_decoded;
            log::info("VIEW", "Decoded: %llu, FPS: %.1f, RTT: %.1fms",
                (unsigned long long)frames_decoded, inst_fps, session.rtt_ms());

            // Network arrival rate (assembled frames since last sample).
            // Useful next to decode-fps to spot decoder-vs-network bottleneck:
            // if arrived ≈ source but decode lags, the client CPU is the
            // bottleneck; if arrived itself drops, the issue is upstream.
            uint64_t arrived_now = session.receiver()
                                   ? session.receiver()->frames_completed()
                                   : 0;
            last_arrived_fps = window_sec > 0
                ? static_cast<float>((arrived_now - last_arrived_count) / window_sec)
                : 0.0f;
            last_arrived_count = arrived_now;

            // Push HUD snapshot.  Many fields are quick to read; the
            // ones we don't have (audio PPS, decoder name) are filled
            // with best-effort placeholders for now.
            StatsView v{};
            v.fps          = static_cast<float>(inst_fps);
            v.arrived_fps  = last_arrived_fps;
            v.rtt_ms       = static_cast<float>(session.rtt_ms());
            v.bitrate_kbps   = session.last_bitrate_bps() / 1000;
            v.width          = session.stream_width();
            v.height         = session.stream_height();
            v.total_rejected = session.total_rejected();
            v.total_dropped  = session.total_dropped();
            if (auto* r = session.receiver()) {
                v.fec_recovered     = r->fec_recovered();
                v.fec_groups_failed = r->fec_failed();
            }
            v.target_fps = session.perf_target_fps();
            v.reject_pct = session.last_reject_pct();
            v.drop_pct   = session.last_drop_pct();
            v.audio_pps  = session.last_audio_pps();
            v.plc_pct    = session.last_plc_pct();
            std::snprintf(v.decoder, sizeof(v.decoder), "%s",
#if defined(DESKBEAM_LINUX)
                          "SW HEVC"
#elif defined(DESKBEAM_WINDOWS)
                          "MF HW"
#elif defined(DESKBEAM_MACOS)
                          "VTB HW"
#else
                          "?"
#endif
            );
            platform.update_stats(v);
        }

        // Adaptive sleep: if we did real work this tick (fed a frame or
        // rendered one), skip the 1ms nap and loop immediately — that
        // shaves up to 1ms of jitter off the render cadence at 60fps.
        // Only sleep when the loop is genuinely idle.
        const bool did_work = frames_fed > 0 || rendered > 0;
        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    session.stop();
    platform.shutdown();
    return 0;
}

} // namespace deskbeam
