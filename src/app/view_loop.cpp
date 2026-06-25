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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <chrono>
#include <thread>

namespace vivora {

ViewLoopState::ViewLoopState() = default;

ViewLoopState::~ViewLoopState() {
    teardown();
}

double ViewLoopState::rtt_ms() const { return session_.rtt_ms(); }

client::SessionState ViewLoopState::state() const { return session_.state(); }

void ViewLoopState::update_status(const char* text) {
    // Dedup by CONTENT (not literal pointer) so the overlay isn't re-shown /
    // raised every ~16ms tick.  Pointer comparison was fragile — distinct
    // literals can theoretically share storage and, more importantly, it made
    // the dedup state hard to reason about across reconnects.
    if (!text) text = "";
    if (status_shown_ == text) return;   // std::string vs const char* — content compare
    status_shown_ = text;
    log::info("VIEW", "status overlay -> \"%s\"", text);
    if (platform_) platform_->set_status(text);
}

void ViewLoopState::teardown() {
    if (torn_down_) return;
    torn_down_ = true;
    // Stop the decode thread before the session/platform (and the pipeline it
    // drives) are torn down (VIV-81).
    if (decode_running_.exchange(false)) {
        if (decode_thread_.joinable()) decode_thread_.join();
    }
    session_.stop();
    if (platform_) platform_->shutdown();
}

void ViewLoopState::decode_thread_proc() {
    bool got_kf = false;
    CompressedFrame cf;
    while (decode_running_.load(std::memory_order_acquire)) {
        if (!q1_->try_pop(cf)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        // Keyframe gating: drop P-frames until the first keyframe of a GOP.
        if (!got_kf) {
            if (cf.keyframe) { got_kf = true; pipeline_->flush_decoder(); }
            else continue;
        }
        SubmitStatus st = pipeline_->submit(cf.data.data(), cf.data.size(),
                                            cf.timestamp, cf.keyframe, cf.seq);
        if (st == SubmitStatus::Rejected) {
            if (cf.heartbeat) continue;  // harmless — next heartbeat replaces
            session_.note_decoder_rejected();
            got_kf = false;
            pipeline_->reinit_decoder();
            decode_needs_idr_.store(true, std::memory_order_release);
            continue;
        }
        // Pull every decoded frame this submit produced into Q2.
        for (;;) {
            FrameHandle h;
            PollStatus ps = pipeline_->poll_frame(h);
            if (ps != PollStatus::Produced) break;     // Empty or PoolFull
            session_.note_decoder_accepted();
            if (!q2_->try_push(h)) pipeline_->recycle(h);  // Q2 full: drop newest
        }
    }
}

bool ViewLoopState::iter_threaded() {
    auto& session  = session_;
    auto& platform = *platform_;
    const auto& cfg = *cfg_;

    if (cfg.stop_flag && cfg.stop_flag->load(std::memory_order_relaxed)) return false;
    if (user_disconnect_.load(std::memory_order_relaxed)) {
        log::info("VIEW", "Disconnect requested from in-stream menu");
        return false;
    }
    if (!platform.pump_events()) return false;
    session.poll();

    // Lazy init once Connected: pipeline decoder + decode thread, audio, clock.
    if (!decoder_ready_ && session.state() == client::SessionState::Connected) {
        if (pipeline_->init_decoder(session.host_codec())) {
            decoder_ready_ = true;
            decode_running_.store(true, std::memory_order_release);
            decode_thread_ = std::thread(&ViewLoopState::decode_thread_proc, this);
            log::info("VIEW", "Decode thread started");
        }
    }
    if (!audio_started_ && session.state() == client::SessionState::Connected) {
        if (session.start_audio()) log::info("VIEW", "Audio playback started");
        audio_started_ = true;
    }
    if (!session_started_ && session.state() == client::SessionState::Connected) {
        session_start_ = Clock::now();
        session_started_ = true;
    }

    // Status overlay before the first frame (VIV-62).
    if (frames_decoded_ == 0) {
        const auto st = session.state();
        if (st == client::SessionState::Connecting) update_status("Connecting…");
        else if (st == client::SessionState::Connected) update_status("Waiting for host to accept…");
    } else {
        update_status("");
    }

    // Disconnect teardown / linger.
    if (session.state() == client::SessionState::Disconnected) {
        if (frames_decoded_ > 0) {
            log::info("VIEW", "Disconnected from host");
            return false;
        }
        if (!disconnecting_) {
            disconnecting_ = true;
            disconnect_at_ = Clock::now();
            update_status("Host didn't accept the connection, or is unreachable");
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - disconnect_at_).count();
        if (waited > DISCONNECT_LINGER_MS) return false;
        return true;
    }

    // Network-loss → IDR (only once a frame is flowing; the decode thread owns
    // decode-error recovery separately via decode_needs_idr_).
    const uint64_t drops = session.frames_dropped();
    if (drops > last_drops_) {
        auto now = Clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_idr_request_).count();
        if (frames_decoded_ > 0 && since > MIN_IDR_INTERVAL_MS) {
            session.reset_video_stream();
            session.request_idr();
            last_idr_request_ = now;
            log::warn("VIEW", "Frame loss (%llu dropped) — requested IDR",
                      (unsigned long long)(drops - last_drops_));
        }
        last_drops_ = drops;
    }
    // No frame yet → keep asking for an IDR (lost first keyframe).
    if (frames_decoded_ == 0 && session.state() == client::SessionState::Connected) {
        auto now = Clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_idr_request_).count();
        if (since > MIN_IDR_INTERVAL_MS) {
            session.request_idr();
            last_idr_request_ = now;
        }
    }

    // Hand newly assembled compressed frames to the decode thread.
    net::AssembledFrame nf;
    while (session.pop_frame(nf)) {
        CompressedFrame cf;
        cf.data      = std::move(nf.data);   // ring slot keeps capacity
        cf.timestamp = nf.timestamp;
        cf.seq       = nf.seq_no;
        cf.keyframe  = nf.keyframe;
        cf.heartbeat = nf.heartbeat;
        if (!q1_->try_push(cf)) {
            // Q1 full — decode thread far behind; force a clean resync.
            decode_needs_idr_.store(true, std::memory_order_release);
        }
    }

    // Decode thread reported a decode error → no-artifact IDR recovery.
    // STICKY: only clear the flag once we actually send the IDR.  Clearing it
    // unconditionally (exchange) would drop the request whenever the throttle
    // blocked it, leaving the decode thread waiting for a keyframe nobody asks
    // for — a multi-second startup stall until the host's periodic IDR.
    if (decode_needs_idr_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_idr_request_).count();
        if (since > MIN_IDR_INTERVAL_MS) {
            decode_needs_idr_.store(false, std::memory_order_release);
            session.reset_video_stream();
            session.request_idr();
            last_idr_request_ = now;
        }
    }

    // Cursor + stream-size sync (same as legacy).
    if (session.state() == client::SessionState::Connected) {
        protocol::StreamInfoMessage info;
        if (session.take_new_stream_info(info)) {
            platform.set_stream_size(info.width, info.height);
        }
        protocol::CursorShapeMessage new_shape;
        if (session.take_new_cursor_shape(new_shape)) {
            platform.upload_cursor_shape(new_shape);
        }
        if (session.has_cursor_position()) {
            platform.update_cursor_position(session.cursor_position());
        }
    }

    // Drain Q2 with the render-penultimate policy: drop stale, render the
    // second-newest (1-frame cushion), keep the newest for next tick.
    int rendered = 0;
    if (q2_) {
        while (q2_->size() > 2) {
            FrameHandle d;
            if (q2_->try_pop(d)) pipeline_->recycle(d);
        }
        FrameHandle h;
        if (q2_->try_pop(h)) {
            pipeline_->present(h);   // copies into the view + schedules paint
            pipeline_->recycle(h);
            rendered = 1;
        }
    }
    frames_decoded_ += rendered;

    // Stats / HUD once per second (mirrors legacy iter()).
    auto now_check = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now_check - last_log_time_).count() >= 1000) {
        double window_sec = std::chrono::duration<double>(now_check - last_log_time_).count();
        double inst_fps = window_sec > 0
            ? (frames_decoded_ - last_log_frames_) / window_sec : 0.0;
        last_log_time_ = now_check;
        last_log_frames_ = frames_decoded_;
        log::info("VIEW", "Decoded: %llu, FPS: %.1f, RTT: %.1fms (threaded)",
                  (unsigned long long)frames_decoded_, inst_fps, session.rtt_ms());

        StatsView v{};
        v.fps          = static_cast<float>(inst_fps);
        v.rtt_ms       = static_cast<float>(session.rtt_ms());
        v.bitrate_kbps = session.last_bitrate_bps() / 1000;
        v.width        = session.stream_width();
        v.height       = session.stream_height();
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
        std::snprintf(v.codec, sizeof(v.codec), "%s",
                      session.host_codec() == VideoCodec::H264 ? "H.264" : "HEVC");
        std::snprintf(v.transport, sizeof(v.transport), "%s", session.transport_label());
        v.session_seconds = session_started_
            ? static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                  now_check - session_start_).count())
            : 0;
        std::snprintf(v.decoder, sizeof(v.decoder), "SW HEVC");
        platform.update_stats(v);
    }

    if (rendered == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return true;
}

bool ViewLoopState::init(ViewPlatform& platform, const ViewLoopConfig& cfg) {
    platform_ = &platform;
    cfg_      = &cfg;

    // Poll → FEC recover → NACK → decode → render all runs on this loop;
    // preemption here shows up directly as render jitter.
    utils::boost_current_thread_priority();

    // The host's static pubkey is mandatory — Noise_NK won't run without it.
    // --host-key is optional when connecting via a rendezvous --peer, in
    // EITHER form (VIV-76): a memorable code is resolved to the host pubkey by
    // the rendezvous LOOKUP, and a hex --peer IS the host's responder static
    // (pinned in the rendezvous block below).  Previously this gate accepted
    // only the code form, so clicking a Recent peer (which dials by hex
    // pubkey) with no --host-key was rejected here, before start() — that was
    // the "recent peer won't connect, pasted code does" bug.
    const bool has_rendezvous_peer = cfg.rendezvous_server && *cfg.rendezvous_server
        && cfg.peer_pubkey_hex && *cfg.peer_pubkey_hex;
    if (cfg.host_key_hex && *cfg.host_key_hex) {
        uint8_t host_pk[32];
        if (!crypto::hex_decode_32(cfg.host_key_hex, host_pk)) {
            log::error("VIEW", "Invalid --host-key — expected 64 lowercase hex chars");
            exit_code_ = 1;
            return false;
        }
        session_.set_host_key(host_pk);
    } else if (!has_rendezvous_peer) {
        log::error("VIEW", "Missing --host-key HEX (64 hex chars), or a --peer "
                           "(pubkey or code) together with a rendezvous server. "
                           "Get either from the host's startup log.");
        exit_code_ = 1;
        return false;
    }

    if (cfg.stun_server && *cfg.stun_server) {
        net::SocketAddr stun = net::resolve_host_port(cfg.stun_server);
        if (stun.ip == 0) {
            log::warn("VIEW", "Could not resolve STUN server '%s' — skipping discovery",
                      cfg.stun_server);
        } else {
            session_.set_stun_server(stun);
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
                    exit_code_ = 1;
                    return false;
                }
                have_sid = true;
            }
            if (have_sid) {
                session_.set_relay(rly, sid);
                log::info("VIEW", "Relay: %s (session pinned via --relay-session)", cfg.relay_server);
            } else {
                session_.set_relay_endpoint(rly);
                log::info("VIEW", "Relay: %s (session will come from rendezvous)", cfg.relay_server);
            }
            if (cfg.license_file && *cfg.license_file) {
                std::ifstream lf(cfg.license_file, std::ios::binary);
                uint8_t token[95];
                if (lf && (lf.read(reinterpret_cast<char*>(token), 95),
                           lf.gcount() == 95)) {
                    session_.set_relay_license(token);
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
            session_.set_rendezvous(rdv);
            if (peer_code::looks_like_hex_pubkey(cfg.peer_pubkey_hex)) {
                uint8_t peer_pk[32];
                if (!crypto::hex_decode_32(cfg.peer_pubkey_hex, peer_pk)) {
                    log::error("VIEW", "Invalid --peer hex");
                    exit_code_ = 1;
                    return false;
                }
                session_.set_peer_pubkey(peer_pk);
                // The peer's pubkey IS its Noise responder static — pin it as
                // the host key too, otherwise start() refuses (host key unset)
                // and a connect-by-pubkey (e.g. double-clicking an incoming
                // peer in Recent) fails even though the code path works.
                session_.set_host_key(peer_pk);
                log::info("VIEW", "Rendezvous lookup (by pubkey): %s", cfg.rendezvous_server);
            } else if (peer_code::is_well_formed(cfg.peer_pubkey_hex)) {
                session_.set_peer_code(cfg.peer_pubkey_hex);
                log::info("VIEW", "Rendezvous lookup (by code '%s'): %s",
                          cfg.peer_pubkey_hex, cfg.rendezvous_server);
            } else {
                log::error("VIEW",
                    "--peer must be a 64-char hex pubkey OR an 'adjective-noun-NNNN' code");
                exit_code_ = 1;
                return false;
            }
        }
    }
    if (!session_.start(cfg.host_ip ? cfg.host_ip : "0.0.0.0", cfg.port)) {
        log::error("VIEW", "Failed to start client session");
        exit_code_ = 1;
        return false;
    }

    // Wire input: window events → session → host.  Dropped silently while
    // view-only is engaged from the in-stream menu (VIV-74).
    platform.set_input_callback([this](const protocol::InputEvent& ev) {
        if (view_only_.load(std::memory_order_relaxed)) return;
        session_.send_input(ev);
    });

    // Wire the in-stream menu (VIV-74): volume/mute reach the audio receiver
    // through the session (cached until audio starts); view-only and
    // disconnect flip loop-local flags read in iter() / the input callback.
    MenuActions actions;
    actions.set_volume    = [this](float v) { session_.set_audio_volume(v); };
    actions.set_muted     = [this](bool m)  { session_.set_audio_muted(m); };
    actions.set_view_only = [this](bool on) {
        view_only_.store(on, std::memory_order_relaxed);
        log::info("VIEW", "view-only %s", on ? "ON" : "OFF");
    };
    actions.disconnect    = [this]() {
        user_disconnect_.store(true, std::memory_order_relaxed);
    };
    platform.set_menu_actions(actions);

    // Threaded pipeline (VIV-81): opt-in via VIVORA_PIPELINE=threaded, and
    // only if the platform actually provides an IVideoPipeline — otherwise
    // fall back to the legacy single-threaded path.
    if (const char* p = std::getenv("VIVORA_PIPELINE")) {
        if (std::strcmp(p, "threaded") == 0) {
            pipeline_ = platform.video_pipeline();
            if (pipeline_) {
                threaded_ = true;
                q1_ = std::make_unique<util::SpscRing<CompressedFrame, 8>>();
                q2_ = std::make_unique<util::SpscRing<FrameHandle, 4>>();
                log::info("VIEW", "Threaded view loop enabled (decode thread + lock-free Q1/Q2)");
            } else {
                log::warn("VIEW", "VIVORA_PIPELINE=threaded but platform has no "
                                  "IVideoPipeline — using legacy path");
            }
        }
    }

    last_log_time_ = Clock::now();
    return true;
}

bool ViewLoopState::iter() {
    if (threaded_) return iter_threaded();

    auto& session  = session_;
    auto& platform = *platform_;
    const auto& cfg = *cfg_;

    // External stop (GUI Stop button, AppController shutdown).
    if (cfg.stop_flag && cfg.stop_flag->load(std::memory_order_relaxed)) {
        return false;
    }

    // In-stream menu Disconnect button (VIV-74).
    if (user_disconnect_.load(std::memory_order_relaxed)) {
        log::info("VIEW", "Disconnect requested from in-stream menu");
        return false;
    }

    // Platform event pump (Qt processEvents / Cocoa pump / etc.).
    // Returns false when the window is closed.
    if (!platform.pump_events()) return false;

    session.poll();

    // Once the handshake completes we know the host codec — spin up
    // the decoder now, and also open the audio output device.  Both
    // idempotent after first success.
    if (!decoder_ready_ && session.state() == client::SessionState::Connected) {
        if (platform.init_decoder(session.host_codec())) {
            decoder_ready_ = true;
        }
    }
    if (!audio_started_ && session.state() == client::SessionState::Connected) {
        if (session.start_audio()) {
            log::info("VIEW", "Audio playback started");
        }
        audio_started_ = true;
    }
    if (!session_started_ && session.state() == client::SessionState::Connected) {
        session_start_ = Clock::now();
        session_started_ = true;
    }

    // Status overlay (VIV-62): before the first frame, tell the user what's
    // happening instead of a blank window.  Cleared once frames flow.
    if (frames_decoded_ == 0) {
        const auto st = session.state();
        if (st == client::SessionState::Connecting)
            update_status("Connecting…");
        else if (st == client::SessionState::Connected)
            update_status("Waiting for host to accept…");
    } else {
        update_status("");
    }

    // Tear the view down on any Disconnected transition.  The state only
    // reaches Disconnected after the connect/silence timeouts in
    // ClientSession, so the initial Connecting phase is unaffected.
    if (session.state() == client::SessionState::Disconnected) {
        if (frames_decoded_ > 0) {
            log::info("VIEW", "Disconnected from host");
            return false;
        }
        // Never received a frame — host ignored/rejected the prompt, or is
        // unreachable.  Linger briefly with a reason so the window doesn't
        // just vanish, then close.
        if (!disconnecting_) {
            disconnecting_ = true;
            disconnect_at_ = Clock::now();
            update_status("Host didn't accept the connection, or is unreachable");
            log::warn("VIEW", "Connection closed before any video — "
                              "host rejected the request or is unreachable");
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - disconnect_at_).count();
        if (waited > DISCONNECT_LINGER_MS) return false;
        // Keep the window alive so the message is visible.
        return true;
    }

    // Detect frame drops and request IDR for recovery.  Only act while a
    // keyframe is currently in play (got_keyframe=true); the no-keyframe
    // branch below owns recovery while we're waiting for IDR.
    uint64_t drops = session.frames_dropped();
    if (drops > last_drops_) {
        auto now = Clock::now();
        auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_idr_request_).count();
        if (got_keyframe_ && since_idr_req > MIN_IDR_INTERVAL_MS) {
            session.reset_video_stream();
            session.request_idr();
            last_idr_request_ = now;
            got_keyframe_ = false;
            platform.flush_decoder();
            log::warn("VIEW", "Frame loss detected (%llu dropped), dropped buffered + requested IDR + flush",
                (unsigned long long)(drops - last_drops_));
        }
        last_drops_ = drops;
    }

    // No-keyframe-yet retry: a completely lost keyframe (all UDP
    // fragments dropped in one WiFi burst) is invisible to the
    // assembler's gap detection.  Shares last_idr_request_ with the
    // drop block via MIN_IDR_INTERVAL_MS.
    if (!got_keyframe_ && session.state() == client::SessionState::Connected) {
        auto now = Clock::now();
        auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_idr_request_).count();
        if (since_idr_req > MIN_IDR_INTERVAL_MS) {
            session.request_idr();
            last_idr_request_ = now;
            log::warn("VIEW", "No keyframe yet, requesting IDR");
        }
    }

    // Feed received frames to decoder (with keyframe gating).
    int frames_fed = 0;
    net::AssembledFrame net_frame;
    while (session.pop_frame(net_frame)) {
        if (!got_keyframe_) {
            if (net_frame.keyframe) {
                got_keyframe_ = true;
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
            session.note_decoder_accepted();
        } else if (net_frame.heartbeat) {
            // Decoder didn't like a heartbeat: harmless, next one ~18ms
            // later will replace it.  Skip the IDR cycle.
            continue;
        } else {
            session.note_decoder_rejected();
            // Decoder rejected → flush DPB + request IDR.  Project rule:
            // any sign of corruption means we drop to clean restart.
            got_keyframe_ = false;
            platform.flush_decoder();
            auto now = Clock::now();
            auto since_idr_req = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_idr_request_).count();
            if (since_idr_req > MIN_IDR_INTERVAL_MS) {
                session.reset_video_stream();
                session.request_idr();
                last_idr_request_ = now;
                log::warn("VIEW", "Decoder rejected frame seq=%u, dropped buffered + requested IDR + flush",
                          net_frame.seq_no);
            }
            break;  // stop feeding this batch — we just trashed state
        }
    }

    // Cursor sync: pull any fresh shape, then push current position every
    // tick so a moving cursor stays responsive even when video is static.
    if (session.state() == client::SessionState::Connected) {
        protocol::StreamInfoMessage info;
        if (session.take_new_stream_info(info)) {
            platform.set_stream_size(info.width, info.height);
            log::info("VIEW", "Stream size: %ux%u (crop)", info.width, info.height);
        }
        protocol::CursorShapeMessage new_shape;
        if (session.take_new_cursor_shape(new_shape)) {
            platform.upload_cursor_shape(new_shape);
        }
        if (session.has_cursor_position()) {
            platform.update_cursor_position(session.cursor_position());
        }
    }

    int rendered = platform.render();
    frames_decoded_ += rendered > 0 ? rendered : frames_fed;

    // FPS logging + HUD stats update.  Runs once per second by wall clock.
    auto now_check = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now_check - last_log_time_).count() >= 1000) {
        auto now = now_check;
        double window_sec = std::chrono::duration<double>(now - last_log_time_).count();
        double inst_fps = window_sec > 0
            ? (frames_decoded_ - last_log_frames_) / window_sec
            : 0.0;
        last_log_time_ = now;
        last_log_frames_ = frames_decoded_;
        log::info("VIEW", "Decoded: %llu, FPS: %.1f, RTT: %.1fms",
            (unsigned long long)frames_decoded_, inst_fps, session.rtt_ms());

        uint64_t arrived_now = session.receiver()
                               ? session.receiver()->frames_completed()
                               : 0;
        last_arrived_fps_ = window_sec > 0
            ? static_cast<float>((arrived_now - last_arrived_count_) / window_sec)
            : 0.0f;
        last_arrived_count_ = arrived_now;

        StatsView v{};
        v.fps          = static_cast<float>(inst_fps);
        v.arrived_fps  = last_arrived_fps_;
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
        // Menu header fields (VIV-74).
        std::snprintf(v.codec, sizeof(v.codec), "%s",
                      session.host_codec() == VideoCodec::H264 ? "H.264" : "HEVC");
        std::snprintf(v.transport, sizeof(v.transport), "%s", session.transport_label());
        v.session_seconds = session_started_
            ? static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                  now - session_start_).count())
            : 0;
        std::snprintf(v.decoder, sizeof(v.decoder), "%s",
#if defined(VIVORA_LINUX)
                      "SW HEVC"
#elif defined(VIVORA_WINDOWS)
                      "MF HW"
#elif defined(VIVORA_MACOS)
                      "VTB HW"
#else
                      "?"
#endif
        );
        platform.update_stats(v);
    }

    // Adaptive sleep: if we did real work, skip the 1ms nap and loop
    // immediately — shaves up to 1ms of jitter off render cadence at
    // 60fps.  Only sleep when the loop is genuinely idle.  In GUI mode
    // the QTimer paces us, so this sleep is harmless (timer interval
    // dominates).  Keep it for CLI parity.
    const bool did_work = frames_fed > 0 || rendered > 0;
    if (!did_work) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

int run_view_loop(ViewPlatform& platform, const ViewLoopConfig& cfg) {
    ViewLoopState state;
    if (!state.init(platform, cfg)) return state.exit_code();
    while (state.iter()) {}
    return state.exit_code();
}

} // namespace vivora
