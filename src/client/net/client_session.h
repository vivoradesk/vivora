#pragma once

#include "common/net/socket.h"
#include "client/net/video_receiver.h"
#include "client/audio/audio_receiver.h"
#include "common/audio/audio_output.h"
#include "common/protocol/input_event.h"
#include "common/utils/types.h"
#include <cstdint>
#include <memory>

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

    // Send NACK to host requesting retransmit of specific lost fragments.
    void send_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count);

    SessionState state() const { return state_; }
    double rtt_ms() const { return rtt_ms_; }
    uint64_t frames_dropped() const;
    VideoReceiver* receiver() { return receiver_.get(); }
    AudioReceiver* audio_receiver() { return audio_receiver_.get(); }

    // Start audio playback. Opens default output device and begins consuming
    // packets from the audio channel. Safe to call after start().
    bool start_audio();
    void stop_audio();

private:
    void handle_packet(const uint8_t* data, size_t len);
    void handle_control(const uint8_t* payload, size_t len);
    void handle_ping(const uint8_t* payload, size_t len);
    void handle_bw_probe(const uint8_t* payload, size_t len);
    void send_hello();
    void send_bw_probe_ack();

    void send_fec_report();

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<net::IUdpSocket> audio_socket_;
    std::unique_ptr<VideoReceiver> receiver_;
    std::unique_ptr<AudioReceiver> audio_receiver_;
    uint16_t audio_local_port_ = 0;
    SessionState state_ = SessionState::Disconnected;
    net::SocketAddr host_addr_{};
    TimePoint connect_start_;
    TimePoint last_hello_time_;
    TimePoint last_recv_time_;
    TimePoint last_fec_report_time_;
    double rtt_ms_ = 0.0;
    bool audio_hole_punched_ = false;

    // Bandwidth probe measurement state.
    uint16_t  probe_id_ = 0;
    uint8_t   probe_received_ = 0;
    uint8_t   probe_count_ = 0;
    TimePoint probe_first_time_;
    TimePoint probe_last_time_;

    static constexpr size_t RECV_BUF_SIZE = 2048;
    static constexpr int64_t FEC_REPORT_INTERVAL_MS = 500;
};

} // namespace deskbeam::client
