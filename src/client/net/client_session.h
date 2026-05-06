#pragma once

#include "common/codec/video_codec.h"
#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "client/net/video_receiver.h"
#include "client/audio/audio_receiver.h"
#include "common/audio/audio_output.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include "common/protocol/stream_info.h"
#include "common/utils/types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace deskbeam::client {

enum class SessionState { Disconnected, Connecting, Connected };

class ClientSession {
public:
    static constexpr int64_t HELLO_RETRY_MS = 500;
    static constexpr int64_t CONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;

    // Pin the host's long-term Curve25519 public key. MUST be called before
    // start() — Noise_NK refuses to run without a known responder static.
    // Input is a raw 32-byte pubkey (hex-decoded at the CLI layer).
    void set_host_key(const uint8_t host_pk[32]);

    // Connect to host at given IP:port. Non-blocking — call poll() to drive.
    bool start(const char* host_ip, uint16_t port);
    void stop();

    // Optional STUN server used at start() to discover our reflexive
    // address. ip=0 disables it. The discovered address is only logged
    // today — it becomes the rendezvous point for p2p hole-punching once
    // signaling is in place.
    void set_stun_server(const net::SocketAddr& addr) { stun_server_ = addr; }
    net::SocketAddr reflexive_addr() const { return reflexive_addr_; }

    // Optional rendezvous lookup target.  When both rendezvous and peer
    // pubkey are set, start() performs a synchronous LOOKUP at the
    // rendezvous server (3s timeout) and connects to whatever reflexive
    // endpoint comes back in the LookupResponse.  --view IP:PORT is then
    // used only as a fallback if the lookup fails.
    void set_rendezvous(const net::SocketAddr& addr) { rendezvous_addr_ = addr; }
    void set_peer_pubkey(const uint8_t pubkey[32]);
    // Lookup-by-code variant: rendezvous resolves the code to a pubkey
    // server-side and the client receives the pubkey alongside the
    // reflexive endpoint.  Mutually exclusive with set_peer_pubkey.
    void set_peer_code(const std::string& code) { peer_code_ = code; }

    // Drive the session: send hellos, receive packets, respond to pings.
    void poll();

    // Decode-feedback hooks called by the view loop after each frame is
    // submitted to the platform decoder.  Drives the client's sustainable
    // framerate estimate that's reported back to the host once per second
    // via PerfReport so the host can lower the capture/encode rate when
    // the client can't keep up (and raise it again when it can).
    void note_decoder_accepted();
    void note_decoder_rejected();

    // Pop next complete video frame. Returns false if none available.
    bool pop_frame(net::AssembledFrame& frame);

    // Send an input event to the host
    void send_input(const protocol::InputEvent& event);

    // Request IDR frame from host (e.g. after detecting frame loss)
    void request_idr();

    // Drop any buffered video frames in the receiver. Used together with
    // request_idr() after loss — nothing currently in the reassembly or
    // completion queue is safe to decode without the upcoming IDR.
    void reset_video_stream();

    // Send NACK to host requesting retransmit of specific lost fragments.
    void send_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count);

    SessionState state() const { return state_; }
    double rtt_ms() const { return rtt_ms_; }

    // HUD accessors — populated each PerfReport tick / audio stat tick.
    uint16_t perf_target_fps()  const { return perf_target_fps_; }
    float    last_reject_pct()  const { return last_reject_pct_; }
    float    last_drop_pct()    const { return last_drop_pct_; }
    uint32_t last_audio_pps()   const { return last_audio_pps_; }
    uint32_t last_plc_pct()     const { return last_plc_pct_; }
    uint32_t last_bitrate_bps() const { return last_bitrate_bps_; }
    uint16_t stream_width()     const { return stream_info_.width; }
    uint16_t stream_height()    const { return stream_info_.height; }
    // Cumulative event counts — useful in the HUD next to the
    // (small) per-second percentages, since visible artefacts are
    // typically rare events whose ratios round to 0.0%.
    uint64_t total_rejected()   const { return total_rejected_; }
    uint64_t total_dropped()    const { return total_dropped_; }

    // Codec advertised by the host in HELLO_ACK.  Defaults to HEVC for
    // legacy hosts that don't carry the codec byte.
    VideoCodec host_codec() const { return host_codec_; }
    uint64_t frames_dropped() const;
    VideoReceiver* receiver() { return receiver_.get(); }
    AudioReceiver* audio_receiver() { return audio_receiver_.get(); }

    // Start audio playback. Opens default output device and begins consuming
    // packets from the audio channel. Safe to call after start().
    bool start_audio();
    void stop_audio();

    // Pop the next freshly received cursor shape (since last call). Returns
    // false when no new shape has arrived. The view layer uploads it to a
    // GPU texture keyed by shape_id.
    bool take_new_cursor_shape(protocol::CursorShapeMessage& out);
    // Latest cursor position message from the host (updated every frame).
    const protocol::CursorPositionMessage& cursor_position() const { return cursor_pos_; }
    // True only after the host has actually sent at least one cursor
    // position packet — view layer must guard update_cursor_position
    // calls with this so it doesn't react to the default-constructed
    // (visible=false) message that would otherwise hijack the system
    // cursor on hosts that don't sync cursors at all (e.g. Linux PipeWire
    // where the cursor is baked into the captured frame).
    bool has_cursor_position() const { return cursor_pos_received_; }

    // Pop a freshly-received StreamInfo (real pre-padding frame dims).
    // Returns false if nothing new has arrived since the last call.
    bool take_new_stream_info(protocol::StreamInfoMessage& out);

private:
    void handle_packet(const uint8_t* data, size_t len);
    void handle_control(const uint8_t* payload, size_t len);
    // Seal a plaintext wire with send_cs_ and push it to the host.  Returns
    // true on success.  All post-handshake send paths funnel through this.
    bool send_sealed(const std::vector<uint8_t>& wire);
    void handle_ping(const uint8_t* payload, size_t len);
    void handle_bw_probe(const uint8_t* payload, size_t len);
    void handle_cursor_shape(const uint8_t* payload, size_t len);
    void handle_cursor_position(const uint8_t* payload, size_t len);
    void handle_stream_info(const uint8_t* payload, size_t len);
    void send_hello();
    void send_bw_probe_ack();

    void send_fec_report();
    void send_perf_report();

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<net::IUdpSocket> audio_socket_;
    std::unique_ptr<VideoReceiver> receiver_;
    std::unique_ptr<AudioReceiver> audio_receiver_;
    uint16_t audio_local_port_ = 0;
    SessionState state_ = SessionState::Disconnected;
    VideoCodec host_codec_ = VideoCodec::HEVC;
    net::SocketAddr host_addr_{};

    // Pinned host static pubkey + handshake state.  The handshake object is
    // live from start() until we process msg2, at which point finalize()
    // transfers keys into send_cs_/recv_cs_ and handshake_complete_ latches.
    uint8_t host_static_pk_[32] = {};
    bool host_key_set_ = false;
    crypto::HandshakeStateNK handshake_;
    crypto::CipherState send_cs_;
    crypto::CipherState recv_cs_;
    // Audio-socket cipher pair derived in the same finalize() call — keeps
    // the audio nonce counter independent from the main transport.  We
    // only ever use `audio_recv_cs_` today (host->client audio); the send
    // side is reserved for the future mic-direction channel.
    crypto::CipherState audio_send_cs_;
    crypto::CipherState audio_recv_cs_;
    bool handshake_complete_ = false;
    net::SocketAddr stun_server_{};
    net::SocketAddr reflexive_addr_{};
    net::SocketAddr rendezvous_addr_{};
    uint8_t         peer_pubkey_[32] = {};
    bool            peer_pubkey_set_ = false;
    std::string     peer_code_;        // alternative to peer_pubkey_; resolved at start()
    // Synchronous LOOKUP at the rendezvous.  Writes the connectable endpoint
    // into `out` on success and (for the by-code variant) the resolved
    // pubkey into `out_pk` so the caller can finish Noise_NK setup.  Blocks
    // for up to ~3s, draining any DBRV-magic packets along the way.
    bool lookup_via_rendezvous(net::SocketAddr& out, uint8_t out_pk[32]);
    TimePoint connect_start_;
    TimePoint last_hello_time_;
    TimePoint last_recv_time_;
    TimePoint last_fec_report_time_;
    double rtt_ms_ = 0.0;
    TimePoint last_audio_punch_{};
    TimePoint last_audio_stat_log_{};
    uint64_t audio_raw_packets_ = 0;
    uint64_t audio_raw_bytes_ = 0;

    // Bandwidth probe measurement state.
    uint16_t  probe_id_ = 0;
    uint16_t  probe_received_ = 0;
    uint16_t  probe_count_ = 0;
    bool      probe_ack_sent_ = false;
    TimePoint probe_first_time_;
    TimePoint probe_last_time_;

    // Latest cursor position/shape received from host. Shape is kept here
    // until the view layer picks it up via take_new_cursor_shape().
    protocol::CursorPositionMessage cursor_pos_{};
    bool                            cursor_pos_received_ = false;
    protocol::CursorShapeMessage    pending_shape_{};
    bool pending_shape_valid_ = false;
    // Last shape_id we delivered to the view layer — avoids redelivering
    // the same shape when the host re-sends it for loss protection.
    uint32_t last_delivered_shape_id_ = 0;

    // Latest StreamInfo from the host.  `new_stream_info_` goes true when
    // the dimensions change so the view layer only reacts on real changes
    // (host re-sends the same values every keyframe for loss resilience).
    protocol::StreamInfoMessage stream_info_{};
    bool new_stream_info_ = false;

    // Shape fragment reassembly buffer keyed by shape_id. Each entry holds
    // one chunk per fragment index; missing chunks remain empty until the
    // host's retry fills them in. Once `received_count == fragments.size()`
    // we concatenate and deserialize into pending_shape_.
    struct ShapeReassembly {
        std::vector<std::vector<uint8_t>> fragments;
        uint8_t received_count = 0;
    };
    std::unordered_map<uint32_t, ShapeReassembly> shape_reassembly_;

    static constexpr size_t RECV_BUF_SIZE = 2048;
    static constexpr int64_t FEC_REPORT_INTERVAL_MS = 500;
    static constexpr int64_t PERF_REPORT_INTERVAL_MS = 1000;
    // Bounds for the auto-tuned target framerate.  120 is the wire/spec
    // max; 15 is the floor below which interactivity feels broken.
    static constexpr uint16_t PERF_TARGET_FPS_MAX = 120;
    static constexpr uint16_t PERF_TARGET_FPS_MIN = 15;
    // Reject ratio thresholds over the last 1 s window.
    static constexpr float    PERF_REJECT_DOWN = 0.05f;  // > 5% → step down
    static constexpr float    PERF_REJECT_UP   = 0.01f;  // < 1% → eligible for step up
    // Step-up requires this many consecutive clean intervals.
    static constexpr int      PERF_UP_STREAK   = 3;

    // Per-interval counters reset each PerfReport tick.
    uint32_t perf_accepted_      = 0;
    uint32_t perf_rejected_      = 0;
    uint64_t perf_drops_baseline_ = 0;  // frames_dropped at last tick
    int      perf_clean_streak_  = 0;
    uint16_t perf_target_fps_    = 60;
    TimePoint last_perf_report_time_;

    // Cached snapshots for the HUD overlay — last-known rates updated
    // whenever the corresponding periodic computation runs.
    float    last_reject_pct_    = 0.0f;
    float    last_drop_pct_      = 0.0f;
    uint32_t last_audio_pps_     = 0;
    uint32_t last_plc_pct_       = 0;
    uint64_t last_audio_recv_    = 0;
    uint64_t last_audio_plc_     = 0;
    // Inbound bitrate counter (main socket only — audio socket has its
    // own raw_bytes counter we already track).  Sampled at PerfReport
    // cadence to expose `last_bitrate_bps_` to the HUD.
    uint64_t bytes_received_     = 0;
    uint64_t bytes_baseline_     = 0;
    uint32_t last_bitrate_bps_   = 0;
    // Cumulative — never reset between intervals, only grow.
    uint64_t total_rejected_     = 0;
    uint64_t total_dropped_      = 0;
    uint64_t last_fec_failed_reported_ = 0;

    // Scratch buffers reused across handle_packet() / poll() calls so we
    // don't allocate a fresh std::vector<std::vector<uint8_t>> per UDP
    // packet (tens of fragments/frame × 60fps).  Cleared on entry; the
    // outer vector keeps its capacity between calls.
    std::vector<std::vector<uint8_t>> fec_recovered_scratch_;
};

} // namespace deskbeam::client
