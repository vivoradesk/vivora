#pragma once

#include "common/codec/video_codec.h"
#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"
#include "client/net/video_receiver.h"
#include "client/audio/audio_receiver.h"
#include "common/audio/audio_output.h"
#include "common/protocol/clipboard_message.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"
#include "common/protocol/monitor_info.h"
#include "common/protocol/stream_info.h"
#include "common/utils/types.h"
#include "common/utils/spsc_ring.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace vivora::client {

enum class SessionState { Disconnected, Connecting, Connected };

// VIV-23: why start() stopped for a TOFU trust decision.  Filled by the
// session when interactive-trust mode is on and the persisted pin either
// doesn't exist yet (first connect → mismatch=false) or disagrees with the
// pubkey the rendezvous just returned (possible MITM → mismatch=true).
// The GUI shows the trust dialog, pins on consent (crypto::pin_peer) and
// simply re-dials — the retry then passes the pin check.
struct TrustPending {
    bool        mismatch = false;  // false = unknown peer (first connect)
    std::string code;              // memorable peer code being dialled
    std::string pubkey_hex;        // pubkey the rendezvous returned (64 hex)
    std::string stored_hex;        // previously pinned pubkey (mismatch only)
};

class ClientSession {
public:
    static constexpr int64_t HELLO_RETRY_MS = 500;
    static constexpr int64_t CONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;
    // After this much HELLO-without-ACK time, if a relay was configured
    // but isn't yet active, BIND to the relay and continue HELLO retries
    // through the relay path.  Leaves ~2 s of CONNECT_TIMEOUT_MS budget
    // for the relay attempt before we give up entirely.
    static constexpr int64_t RELAY_FALLBACK_MS = 3000;

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

    // Optional explicit relay endpoint + 32-byte session id agreed with
    // the host out of band.  When set, start() BINDs to the relay before
    // the first HELLO and all subsequent host-bound traffic is wrapped
    // in DBRL DATA frames; incoming DBRL DATA from the relay is unwrapped
    // and dispatched as if it came directly from the host.  Manual flag
    // for now (testing); a future commit will trigger this automatically
    // when direct hole-punching times out.
    void set_relay(const net::SocketAddr& addr, const uint8_t session_id[32]);
    // Endpoint-only: configure relay, but leave session_id to be filled
    // from the rendezvous LookupResponse.  Used when --relay is passed
    // without --relay-session.
    void set_relay_endpoint(const net::SocketAddr& addr) { relay_addr_ = addr; }
    // Optional: provide a 95-byte license token to attach to the relay
    // BIND.  Only required when the configured relay enforces
    // --require-license (Pro-managed).  Self-host instances ignore it.
    void set_relay_license(const uint8_t token[95]);
    // Lookup-by-code variant: rendezvous resolves the code to a pubkey
    // server-side and the client receives the pubkey alongside the
    // reflexive endpoint.  Mutually exclusive with set_peer_pubkey.
    void set_peer_code(const std::string& code) { peer_code_ = code; }

    // VIV-23 interactive TOFU.  When on, start() does NOT auto-pin an
    // unknown peer and does NOT hard-refuse a pin mismatch: in both cases
    // it records a TrustPending and returns false so the (GUI) caller can
    // ask the user and retry.  Off (default) keeps the CLI behaviour:
    // silent pin on first contact, log + refuse on mismatch.
    void set_interactive_trust(bool on) { interactive_trust_ = on; }
    bool has_trust_pending() const { return trust_pending_active_; }
    const TrustPending& trust_pending() const { return trust_pending_; }

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
    // Wire path for the in-stream menu header (VIV-74).
    const char* transport_label() const { return relay_active_ ? "Relay" : "P2P"; }

    // HUD accessors — populated each PerfReport tick / audio stat tick.
    uint16_t perf_target_fps()  const { return perf_target_fps_; }
    float    last_reject_pct()  const { return last_reject_pct_; }
    float    last_drop_pct()    const { return last_drop_pct_; }
    uint32_t last_audio_pps()   const { return last_audio_pps_; }
    uint32_t last_plc_pct()     const { return last_plc_pct_; }
    uint32_t last_bitrate_bps() const { return last_bitrate_bps_; }
    uint32_t encoding_kbps()    const { return encoding_kbps_; }
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

    // In-stream menu audio controls (VIV-74).  Values are cached on the
    // session so they survive an audio (re)start; when a receiver is live
    // the change is applied to it immediately.  Volume is a linear gain in
    // [0,1].
    void  set_audio_volume(float v);
    void  set_audio_muted(bool m);
    float audio_volume() const { return audio_volume_; }
    bool  audio_muted()  const { return audio_muted_; }

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

    // VIV-50 monitor selection.  request_monitor_list() asks the host to
    // enumerate its capturable displays (answered asynchronously — poll
    // take_new_monitor_list()).  select_monitor() switches the host's capture
    // to the given display index.
    void request_monitor_list();
    void select_monitor(uint8_t index);
    // Pop a freshly-received display list (since last call).  Returns false if
    // nothing new arrived.
    bool take_new_monitor_list(std::vector<protocol::MonitorDesc>& out);

    // VIV-22 clipboard sync.  send_clipboard() fragments the message and
    // sends it to the host; the prepared wires are re-sent once ~150ms
    // later from poll() for UDP-loss resilience (host dedups by clip_id).
    void send_clipboard(const protocol::ClipboardMessage& msg);
    // Pop the latest fully-reassembled clipboard from the host since the
    // last call (latest wins).  Returns false if none arrived.
    bool take_new_clipboard(protocol::ClipboardMessage& out);

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
    void handle_monitor_list(const uint8_t* payload, size_t len);
    void send_hello();
    void send_bw_probe_ack();

    void send_fec_report();
    void send_perf_report();

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<net::IUdpSocket> audio_socket_;
    std::unique_ptr<VideoReceiver> receiver_;
    std::unique_ptr<AudioReceiver> audio_receiver_;
    uint16_t audio_local_port_ = 0;
    // Cached in-stream-menu audio settings (VIV-74); applied to the receiver
    // on start_audio() and on every live change.
    float audio_volume_ = 1.0f;
    bool  audio_muted_  = false;
    SessionState state_ = SessionState::Disconnected;
    VideoCodec host_codec_ = VideoCodec::HEVC;
    net::SocketAddr host_addr_{};

    // Pinned host static pubkey + handshake state.  The handshake object is
    // live from start() until we process msg2, at which point finalize()
    // transfers keys into send_cs_/recv_cs_ and handshake_complete_ latches.
    uint8_t host_static_pk_[32] = {};
    bool host_key_set_ = false;
    // Our own long-term identity — the same device keypair the host side
    // uses.  IK sends this (encrypted) in msg1 so the host can recognise the
    // viewer in its approval prompt (VIV-61).  Loaded lazily on first use.
    crypto::KeyPair client_identity_{};
    bool client_identity_loaded_ = false;
    bool ensure_client_identity();
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
    // VIV-23 interactive TOFU state (see set_interactive_trust above).
    bool            interactive_trust_    = false;
    bool            trust_pending_active_ = false;
    TrustPending    trust_pending_{};
    // LAN candidates advertised by the host at registration, retrieved in
    // the LookupResponse and used by start() for same-NAT short-circuit.
    static constexpr size_t MAX_LAN_CANDIDATES = 4;
    net::SocketAddr lookup_lan_[MAX_LAN_CANDIDATES]{};
    uint8_t         lookup_lan_count_ = 0;

    // Relay: target endpoint, agreed-on session id, and runtime state.
    // When relay_active_ is true, every host-bound packet is wrapped in
    // DBRL DATA before going on the wire; conversely, DBRL DATA arriving
    // from relay_addr_ is unwrapped and re-fed through handle_packet as
    // if from the host.
    net::SocketAddr relay_addr_{};
    uint8_t         relay_session_id_[32] = {};
    bool            relay_session_set_    = false;
    uint8_t         relay_alloc_id_[8]    = {};
    bool            relay_active_         = false;
    TimePoint       last_relay_keepalive_{};
    uint8_t         relay_license_[95]    = {};
    bool            relay_license_set_    = false;
    bool relay_bind_blocking();          // synchronous BIND + ACK during start()
    bool transport_send(const uint8_t* data, size_t len);   // wrap or direct
    void relay_send_keepalive();
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

    // Latest display list from the host (VIV-50).  `new_monitor_list_` latches
    // on arrival so the view layer pushes it into the monitor panel once.
    std::vector<protocol::MonitorDesc> monitor_list_;
    bool new_monitor_list_ = false;

    // VIV-22 clipboard state.  Inbound: reassembler + latest complete
    // message.  Outbound: prepared plaintext wires kept for one delayed
    // re-send from poll() (sealed fresh on each send).
    protocol::ClipboardReassembler clipboard_rx_;
    protocol::ClipboardMessage     pending_clipboard_{};
    bool                           pending_clipboard_valid_ = false;
    uint32_t                       clip_tx_id_ = 0;
    std::vector<std::vector<uint8_t>> clip_tx_wires_;
    bool      clip_resend_pending_ = false;
    TimePoint clip_last_send_{};
    static constexpr int64_t CLIPBOARD_RESEND_MS = 150;

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
    // 150ms (was 500): a FEC group failure detected on the client only reached
    // the host at the next report boundary, so the host raised parity in
    // reaction to the PREVIOUS burst while the next one hit at the old M.  Faster
    // reporting lets adaptive FEC track bursts closer to real time (VIV-82).
    static constexpr int64_t FEC_REPORT_INTERVAL_MS = 150;
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
    uint32_t encoding_kbps_      = 0;  // host's encoder target (HostStats packet)
    // Cumulative — never reset between intervals, only grow.
    uint64_t total_rejected_     = 0;
    uint64_t total_dropped_      = 0;
    uint64_t last_fec_failed_reported_ = 0;

    // Scratch buffers reused across handle_packet() / poll() calls so we
    // don't allocate a fresh std::vector<std::vector<uint8_t>> per UDP
    // packet (tens of fragments/frame × 60fps).  Cleared on entry; the
    // outer vector keeps its capacity between calls.
    std::vector<std::vector<uint8_t>> fec_recovered_scratch_;

    // Async socket-receive thread (VIV-81; VIVORA_PIPELINE=threaded).  A
    // dedicated thread drains the video socket into recv_ring_ continuously
    // so the kernel UDP buffer never overflows under burst regardless of
    // net.core.rmem_max — the proven root cause of the recvbuf-loss → IDR
    // churn we hit.  poll() consumes the ring instead of recv_from() when
    // async_recv_ is on; decrypt/dispatch/relay-unwrap are unchanged (still
    // run on the poll() thread).  Off by default (legacy direct recv).
    struct RawPacket {
        int             len = 0;
        net::SocketAddr from{};
        uint8_t         data[RECV_BUF_SIZE];
    };
    bool                  async_recv_ = false;
    std::atomic<bool>     recv_running_{false};
    std::thread           recv_thread_;
    // ~2 MB; heap-allocated only in threaded mode (avoids bloating the
    // by-value ClientSession on the CLI's stack).
    std::unique_ptr<util::SpscRing<RawPacket, 8192>> recv_ring_;
    void recv_thread_proc();
};

} // namespace vivora::client
