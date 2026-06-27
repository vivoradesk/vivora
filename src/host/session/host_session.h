#pragma once

#include "common/codec/video_codec.h"
#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/monitor_info.h"
#include "common/protocol/stream_info.h"
#include "host/session/host_approval_gate.h"
#include "host/session/video_sender.h"
#include "host/session/paced_sender.h"
#include "host/audio/audio_sender.h"
#include "host/input/input_injector.h"
#include "common/utils/types.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <functional>
#include <string>

namespace vivora::host {

enum class SessionState { WaitingForClient, Connected, Disconnected };

// Per-client state tracked by the host.
struct ClientInfo {
    net::SocketAddr addr{};
    TimePoint connected_time;
    TimePoint last_recv_time;
    TimePoint last_ping_time;
    TimePoint ping_sent_time;
    uint32_t  ping_seq      = 0;
    double    rtt_ms         = 0.0;
    double    last_rtt_sent  = -1.0;
    bool      idr_needed     = true;
    float     loss_rate      = 0.0f;

    // Bandwidth probe state (per-client).
    uint16_t  probe_id       = 0;
    uint32_t  probe_bw_bps   = 0;
    bool      probe_pending  = false;
    bool      probe_scheduled = false;   // true once probe is queued; cleared after first run
    TimePoint probe_sent_time;

    // Adaptive framerate feedback from this client (Phase A/B: stored
    // and logged; Phase C will feed it into the encoder).
    uint16_t  perf_target_fps = 60;
    uint8_t   perf_reject_pct = 0;
    uint8_t   perf_drop_pct   = 0;

    // Noise_NK handshake state.  `handshake` is created on msg1 arrival and
    // destroyed once finalize() has populated `send_cs` / `recv_cs`.
    // `handshake_complete` gates any transport-level decrypt/encrypt.
    // `audio_send_cs` / `audio_recv_cs` are derived in the same finalize()
    // call from two extra HKDF outputs — used exclusively on the audio
    // socket so the nonce counters stay independent from video.
    std::unique_ptr<crypto::HandshakeStateNK> handshake;
    crypto::CipherState send_cs;
    crypto::CipherState recv_cs;
    crypto::CipherState audio_send_cs;
    crypto::CipherState audio_recv_cs;
    bool handshake_complete = false;

    // Where audio packets are sent for this client (client IP + the
    // audio_port the client advertised in its HELLO).  Mirrored into
    // AudioSender's destination list at handshake completion; we keep
    // a copy here so we can evict the AudioSender entry when this
    // client is removed (its CipherState would otherwise be freed
    // while AudioSender still holds a pointer to it).
    net::SocketAddr audio_dest{};

    // VIV-53 approval gate.  Latches once the HostApprovalGate reports
    // Approved so we don't keep re-querying the mutex per frame.
    // false → client is in Pending state (or gate is null = CLI mode,
    // in which case ::poll initialises to true on first frame).
    bool approved = false;
    // Per-connection capabilities granted at approval (VIV-60).  `input`
    // is enforced (view-only when false); clipboard/file_transfer flags
    // are stored for those features to honour when they ship.
    CapabilityGrant grant{};
    // Audio destination registered with AudioSender?  Cleared until
    // approval lands so a Rejected client never gets audio bytes.
    bool audio_registered = false;
    // Client's advertised audio port from HELLO — held until approval
    // promotes it into AudioSender's destination list.
    uint16_t audio_port_pending = 0;
};

class HostSession {
public:
    static constexpr uint16_t DEFAULT_PORT = 9876;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t PING_INTERVAL_MS = 1000;

    bool start(uint16_t port = DEFAULT_PORT);
    void stop();

    // Override the host-identity file location. Empty string (default) picks
    // the platform-native path from default_host_key_path().  Must be called
    // before start() — we load the key once at startup.
    void set_host_identity_path(const std::string& path) { host_identity_path_ = path; }

    // Host's long-term Curve25519 public key, as a 64-char lowercase hex
    // string.  Use this to print on startup / show in a QR so the client can
    // pin it via --host-key HEX.  Valid after start() succeeds.
    std::string host_public_key_hex() const;

    // Optional STUN server used at start() to discover our reflexive
    // (public) address. ip=0 disables it. The discovered address is only
    // logged today — it becomes the rendezvous point for p2p hole-punching
    // once signaling is in place.
    void set_stun_server(const net::SocketAddr& addr) { stun_server_ = addr; }
    net::SocketAddr reflexive_addr() const { return reflexive_addr_; }

    // Optional rendezvous server.  When set, start() registers the host's
    // long-term pubkey at the rendezvous so clients can locate us by id
    // through --peer.  poll() refreshes the registration every 30s and
    // reacts to PunchHint by opening a NAT pinhole toward the client.
    void set_rendezvous(const net::SocketAddr& addr) { rendezvous_addr_ = addr; }

    // Optional relay endpoint + 32-byte session id agreed with the client
    // out of band.  When set, start() BINDs to the relay and every
    // outbound packet to the (single) connected client goes through
    // relay-wrapped DBRL DATA; inbound DBRL DATA from the relay is
    // unwrapped and dispatched as if from the client's direct endpoint.
    // V1 limitation: only one concurrent client in relay mode (one
    // session_id, one binding); multi-client relay needs per-client
    // session_ids minted by the rendezvous, deferred to a later commit.
    void set_relay(const net::SocketAddr& addr, const uint8_t session_id[32]);
    // Endpoint-only: configure relay, but leave session_id to be filled
    // from the rendezvous RegisterAck.  Used when --relay is passed
    // without --relay-session.
    void set_relay_endpoint(const net::SocketAddr& addr) { relay_addr_ = addr; }
    // Optional 95-byte license token attached to the relay BIND.  Required
    // when the configured relay enforces --require-license; ignored by
    // self-host instances.
    void set_relay_license(const uint8_t token[95]);

    // Advertise which codec the host is encoding in.  Sent to the client
    // in HELLO_ACK so it can initialise the matching decoder.
    void set_codec(VideoCodec codec) { codec_ = codec; }

    // VIV-53 per-client approval gate.  When set, every new client
    // that completes handshake lands in Pending state — HostSession
    // skips frame sends + audio registration for it until the GUI
    // calls gate.set_state(key, Approved).  null = CLI mode, every
    // handshake is implicitly approved on completion.
    void set_approval_gate(std::shared_ptr<host::HostApprovalGate> gate) {
        approval_gate_ = std::move(gate);
    }

    // Force an immediate rendezvous re-registration on the next poll
    // iteration, bypassing the RDV_KEEPALIVE_S pacing.  Wired to the
    // GUI Refresh button so the user can "kick" the registration if
    // they suspect the server lost their entry.  Atomic flag — safe
    // to set from any thread.
    void request_rendezvous_refresh() { rendezvous_refresh_pending_ = true; }

    // Process incoming packets (handshake, pong). Call frequently.
    void poll();

    // Drain the next chunk of a keyframe being send-paced (VIV-82 option B).
    // Call every host-loop tick; clock-gated, sends nothing until the chunk
    // interval elapses.  No-op when no keyframe is pacing or pacing is off.
    void drain_kf_pacer();

    // Send an encoded frame to ALL connected clients.
    // Returns number of packets sent (sum), or -1 if no clients.
    // fec_enabled=false bypasses FEC for this frame (used by heartbeat
    // path on a static screen — losing a heartbeat is harmless and
    // putting it through FEC just inflates the failure counter).
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp, bool keyframe,
                   bool fec_enabled = true);

    // Force-emit parity for the in-flight FEC group and broadcast it to
    // every client, for the case where capture stops mid-group on a
    // static screen and the client never gets enough data to recover the
    // last partial frame. Returns total packets sent (0 if no group was
    // pending). See VideoSender::flush_pending_fec.
    int flush_video_fec(uint16_t frame_seq, uint32_t timestamp);

    void set_screen_resolution(uint32_t w, uint32_t h) {
        pending_screen_w_ = w;
        pending_screen_h_ = h;
        if (input_injector_) input_injector_->set_screen_resolution(w, h);
    }

    // Aggregate state across all clients.
    SessionState state() const { return state_; }
    size_t client_count() const { return clients_.size(); }

    // Seconds since the host last received a mouse / keyboard event from
    // ANY client.  0 if no input has ever been received this session.
    // GUI uses this for the idle-timeout warning + auto-disconnect feature.
    int64_t seconds_since_last_input() const;

    // Force-disconnect every currently-attached client.  Socket stays
    // open and new clients can still connect.  Triggered by the idle
    // timeout from host_loop.
    void disconnect_all_clients();

    // True if ANY client needs an IDR (new connect or explicit request).
    bool idr_needed() const;
    void clear_idr_needed();

    // Worst-case RTT across all clients (for bitrate controller).
    double rtt_ms() const;

    // Max loss rate across all clients.
    float last_loss_rate() const;

    // Effective (post-FEC/NACK) loss the client actually suffered — the worst
    // drop/reject % across clients, as a 0..1 ratio.  This is what the bitrate
    // controller should react to: raw channel loss that FEC fully recovers must
    // NOT hold the bitrate down (VIV-82) — only undelivered frames should.
    float last_effective_loss() const;

    // Best probe result (lowest ceiling wins for conservative adaptation).
    uint32_t probe_bw_bps() const;
    bool probe_pending() const;

    // Worst-case target_fps across all connected clients (the host has
    // to throttle to the slowest viewer).  Defaults to 60 when there are
    // no clients yet; floors at 15 to keep interactivity from collapsing.
    uint16_t min_perf_target_fps() const;

    VideoSender* sender() { return sender_.get(); }
    AudioSender* audio_sender() { return audio_sender_.get(); }

    // Broadcast a cursor position packet to all connected clients (unreliable).
    void send_cursor_position(const protocol::CursorPositionMessage& msg);
    // Broadcast a cursor shape packet to all connected clients. Caller is
    // expected to send 2-3 times (spaced ~100ms apart) to survive UDP loss.
    void send_cursor_shape(const protocol::CursorShapeMessage& msg);

    // Broadcast the real (pre-padding) stream dimensions so the client can
    // trim encoder-alignment padding and scale mouse input correctly.
    void send_stream_info(uint16_t width, uint16_t height);

    // Broadcast the current encoder target bitrate (kbps) so the client HUD can
    // show "encoding (actual)" — the gently-climbing target vs the measured
    // wire rate that fills it on content (VIV-82).
    void send_encoder_bitrate(uint32_t kbps);

    // VIV-50 monitor selection.  The session only marshals the wire messages;
    // host_loop owns the capture platform, so it answers a list request by
    // enumerating the platform and applies a switch request to it.
    //
    // consume_monitor_list_request(): true (and reset) if any client asked for
    // the display list since the last check — host_loop replies via
    // send_monitor_list(platform.list_monitors()).
    bool consume_monitor_list_request() {
        bool v = monitor_list_requested_;
        monitor_list_requested_ = false;
        return v;
    }
    // consume_monitor_select(): writes the requested display index and returns
    // true if a switch is pending (reset on read), else false.
    bool consume_monitor_select(uint32_t& index) {
        if (!monitor_select_pending_) return false;
        index = monitor_select_index_;
        monitor_select_pending_ = false;
        return true;
    }
    // Broadcast the capturable-display list to all connected clients.
    void send_monitor_list(const std::vector<protocol::MonitorDesc>& monitors);

    // True when a new client just connected since last check.
    // Consumed (reset) on read — used by host loop for warmup arming.
    bool consume_new_client_flag() {
        bool v = new_client_flag_;
        new_client_flag_ = false;
        return v;
    }

private:
    void handle_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender);
    void handle_hello(const uint8_t* payload, size_t len, const net::SocketAddr& sender);
    // Rendezvous wire integration — DBRV-magic packets are demuxed off the
    // top of handle_packet and forwarded here.  Same socket as video so any
    // NAT pinhole the rendezvous server opens (via REGISTER round-trip) is
    // exactly the binding the client will reach.
    void send_rendezvous_register();
    void handle_rendezvous_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender);
    // Called from handle_rendezvous_packet on PunchHint — sends a tiny UDP
    // probe toward the inbound client so port-restricted NATs accept the
    // upcoming HELLO.  The client side will be retrying HELLO anyway.
    void punch_to(const net::SocketAddr& client);
    // Seal `wire` with client's send_cs and push it out the main socket.
    // Returns true on success.  Used by every non-handshake send path so the
    // encryption layer lives in exactly one place.
    bool send_sealed(ClientInfo& client, const std::vector<uint8_t>& wire);
    void handle_pong(const uint8_t* payload, size_t len, const net::SocketAddr& sender);
    void handle_input(const uint8_t* payload, size_t len);
    void handle_bw_probe_ack(const uint8_t* payload, size_t len, const net::SocketAddr& sender);
    void send_ping(ClientInfo& client);
    void send_bw_probe(ClientInfo& client);

    ClientInfo* find_client(const net::SocketAddr& addr);

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<net::IUdpSocket> audio_socket_;
    std::unique_ptr<VideoSender> sender_;
    // Decoupled paced sender (VIV-82): all sealed wire sends route through it
    // so a frame goes out spread, not as a WiFi-dropping micro-burst.  Owns a
    // send thread that uses socket_ — reset before socket_ in stop().
    std::unique_ptr<PacedSender> paced_sender_;
    std::unique_ptr<AudioSender> audio_sender_;

    // Keyframe send-pacer (VIV-82 option B): a big keyframe is drained a chunk
    // at a time across host-loop ticks (clock-gated, no sleep) so it doesn't go
    // out as one ~100-packet burst the WiFi AP drops wholesale.  P-frames send
    // immediately.  Gated by VIVORA_KF_PACE.
    struct KfPacer {
        std::vector<std::vector<uint8_t>> wires;   // copy of prepared keyframe wires
        std::vector<net::SocketAddr>      dests;   // clients at enqueue time
        size_t            pos = 0;
        TimePoint         last_chunk{};
        bool              active = false;
    };
    KfPacer kf_pacer_;
    bool    kf_pace_enabled_ = false;
    // 1 packet per ~100 µs (~88 Mbps) — the rate the BW probe measured as
    // loss-free on this WiFi.  Sending even a small chunk (8) back-to-back
    // still overran the AP; one-at-a-time is what stays clean.  The host loop
    // spins far faster than 100 µs during the inter-capture idle, so this is
    // clock-gated with no sleep.
    static constexpr size_t  KF_PACE_CHUNK  = 1;
    static constexpr int64_t KF_PACE_GAP_US = 100;
    // Pace any frame with more than this many packets — P-frames burst-lose on
    // WiFi too (just smaller), which cut the bitrate to 3-5 Mbps; tiny frames
    // skip pacing to avoid needless latency.  A frame paces in size*100µs, well
    // under a 60fps interval.
    static constexpr size_t  PACE_MIN_PACKETS = 4;
    std::unique_ptr<InputInjector> input_injector_;
    SessionState state_ = SessionState::WaitingForClient;

    // Connected clients keyed by address.
    std::map<net::SocketAddr, ClientInfo> clients_;
    bool new_client_flag_ = false;

    // Last input event (mouse / keyboard) from any client, in
    // monotonic clock domain.  Default-constructed value means "no
    // input yet this session".  Updated in handle_input.
    TimePoint last_input_time_{};
    VideoCodec codec_ = VideoCodec::HEVC;

    uint32_t pending_screen_w_ = 0;
    uint32_t pending_screen_h_ = 0;

    // VIV-50 monitor selection request state (consumed by host_loop).
    bool     monitor_list_requested_ = false;
    bool     monitor_select_pending_ = false;
    uint32_t monitor_select_index_   = 0;

    // STUN discovery: target server (zeroed = disabled) and the result
    // captured at start() for external signaling to pick up.
    net::SocketAddr stun_server_{};
    net::SocketAddr reflexive_addr_{};

    // Rendezvous: when set, register host pubkey at this server post-STUN
    // and refresh every RDV_KEEPALIVE_S.  last_rdv_send_ paces keepalive
    // ticks driven from poll() — no separate timer thread.
    net::SocketAddr rendezvous_addr_{};
    TimePoint       last_rdv_send_{};
    static constexpr int64_t RDV_KEEPALIVE_S = 30;

    // VIV-53 connection-approval gate (optional).  Set by GUI mode.
    std::shared_ptr<host::HostApprovalGate> approval_gate_;
    // Force-refresh flag wired to the GUI Refresh button.  Atomic
    // because the GUI thread writes and the worker thread reads.
    std::atomic<bool> rendezvous_refresh_pending_{false};

    // Relay (v1: single concurrent client, manual session_id from CLI).
    net::SocketAddr relay_addr_{};
    uint8_t         relay_session_id_[32] = {};
    bool            relay_session_set_    = false;
    uint8_t         relay_alloc_id_[8]    = {};
    bool            relay_active_         = false;
    TimePoint       last_relay_keepalive_{};
    uint8_t         relay_license_[95]    = {};
    bool            relay_license_set_    = false;
    static constexpr int64_t RELAY_KEEPALIVE_S = 20;
    bool relay_bind_blocking();
    void relay_send_keepalive();
    // Wrap in DBRL DATA when relay_active_, else direct UDP to `peer`.
    int  transport_send(const uint8_t* data, size_t len, const net::SocketAddr& peer);

    // Long-term host identity.  Loaded once from disk (or generated on
    // first run) in start().  Its public key is shared out-of-band with the
    // client so Noise_NK can authenticate us.
    crypto::KeyPair host_identity_{};
    std::string     host_identity_path_;

    // Bandwidth probe constants.
    static constexpr uint16_t BW_PROBE_COUNT = 1000;
    static constexpr uint16_t BW_PROBE_SIZE  = 1200;
    static constexpr int64_t  BW_PROBE_TIMEOUT_MS = 10000;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace vivora::host
