// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/host_loop.h"
#include "common/audio/audio_capture.h"
#include "common/crypto/host_identity.h"   // hex_decode_32
#include <fstream>
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
#include <thread>

namespace vivora {

namespace {

// Stamp the platform's switch capability onto every advertised display, so a
// viewer never offers a "switch to this monitor" control that the host will
// silently ignore (Linux enumerates displays but cannot retarget capture).
std::vector<vivora::protocol::MonitorDesc>
advertised_monitors(vivora::HostPlatform& platform) {
    auto list = platform.list_monitors();
    const bool can_switch = platform.supports_monitor_switch();
    for (auto& m : list) m.switchable = can_switch;
    return list;
}

} // namespace

int run_host_loop(HostPlatform& platform, const HostLoopConfig& cfg) {
    // Capture → encode → fragment → send all runs single-threaded on this
    // loop; a background compile or Windows Update scan preempting it adds
    // straight jitter to end-to-end latency.
    utils::boost_current_thread_priority();
    // Mutable: a VIV-50 monitor switch re-targets capture to a display of a
    // possibly different resolution, after which these are refreshed and the
    // new dimensions are pushed to clients via StreamInfo.
    uint32_t cap_w = platform.capture_width();
    uint32_t cap_h = platform.capture_height();

    // VIV-67 stream framerate cap (Settings → HostWorkerConfig → here).
    // Guard against a zeroed config so the interval math below never
    // divides by zero.
    const uint16_t fps_cap = cfg.max_fps > 0 ? cfg.max_fps : 60;

    // Bitrate controller — picks a sensible default from resolution +
    // configured framerate, allows a manual override via --bitrate, and
    // exposes hooks for future congestion-control feedback (RTT / loss /
    // bandwidth estimate).
    codec::BitrateController bitrate_ctl(
        codec::default_bitrate_for(cap_w, cap_h, fps_cap));
    if (cfg.manual_bitrate_bps != 0) {
        bitrate_ctl.set_manual_target(cfg.manual_bitrate_bps);
        bitrate_ctl.tick();
    }
    log::info("HOST", "Initial bitrate: %u kbps (%s)",
              bitrate_ctl.current() / 1000,
              cfg.manual_bitrate_bps ? "manual" : "auto");

    // Diagnostic env-var: raise the hard warmup/recovery ceiling above
    // 10 Mbps so a real channel-capacity test can ramp further.  Remembered
    // in `base_ceiling_override` so the relay profile below can restore it
    // when a session flips from relay back to direct.
    uint32_t base_ceiling_override = 0;
    if (const char* env = std::getenv("VIVORA_MAX_BPS")) {
        uint32_t max_bps = static_cast<uint32_t>(std::atoll(env));
        if (max_bps > 0) {
            base_ceiling_override = max_bps;
            bitrate_ctl.set_ceiling_override(max_bps);
            log::info("HOST", "VIVORA_MAX_BPS=%u -> ceiling override %u kbps",
                      max_bps, max_bps / 1000);
        }
    }

    // VIV-114 relay profile.  When a session runs THROUGH the relay (not a
    // direct P2P link), the free relay box (~48 Mbps NIC, shared across many
    // concurrent sessions) cannot carry a full adaptive stream.  While any
    // attached client is relay-reached we clamp the encoder to a conservative
    // wire budget plus an fps ceiling, decoupled from the P2P adaptive
    // controller so direct sessions keep the full behaviour.  Both knobs are
    // overridable for tuning / testing against a beefier relay.
    uint32_t relay_max_bps = 5'000'000;   // relay wire budget (bps)
    uint16_t relay_max_fps = 30;          // relay fps ceiling
    if (const char* env = std::getenv("VIVORA_RELAY_MAX_BPS")) {
        uint32_t v = static_cast<uint32_t>(std::atoll(env));
        if (v > 0) relay_max_bps = v;
    }
    if (const char* env = std::getenv("VIVORA_RELAY_MAX_FPS")) {
        int v = std::atoi(env);
        if (v > 0 && v <= 240) relay_max_fps = static_cast<uint16_t>(v);
    }
    // Tracks whether the relay clamp is currently applied, so we only act on
    // the direct<->relay transition.
    bool relay_profile_on = false;

    // Start session.
    host::HostSession session;
    session.set_screen_resolution(platform.input_width(), platform.input_height());
    session.set_screen_origin(platform.input_origin_x(), platform.input_origin_y());
    session.set_configured_codec(platform.actual_codec());
    if (cfg.approval_gate) session.set_approval_gate(cfg.approval_gate);
    if (cfg.stun_server && *cfg.stun_server) {
        net::SocketAddr stun = net::resolve_host_port(cfg.stun_server);
        if (stun.ip == 0) {
            log::warn("HOST", "Could not resolve STUN server '%s' — skipping discovery",
                      cfg.stun_server);
        } else {
            session.set_stun_server(stun);
        }
    }
    if (cfg.rendezvous_server && *cfg.rendezvous_server) {
        net::SocketAddr rdv = net::resolve_host_port(cfg.rendezvous_server);
        if (rdv.ip == 0) {
            log::warn("HOST", "Could not resolve rendezvous '%s' — disabling",
                      cfg.rendezvous_server);
        } else {
            session.set_rendezvous(rdv);
            log::info("HOST", "Rendezvous: %s", cfg.rendezvous_server);
        }
    }
    if (cfg.relay_server && *cfg.relay_server) {
        net::SocketAddr rly = net::resolve_host_port(cfg.relay_server);
        if (rly.ip == 0) {
            log::warn("HOST", "Could not resolve relay '%s' — disabling", cfg.relay_server);
        } else {
            uint8_t sid[32]{};
            bool have_sid = false;
            if (cfg.relay_session_hex && *cfg.relay_session_hex) {
                if (!crypto::hex_decode_32(cfg.relay_session_hex, sid)) {
                    log::error("HOST", "Invalid --relay-session — expected 64 lowercase hex chars");
                    if (cfg.error_out)
                        *cfg.error_out = "Invalid relay session id — expected 64 "
                                         "lowercase hex characters.";
                    return 1;
                }
                have_sid = true;
            }
            // Manual session_id pin OR wait for the rendezvous-minted one
            // (filled in HostSession::handle_rendezvous_packet on RegisterAck).
            if (have_sid) {
                session.set_relay(rly, sid);
                log::info("HOST", "Relay: %s (session pinned via --relay-session)", cfg.relay_server);
            } else {
                session.set_relay_endpoint(rly);
                log::info("HOST", "Relay: %s (session will come from rendezvous)", cfg.relay_server);
            }
            if (cfg.license_file && *cfg.license_file) {
                std::ifstream lf(cfg.license_file, std::ios::binary);
                uint8_t token[95];
                if (lf && (lf.read(reinterpret_cast<char*>(token), 95),
                           lf.gcount() == 95)) {
                    session.set_relay_license(token);
                    log::info("HOST", "License attached (%s)", cfg.license_file);
                } else {
                    log::warn("HOST", "Could not read 95-byte license from %s",
                              cfg.license_file);
                }
            }
        }
    }
    if (!session.start(cfg.port)) {
        log::error("HOST", "Failed to start session on port %u", cfg.port);
        if (cfg.error_out) {
            // Overwhelmingly this is a second copy of Vivora already sharing:
            // the first one holds the port, the second starts, fails, and used
            // to stop with nothing said -- so from the outside the machine
            // simply stopped accepting connections.
            *cfg.error_out =
                "Could not listen on port " + std::to_string(cfg.port) +
                ", or on any of the nine after it. Another copy of Vivora is "
                "probably already sharing on this machine — quit it, or "
                "choose a different port in Settings.";
        }
        return 1;
    }
    // session.port(), not cfg.port: a busy port is stepped over rather than
    // being fatal (VIV-19), so the two are not always the same number.
    log::info("HOST", "Waiting for client on port %u... (Ctrl+C to stop)",
              session.port());

    // Diagnostic env-var: freeze FEC M at a fixed value, disabling
    // loss-adaptive and RTT-lock behavior. For probing the real channel
    // capacity with known redundancy (e.g. M=10 at K=10 = 50%).
    if (const char* env = std::getenv("VIVORA_FEC_M")) {
        int m = std::atoi(env);
        if (m > 0 && m <= 32 && session.sender()) {
            session.sender()->set_force_m(static_cast<uint8_t>(m));
            log::info("HOST", "VIVORA_FEC_M=%d -> force M (adaptive disabled)", m);
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
    // Encode-time aggregator — reset every periodic log tick.  We measure
    // the platform.capture_and_encode() duration rather than just the
    // encoder kernel because that's the wall-clock cost the host_loop
    // sees, which is what really constrains capture cadence.
    double   enc_min_ms = 1e9, enc_max_ms = 0.0, enc_sum_ms = 0.0;
    // Capture is timed apart from encode (the platform reports its share of
    // capture_and_encode), because they are separate stages of the pipeline
    // and a latency budget is argued about per stage.
    double   cap_max_ms = 0.0, cap_sum_ms = 0.0;
    uint64_t cap_count = 0;
    uint64_t enc_count  = 0;
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
    double   retx_ewma        = 0.0;
    auto     last_retx_time   = std::chrono::steady_clock::now();
    // Actual wire-throughput sampling (~2Hz) for the controller's
    // utilization gate — the climb only proceeds when the wire really
    // carries close to the current target (VIV-84).
    uint64_t last_bytes_sample = 0;
    auto     last_bytes_time   = std::chrono::steady_clock::now();

    // Deadband: compare proposed bitrate against the last one we actually
    // applied to the encoder (not the controller's internal current), so
    // small accumulating drift still eventually crosses the 5% threshold.
    uint32_t last_applied_br = bitrate_ctl.current();
    auto last_stats_send = std::chrono::steady_clock::now();
    // VIV-147: ~1 Hz display hot-plug / mode-change poll.
    auto last_display_poll = std::chrono::steady_clock::now();
    auto loss_grace_until = std::chrono::steady_clock::now();
    bool had_clients = false;
    // Phase B+: encoder lifecycle.  We track the previous tick's
    // client count to fire start_encoder() exactly once on the 0→N
    // transition and stop_encoder() exactly once on N→0.  Platforms
    // that haven't opted in to lazy encoding (Linux / macOS today)
    // have no-op start/stop_encoder so this is harmless.
    int  prev_client_count = 0;
    // Idle-timeout state — first crossing fires the warning callback,
    // second crossing (warn + warning_sec) force-disconnects.  Reset
    // whenever the client count drops to zero (we always grant a
    // freshly-attached client a full idle budget).
    bool idle_warned = false;

    // Adaptive framerate throttle.  Clients send PerfReport once per second
    // with their sustainable target_fps; the host paces capture+encode to
    // the slowest client.  EWMA over the last few samples avoids reacting
    // to single-interval spikes, and we log every applied change so we can
    // see the throttle live in the host log alongside bitrate adjustments.
    // The user's framerate cap (VIV-67) seeds the state and bounds the
    // per-tick target below — adaptation only ever lowers the rate.
    auto     last_capture_time   = TimePoint{};
    uint16_t applied_target_fps  = fps_cap;
    float    target_fps_ewma     = static_cast<float>(fps_cap);
    int64_t  min_frame_interval_us = 1'000'000 / fps_cap;
    // Push-model platforms (Linux PipeWire) pace their own capture
    // callback with this — host_loop's gate below never sees frames the
    // compositor pushes at panel rate (VIV-67).
    platform.set_min_frame_interval_us(min_frame_interval_us);

    // FEC group tail-flush: when capture stays silent on a static screen,
    // any in-progress FEC group (P-frame fragments not yet K-aligned) sits
    // unparityied. The client can't recover the last partial frame, so e.g.
    // a mouse-drag selection that disappears on host stays visible on the
    // client. Periodic flush keeps this window short.
    auto last_send_time          = TimePoint{};
    uint16_t last_sent_seq       = 0;
    uint32_t last_sent_ts        = 0;
    auto last_fec_flush_time     = TimePoint{};

    while (true) {
        // Drain the next chunk of any keyframe being send-paced (VIV-82 B).
        // The loop spins fast between captures, so this clock-gated call spreads
        // a big keyframe over several ms without any sleep.
        session.drain_kf_pacer();

        // GUI cooperative stop.  CLI never sets this and uses Ctrl+C.
        if (cfg.stop_flag && cfg.stop_flag->load(std::memory_order_relaxed)) {
            log::info("HOST", "Stop requested by controller");
            break;
        }

        // GUI Refresh button → force rendezvous re-register on next poll.
        if (cfg.rendezvous_refresh_flag
            && cfg.rendezvous_refresh_flag->exchange(false)) {
            session.request_rendezvous_refresh();
        }

        // VIV-52: kick any live viewer whose device just lost account
        // membership.  The GUI thread queues its static pubkey on the shared
        // approval gate when the mesh diff drops it; we drain + apply here.
        if (cfg.approval_gate) {
            for (const auto& hex : cfg.approval_gate->take_kicks())
                session.disconnect_client_by_pubkey(hex);
        }

        session.poll();

        // VIV-22 clipboard sync: GUI thread <-> session handoff via the
        // shared bridge.  Outbound (host user copied something) → broadcast
        // to granted viewers; inbound (viewer copied) → hand to the GUI
        // thread's ClipboardSync to write the local clipboard.
        if (cfg.clipboard) {
            protocol::ClipboardMessage clip;
            if (cfg.clipboard->take_outbound(clip)) {
                session.send_clipboard(clip);
            }
            if (session.take_new_clipboard(clip)) {
                cfg.clipboard->push_inbound(std::move(clip));
            }
        }

        // Auto-exit on "all clients disconnected" is CLI-only behaviour:
        // headless host process is one-shot per session.  GUI host stays
        // up indefinitely, polled by AppController, so we suppress the
        // auto-exit when stop_flag is wired.  VIVORA_HOST_STAY=1 keeps the
        // CLI host up across client churn too — a WiFi blip killing the
        // client shouldn't take the whole test rig down (VIV-84 rig QoL).
        static const bool host_stay = [] {
            const char* e = std::getenv("VIVORA_HOST_STAY");
            return e && e[0] == '1';
        }();
        if (session.state() == host::SessionState::Disconnected
            && had_clients && !cfg.stop_flag) {
            if (!host_stay) {
                log::info("HOST", "All clients disconnected");
                break;
            }
            had_clients = false;  // re-arm for the next client's session
            log::info("HOST", "All clients disconnected — staying up (VIVORA_HOST_STAY)");
        }

        // Publish state for the GUI poll.  Cheap atomic stores; cost is
        // negligible compared to the encode kernel below.
        if (cfg.client_count_out) {
            cfg.client_count_out->store(
                static_cast<int>(session.client_count()),
                std::memory_order_relaxed);
        }
        if (cfg.state_out) {
            cfg.state_out->store(
                session.state() == host::SessionState::Connected ? 1 : 0,
                std::memory_order_relaxed);
        }

        // VIV-114: apply / lift the relay profile as the session's relay
        // state changes.  Detection is host-side — a client reached through
        // the relay is keyed under the synthetic sentinel addr, so this is
        // true only when the shared encoded stream really traverses the relay
        // box (a direct-LAN client returns false even if the host is also
        // relay-bound for other peers).  Runs BEFORE the new-client warmup so
        // the hard client-cap clamps the warmup-start burst on the very first
        // relayed client, not one tick late.
        {
            const bool relayed_now = session.any_client_relayed();
            if (relayed_now != relay_profile_on) {
                relay_profile_on = relayed_now;
                if (relayed_now) {
                    // Clamp the recovery ceiling AND the connect-time BW-probe
                    // hard cap (set_probe_bandwidth() caps at the ceiling
                    // override) to the relay wire budget, so the probe can't
                    // burst the encoder far above the relay NIC.  The
                    // authoritative clamp is the client-cap applied each tick
                    // below — set here too so the very first warmup obeys it.
                    bitrate_ctl.set_ceiling_override(relay_max_bps);
                    bitrate_ctl.set_client_cap(relay_max_bps);
                    log::info("HOST",
                              "Relay session -> relay profile ON "
                              "(wire <= %u kbps, fps <= %u)",
                              relay_max_bps / 1000, (unsigned)relay_max_fps);
                } else {
                    bitrate_ctl.set_ceiling_override(base_ceiling_override);
                    bitrate_ctl.set_client_cap(
                        session.min_client_bitrate_cap_bps());
                    log::info("HOST",
                              "Direct session -> relay profile OFF, "
                              "full adaptive restored");
                }
            }
        }

        // Phase B+: encoder lifecycle on client_count transitions.
        // Runs BEFORE the new-client block so set_bitrate / request_idr
        // calls below land on a live encoder.  On a start_encoder()
        // failure we bail out the client — better than leaving a half-
        // initialised session that the next bitrate tick will crash on.
        {
            const int now_count = static_cast<int>(session.client_count());
            if (prev_client_count == 0 && now_count > 0) {
                if (!platform.start_encoder()) {
                    log::error("HOST", "start_encoder() failed — dropping connecting client");
                    session.disconnect_all_clients(
                        protocol::DisconnectReason::EncoderFailed);
                    prev_client_count = 0;
                    continue;
                }
            }
            if (prev_client_count > 0 && now_count == 0) {
                platform.stop_encoder();
            }
            prev_client_count = now_count;
        }

        // Handle new client connections.
        if (session.consume_new_client_flag()) {
            // Tell the new client the real (pre-padding) frame size so
            // its renderer crops encoder-alignment padding and its mouse
            // mapping matches the host screen.  Repeated on every
            // keyframe below for loss resilience.
            session.send_stream_info(static_cast<uint16_t>(cap_w),
                                     static_cast<uint16_t>(cap_h),
                                     applied_target_fps);
            // `n <= 1` must take the first-client path even when
            // had_clients is stale (a reconnect after the previous viewer
            // dropped): the proportional-cut branch below computes
            // current*(n-1)/n = 0 at n=1 and slams the session to the
            // 1 Mbps floor.
            const size_t n_clients = session.client_count();
            if (!had_clients || n_clients <= 1) {
                // First client: the capture geometry is real by now (the
                // encoder just started) — refresh the auto default the
                // controller was built with, which is floor-garbage when
                // dimensions weren't known at host-loop start (lazy
                // encoder), then arm the warm-up ramp (cold start).
                bitrate_ctl.update_default(codec::default_bitrate_for(
                    platform.capture_width(), platform.capture_height(), fps_cap));
                bitrate_ctl.set_client_count(1);
                bitrate_ctl.notify_client_connected();
                platform.set_bitrate(bitrate_ctl.current());
                last_applied_br = bitrate_ctl.current();
                log::info("HOST", "First client connected, warmup -> %u kbps",
                          last_applied_br / 1000);
                last_retx_sample = session.sender() ? session.sender()->retransmits() : 0;
                last_pkts_sample = session.sender() ? session.sender()->packets_sent() : 0;
                last_bytes_sample = session.sender() ? session.sender()->bytes_sent() : 0;
                last_bytes_time   = std::chrono::steady_clock::now();
            } else {
                // Additional client (n >= 2): cut bitrate proportionally so
                // total wire rate doesn't spike (N clients share the link).
                bitrate_ctl.set_client_count(n_clients);
                uint32_t new_br = bitrate_ctl.current() * (n_clients - 1) / n_clients;
                if (new_br < 1'000'000) new_br = 1'000'000;
                bitrate_ctl.force_bitrate(new_br);
                platform.set_bitrate(new_br);
                last_applied_br = new_br;
                log::info("HOST", "New client connected (%zu total), bitrate -> %u kbps",
                          n_clients, new_br / 1000);
            }
            had_clients = true;
        }

        // Lazy-encoder gate: when nobody's attached, skip the entire
        // capture / encode / wire path — DXGI Duplicate1, the VAAPI /
        // NVENC / AMF / QSV kernels, FEC, and the FPS-paced send all
        // sit idle.  We still tick session.poll() at full speed (so a
        // fresh HELLO is picked up within ~10ms) and call platform.on_idle
        // so DXGI can release the previous frame.  GUI host stays "Listening"
        // in the UI but uses near-zero GPU until the first client lands.
        if (session.client_count() == 0) {
            // Resetting idle bookkeeping here means a client that
            // disconnects and reconnects gets a fresh idle budget.
            idle_warned = false;
            platform.on_idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Idle-timeout (GUI Phase B): warn and then force-disconnect
        // clients that haven't sent any input in `idle_timeout_min`.
        // Disabled when idle_timeout_min == 0 (CLI default).
        if (cfg.idle_timeout_min > 0) {
            const int64_t idle_sec = session.seconds_since_last_input();
            const int64_t warn_at  = static_cast<int64_t>(cfg.idle_timeout_min) * 60;
            const int64_t disc_at  = warn_at + cfg.idle_warning_sec;
            if (!idle_warned && idle_sec >= warn_at) {
                idle_warned = true;
                if (cfg.on_idle_warning) {
                    cfg.on_idle_warning(cfg.idle_warning_sec);
                }
                log::info("HOST", "Idle %llds — warning issued, disconnect in %ds",
                          (long long)idle_sec, cfg.idle_warning_sec);
            }
            if (idle_warned && idle_sec >= disc_at) {
                log::info("HOST", "Idle %llds — force-disconnecting clients",
                          (long long)idle_sec);
                session.disconnect_all_clients(
                    protocol::DisconnectReason::IdleTimeout);
                idle_warned = false;
                continue;   // skip this tick's encode work; loop top will
                            // see client_count==0 and take the lazy path
            }
        }

        // Keep bitrate controller aware of client count so it can
        // divide the ceiling (total wire = bitrate × clients).
        bitrate_ctl.set_client_count(session.client_count());

        // Feed BW probe result to bitrate controller (once).
        if (session.probe_bw_bps() > 0 && !session.probe_pending()) {
            bitrate_ctl.set_probe_bandwidth(session.probe_bw_bps());
        }

        // Feed telemetry to bitrate controller.  Loss signal = client FEC loss
        // (channel loss before recovery) maxed with host retx rate.  (VIV-82
        // tried post-FEC "effective" loss to dodge the inflated metric, but it
        // probed up, choked, and oscillated/floored at ~6 Mbps with periodic
        // dips — worse than stable raw-loss.  Reverted; the metric fix belongs
        // in the FEC loss accounting, not the controller.)
        bitrate_ctl.on_rtt(session.rtt_ms());
        if (session.sender()) {
            // Sample actual sent throughput for the utilization gate.
            auto now_bt = std::chrono::steady_clock::now();
            auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now_bt - last_bytes_time).count();
            if (dt_ms >= 500) {
                uint64_t cur_bytes = session.sender()->bytes_sent();
                uint64_t d_bytes = cur_bytes - last_bytes_sample;
                bitrate_ctl.on_wire_usage(static_cast<uint32_t>(
                    d_bytes * 8000 / static_cast<uint64_t>(dt_ms)));
                last_bytes_sample = cur_bytes;
                last_bytes_time   = now_bt;
            }
        }
        {
            // Cut driver = EFFECTIVE loss (post-FEC/NACK client damage:
            // dropped/rejected frames).  This link taught us why (VIV-84):
            // its radio steadily loses ~4-5% raw at ANY rate, FEC+NACK
            // recover all of it (client damage 0.0%, picture perfect) — yet
            // the raw pre-FEC signal kept the controller permanently in the
            // cut band, pinning a 90 Mbps-probed link to the 6M floor.
            // Recovered loss is the redundancy machinery WORKING, not
            // congestion.  (The VIV-82 attempt at this failed because
            // nothing then stopped the climb before real damage; now the
            // utilization gate + recovery-traffic/RTT climb guards in the
            // controller are that early warning.)
            double loss_signal = session.last_effective_loss();
            if (session.sender()) {
                // Retx ratio over a ≥500ms window, EWMA-smoothed.  It used to
                // be computed over a mere 20-packet window and fed raw: a NACK
                // batch recovering one keyframe burst (~30-40 retx) inside a
                // 20-packet window read as "100% loss" → instant ×0.5, and the
                // same event smeared across two adapt windows read as
                // SUSTAINED congestion → full cut + recovery penalty — while
                // the client's own loss EWMA said 0.0% (everything recovered).
                // That trap deepens as bitrate drops (same absolute burst =
                // larger %), which is why the bitrate could never leave the
                // floor on dynamic content (VIV-84).
                uint64_t cur_retx = session.sender()->retransmits();
                uint64_t cur_pkts = session.sender()->packets_sent();
                uint64_t d_retx = cur_retx - last_retx_sample;
                uint64_t d_pkts = cur_pkts - last_pkts_sample;
                auto now_rt = std::chrono::steady_clock::now();
                auto rt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_rt - last_retx_time).count();
                if (rt_ms >= 500 && d_pkts >= 20) {
                    double retx_ratio = static_cast<double>(d_retx)
                                      / static_cast<double>(d_pkts);
                    retx_ewma = retx_ewma * 0.75 + retx_ratio * 0.25;
                    last_retx_sample = cur_retx;
                    last_pkts_sample = cur_pkts;
                    last_retx_time   = now_rt;
                }
                // Recovery traffic gates the CLIMB (don't grow while the
                // redundancy machinery is straining) but never cuts.
                bitrate_ctl.on_recovery_traffic(retx_ewma);
            }
            // Suppress loss for a grace period after an IDR recovery: the
            // client's FEC decoder reset on the drop reports a burst of
            // "missing" packets that is an artifact of the reset, not real
            // congestion.  Feeding it crashed the bitrate to the floor on every
            // freeze, so the bitrate "stuck at 4M" (VIV-82).
            static const bool br_trace = [] {
                const char* e = std::getenv("VIVORA_BR_TRACE");
                return e && e[0] == '1';
            }();
            const bool graced = std::chrono::steady_clock::now() < loss_grace_until;
            if (graced) loss_signal = 0.0;
            if (br_trace && (loss_signal > 0.005 || retx_ewma > 0.005)) {
                static auto last_ls_log = std::chrono::steady_clock::time_point{};
                auto now_ls = std::chrono::steady_clock::now();
                if (now_ls - last_ls_log >= std::chrono::milliseconds(500)) {
                    last_ls_log = now_ls;
                    log::info("BRTRACE",
                              "loss-signal eff=%.2f%% (raw=%.2f%% retx_ewma=%.2f%%%s)",
                              loss_signal * 100.0, session.last_loss_rate() * 100.0,
                              retx_ewma * 100.0, graced ? ", graced->0" : "");
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
                // Carve FEC parity out of the wire budget.  pct = overhead %
                // (per-frame mode: the pooled %; legacy: 100*M/K) — the formula
                // below is identical to the old k/(k+m) when pct = 100*M/K.
                uint32_t pct = s->fec_overhead_pct();
                encoder_bps = static_cast<uint32_t>(
                    static_cast<uint64_t>(br) * 100 / (100 + pct));
            }
            // Floor — at FAILURE_DRIVEN_M_MAX=30 with K=10 the carve-out
            // takes the encoder to 25% of wire (e.g. 250 kbps from a
            // 1 Mbps starting budget) which makes a frozen mud picture.
            // Clamp at 500 kbps so the encoder always has enough to keep
            // a recognisable stream even when M is fully ramped.
            constexpr uint32_t MIN_ENCODER_BPS = 500'000;
            if (encoder_bps < MIN_ENCODER_BPS) encoder_bps = MIN_ENCODER_BPS;
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

            // ~1 Hz: tell the client our encoder target so its HUD can show
            // "encoding (actual)" — the gently-climbing target vs the measured
            // wire that fills it on content (VIV-82).
            auto now_stats = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_stats - last_stats_send).count() >= 1000) {
                session.send_encoder_bitrate(last_applied_br / 1000);
                last_stats_send = now_stats;
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
            // Don't let the post-reset loss burst (FEC decoder reset on the
            // client) crash the bitrate — ignore loss for ~1.5s (VIV-82).
            loss_grace_until = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(1500);
            // Trigger origin is logged at the source (HostSession logs
            // "Client requested IDR (frame loss recovery)" for the loss
            // path; "First client connected" / "New client connected"
            // for the handshake path).  Keep this line neutral so a
            // recovery storm doesn't look like a client-flap storm in
            // the host log.
            log::info("HOST", "Forwarding IDR request to encoder");
        }

        // VIV-50 monitor selection.  Answer a display-list request by
        // enumerating the platform, and apply a pending display switch.
        if (session.consume_monitor_list_request()) {
            session.send_monitor_list(advertised_monitors(platform));
        }

        // VIV-147 display hot-plug.  Nothing asks the host to re-enumerate on
        // its own, so a monitor plugged in or unplugged mid-session left the
        // viewer's panel showing the startup list forever — and if the change
        // resized the display we are capturing, the encoder kept producing
        // frames at the old geometry.  Poll ~1 Hz (cheap on every platform;
        // see HostPlatform::poll_display_change) and push the truth out.
        {
            auto now_disp = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now_disp - last_display_poll).count() >= 1000) {
                last_display_poll = now_disp;
                if (platform.poll_display_change()) {
                    log::info("HOST", "Display configuration changed — re-advertising monitors");
                    if (platform.refresh_capture()) {
                        // The captured display itself resized: everything
                        // derived from its geometry has to be re-armed, exactly
                        // as after an explicit monitor switch.
                        cap_w = platform.capture_width();
                        cap_h = platform.capture_height();
                        session.set_screen_resolution(platform.input_width(),
                                                      platform.input_height());
                        session.set_screen_origin(platform.input_origin_x(),
                                                  platform.input_origin_y());
                        session.send_stream_info(static_cast<uint16_t>(cap_w),
                                                 static_cast<uint16_t>(cap_h),
                                                 applied_target_fps);
                        platform.request_idr();
                        force_encode = true;
                        loss_grace_until = std::chrono::steady_clock::now()
                                         + std::chrono::milliseconds(1500);
                        log::info("HOST", "Capture geometry now %ux%u", cap_w, cap_h);
                    }
                    session.send_monitor_list(advertised_monitors(platform));
                }
            }
        }
        // VIV-112 codec negotiation: a client whose decoder can't handle the
        // configured codec (advertised in its HELLO caps, or a runtime decode
        // failure) has driven the session to pick a different codec.  Rebuild
        // the encoder for it and re-sync the session so future HELLO_ACKs
        // advertise the live codec.
        vivora::VideoCodec want_codec;
        if (session.consume_codec_change(want_codec)) {
            if (platform.set_codec(want_codec)) {
                session.set_codec(platform.actual_codec());
                platform.request_idr();   // clean re-init for the new codec
                force_encode = true;
                loss_grace_until = std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds(1500);
                log::info("HOST", "Codec switched to %s per client negotiation",
                          platform.actual_codec() == vivora::VideoCodec::HEVC
                              ? "HEVC" : "H.264");
            } else {
                log::warn("HOST", "Codec switch to %s not supported on this "
                          "platform — staying on %s",
                          want_codec == vivora::VideoCodec::HEVC ? "HEVC" : "H.264",
                          platform.actual_codec() == vivora::VideoCodec::HEVC
                              ? "HEVC" : "H.264");
            }
            // Announce either way.  On success every connected viewer must
            // follow the new codec, not only the one that drove the switch; on
            // a refusal the newcomer's HELLO_ACK promised the codec we did not
            // switch to, and this corrects it (VIV-147).
            session.announce_codec();
        }

        uint32_t want_monitor = 0;
        if (session.consume_monitor_select(want_monitor)) {
            log::info("HOST", "Client requested switch to display %u", want_monitor);
            if (platform.select_monitor(want_monitor,
                                        /*seed_cursor=*/session.has_remote_clients())) {
                // Capture now targets a (possibly) different-resolution display:
                // refresh dims, re-arm input mapping, tell clients the new size,
                // and force a keyframe so the decoder re-inits cleanly.
                cap_w = platform.capture_width();
                cap_h = platform.capture_height();
                session.set_screen_resolution(platform.input_width(),
                                              platform.input_height());
                session.set_screen_origin(platform.input_origin_x(),
                                          platform.input_origin_y());
                session.send_stream_info(static_cast<uint16_t>(cap_w),
                                         static_cast<uint16_t>(cap_h),
                                         applied_target_fps);
                platform.request_idr();
                force_encode = true;
                // The switch resets the client's decoder (new resolution) — give
                // the resulting loss burst the same grace as an IDR recovery so
                // it doesn't crash the bitrate (VIV-82).
                loss_grace_until = std::chrono::steady_clock::now()
                                 + std::chrono::milliseconds(1500);
                // Re-advertise so the panel's "viewing" highlight follows.
                session.send_monitor_list(advertised_monitors(platform));
                log::info("HOST", "Switched to display %u (%ux%u)",
                          want_monitor, cap_w, cap_h);
            } else {
                log::warn("HOST", "Display switch to %u failed — staying on current",
                          want_monitor);
                // Re-advertise so the viewer's optimistic highlight snaps back
                // to the display we are actually streaming.  Without this the
                // panel keeps claiming it switched.
                session.send_monitor_list(advertised_monitors(platform));
            }
        }

        // VIV-147: a monitor switch, a hot-plug rebuild or the first encoder
        // start can land on an HDR display, where the platform promotes H.264
        // to HEVC Main10 on its own.  Nothing above asked for that change, so
        // catch it here by comparing the live encoder with what the viewers
        // were told, and tell them.
        if (platform.actual_codec() != session.codec()) {
            log::info("HOST", "Live codec is now %s (encoder rebuild) — announcing",
                      platform.actual_codec() == vivora::VideoCodec::HEVC
                          ? "HEVC" : "H.264");
            session.set_codec(platform.actual_codec());
            session.announce_codec();
        }

        // Adaptive framerate gate: skip this iteration's capture if it
        // would arrive sooner than the negotiated min frame interval.
        // Update the smoothed target every iteration but only re-arm
        // the interval when EWMA crosses ±2 fps from the applied value
        // (prevents single-spike whiplash).
        {
            // Viewer-requested bitrate cap (PerfReport bytes [4..8), 0 =
            // none): hard clamp inside the controller, cheap no-op when
            // unchanged.  VIV-114: while relayed, fold in the relay wire
            // budget (tightest of viewer cap and relay budget wins) so the
            // encoder stays within the relay NIC on every adaptation path.
            uint32_t eff_bitrate_cap = session.min_client_bitrate_cap_bps();
            if (relay_profile_on
                && (eff_bitrate_cap == 0 || relay_max_bps < eff_bitrate_cap))
                eff_bitrate_cap = relay_max_bps;
            bitrate_ctl.set_client_cap(eff_bitrate_cap);
            // min_perf_target_fps(cap) already clamps client reports to the
            // user's cap (VIV-67), so the EWMA can never ratchet above it.
            // VIV-114: while relayed, lower the effective cap to the relay fps
            // ceiling so the paced capture rate drops to it (decoupled from
            // P2P — full cap restored the moment the session goes direct).
            const uint16_t eff_fps_cap = relay_profile_on
                ? std::min<uint16_t>(fps_cap, relay_max_fps)
                : fps_cap;
            uint16_t want = session.min_perf_target_fps(eff_fps_cap);
            target_fps_ewma = 0.7f * target_fps_ewma + 0.3f * static_cast<float>(want);
            uint16_t smoothed = static_cast<uint16_t>(target_fps_ewma + 0.5f);
            int diff = static_cast<int>(smoothed) - static_cast<int>(applied_target_fps);
            if (diff >= 2 || diff <= -2) {
                applied_target_fps = smoothed;
                min_frame_interval_us = 1'000'000 / std::max<uint16_t>(smoothed, 1);
                platform.set_min_frame_interval_us(min_frame_interval_us);
                log::info("HOST", "Adaptive framerate -> %u fps (interval %lld us)",
                          applied_target_fps,
                          static_cast<long long>(min_frame_interval_us));
                // Push the new effective target to clients right away so
                // their HUDs track it without waiting for the next keyframe.
                session.send_stream_info(static_cast<uint16_t>(cap_w),
                                         static_cast<uint16_t>(cap_h),
                                         applied_target_fps);
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
        const auto enc_start = Clock::now();
        bool got_frame = platform.capture_and_encode(pts_us, content_changed, force_encode);
        if (got_frame) {
            last_capture_time = Clock::now();
            const double total_ms = std::chrono::duration<double, std::milli>(
                last_capture_time - enc_start).count();
            // What the platform spent getting the frame, and what is left --
            // which is the encode.  Platforms that do not measure capture
            // report 0, and then this reads exactly as it did before.
            const double cap_ms = platform.last_capture_ms();
            const double enc_ms = cap_ms > 0.0 && cap_ms <= total_ms
                                ? total_ms - cap_ms : total_ms;
            if (cap_ms > 0.0) {
                if (cap_ms > cap_max_ms) cap_max_ms = cap_ms;
                cap_sum_ms += cap_ms;
                cap_count++;
            }
            if (enc_ms < enc_min_ms) enc_min_ms = enc_ms;
            if (enc_ms > enc_max_ms) enc_max_ms = enc_ms;
            enc_sum_ms += enc_ms;
            enc_count++;
        }

        // Constant-rate heartbeat: when capture stays silent on a static
        // screen, re-feed the last texture so the wire keeps the same
        // packet cadence as active streaming. Keeps WiFi / routers from
        // dropping the link into low-power state, keeps FEC groups filling
        // at the normal rate, and means a release-and-stop event (mouse
        // drag end, last keystroke) doesn't strand the final partial frame.
        // Triggers ~one frame interval past the expected real frame, so
        // active flows never see it; static idle gets full frame rate.
        if (!got_frame && session.state() == host::SessionState::Connected
            && last_send_time.time_since_epoch().count() != 0) {
            const auto now = Clock::now();
            const auto since_send_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - last_send_time).count();
            // Honour adaptive frame interval: if client ratched target down
            // (e.g. target=30 → interval=33ms), heartbeat fires at the same
            // cadence so we don't overshoot client's stated capacity.
            if (since_send_us >= min_frame_interval_us + 2000) {
                pts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count();
                if (platform.re_encode_last(pts_us)) {
                    got_frame = true;
                    last_capture_time = now;
                }
            }
        }

        // Tail-flush in-progress FEC group when the wire's been silent for
        // a while. Sender feeds P-frame fragments into FEC groups; if a
        // group hasn't reached K shards by the time capture stops (e.g.
        // user releases a mouse drag), no parity is sent and the client
        // can't reconstruct the last partial frame on any loss. Forcing a
        // partial-group flush emits parity (K_eff < K, M parity packets)
        // so the client closes the group and recovers the tail frame.
        // 150ms is one ping past human reaction time — long enough that
        // active flows don't trigger it, short enough that a release-and-
        // wait scenario doesn't visibly stick.
        if (!got_frame && session.state() == host::SessionState::Connected
            && last_send_time.time_since_epoch().count() != 0) {
            const auto now = Clock::now();
            const auto since_send = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_send_time).count();
            const auto since_flush = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_fec_flush_time).count();
            if (since_send >= 150 && since_flush >= 150) {
                int n = session.flush_video_fec(last_sent_seq, last_sent_ts);
                if (n > 0) {
                    log::info("HOST", "FEC tail-flush: %d parity packets after %lldms idle",
                              n, (long long)since_send);
                }
                last_fec_flush_time = now;
            }
        }

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
                                             static_cast<uint16_t>(cap_h),
                                             applied_target_fps);
                }
                session.send_frame(pkt.data, pkt.len,
                                   frame_seq, timestamp, pkt.keyframe,
                                   /*fec_enabled=*/!pkt.heartbeat);
                last_send_time = Clock::now();
                last_sent_seq  = frame_seq;
                last_sent_ts   = timestamp;
            }

            frame_seq++;
            total_frames++;

            const auto now_log = Clock::now();
            const double window_sec = std::chrono::duration<double>(now_log - last_log_time).count();
            if (window_sec >= 1.0) {
                const double inst_fps = (total_frames - last_log_frames) / window_sec;
                last_log_time   = now_log;
                last_log_frames = total_frames;
                const uint64_t retx = session.sender() ? session.sender()->retransmits() : 0;
                const uint8_t  fec_k = session.sender() ? session.sender()->fec_group_size() : 0;
                const double avg_ms = enc_count > 0 ? enc_sum_ms / static_cast<double>(enc_count) : 0.0;
                const double min_ms = enc_count > 0 ? enc_min_ms : 0.0;
                const double cap_avg = cap_count > 0
                                     ? cap_sum_ms / static_cast<double>(cap_count) : 0.0;
                log::info("HOST",
                    "Frames: %llu, FPS: %.1f, capture: %.2f/%.2f ms (avg/max), "
                    "encode: %.2f/%.2f/%.2f ms (min/avg/max), "
                    "RTT: %.1fms, retx: %llu, fec_k: %d, clients: %zu",
                    (unsigned long long)total_frames,
                    inst_fps,
                    cap_avg, cap_max_ms,
                    min_ms, avg_ms, enc_max_ms,
                    session.rtt_ms(),
                    (unsigned long long)retx,
                    (int)fec_k,
                    session.client_count());
                enc_min_ms = 1e9; enc_max_ms = 0.0; enc_sum_ms = 0.0; enc_count = 0;
                cap_max_ms = 0.0; cap_sum_ms = 0.0; cap_count = 0;
            }
        }
    }

    if (audio_capture) audio_capture->stop();
    session.stop();
    platform.shutdown();
    return 0;
}

} // namespace vivora
