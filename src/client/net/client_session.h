#pragma once

#include "common/codec/video_codec.h"
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
#include <unordered_map>

namespace deskbeam::client {

enum class SessionState { Disconnected, Connecting, Connected };

class ClientSession {
public:
    static constexpr int64_t HELLO_RETRY_MS = 500;
    static constexpr int64_t CONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;

    // Connect to host at given IP:port. Non-blocking — call poll() to drive.
    bool start(const char* host_ip, uint16_t port);
    void stop();

    // Drive the session: send hellos, receive packets, respond to pings.
    void poll();

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

    // Pop a freshly-received StreamInfo (real pre-padding frame dims).
    // Returns false if nothing new has arrived since the last call.
    bool take_new_stream_info(protocol::StreamInfoMessage& out);

private:
    void handle_packet(const uint8_t* data, size_t len);
    void handle_control(const uint8_t* payload, size_t len);
    void handle_ping(const uint8_t* payload, size_t len);
    void handle_bw_probe(const uint8_t* payload, size_t len);
    void handle_cursor_shape(const uint8_t* payload, size_t len);
    void handle_cursor_position(const uint8_t* payload, size_t len);
    void handle_stream_info(const uint8_t* payload, size_t len);
    void send_hello();
    void send_bw_probe_ack();

    void send_fec_report();

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<net::IUdpSocket> audio_socket_;
    std::unique_ptr<VideoReceiver> receiver_;
    std::unique_ptr<AudioReceiver> audio_receiver_;
    uint16_t audio_local_port_ = 0;
    SessionState state_ = SessionState::Disconnected;
    VideoCodec host_codec_ = VideoCodec::HEVC;
    net::SocketAddr host_addr_{};
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

    // Scratch buffers reused across handle_packet() / poll() calls so we
    // don't allocate a fresh std::vector<std::vector<uint8_t>> per UDP
    // packet (tens of fragments/frame × 60fps).  Cleared on entry; the
    // outer vector keeps its capacity between calls.
    std::vector<std::vector<uint8_t>> fec_recovered_scratch_;
};

} // namespace deskbeam::client
