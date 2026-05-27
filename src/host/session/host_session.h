#pragma once

#include "common/codec/video_codec.h"
#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/stream_info.h"
#include "host/session/video_sender.h"
#include "host/audio/audio_sender.h"
#include "host/input/input_injector.h"
#include "common/utils/types.h"
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

    // Process incoming packets (handshake, pong). Call frequently.
    void poll();

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
    std::unique_ptr<AudioSender> audio_sender_;
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
