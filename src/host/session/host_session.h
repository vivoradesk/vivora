#pragma once

#include "common/net/socket.h"
#include "host/session/video_sender.h"
#include "host/input/input_injector.h"
#include "common/utils/types.h"
#include <cstdint>
#include <memory>
#include <functional>

namespace deskbeam::host {

enum class SessionState { WaitingForClient, Connected, Disconnected };

class HostSession {
public:
    static constexpr uint16_t DEFAULT_PORT = 9876;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t PING_INTERVAL_MS = 1000;

    bool start(uint16_t port = DEFAULT_PORT);
    void stop();

    // Process incoming packets (handshake, pong). Call frequently.
    void poll();

    // Send an encoded frame to the connected client.
    // Returns number of packets sent, or -1 if not connected / error.
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp, bool keyframe);

    void set_screen_resolution(uint32_t w, uint32_t h) { input_injector_.set_screen_resolution(w, h); }
    SessionState state() const { return state_; }
    bool idr_needed() const { return idr_needed_; }
    void clear_idr_needed() { idr_needed_ = false; }
    double rtt_ms() const { return rtt_ms_; }
    const net::SocketAddr& client_addr() const { return client_addr_; }
    VideoSender* sender() { return sender_.get(); }

private:
    void handle_packet(const uint8_t* data, size_t len, const net::SocketAddr& sender);
    void handle_hello(const uint8_t* payload, size_t len, const net::SocketAddr& sender);
    void handle_pong(const uint8_t* payload, size_t len);
    void handle_input(const uint8_t* payload, size_t len);
    void send_ping();

    std::unique_ptr<net::IUdpSocket> socket_;
    std::unique_ptr<VideoSender> sender_;
    InputInjector input_injector_;
    SessionState state_ = SessionState::WaitingForClient;
    net::SocketAddr client_addr_{};
    TimePoint last_recv_time_;
    TimePoint last_ping_time_;
    uint32_t ping_seq_ = 0;
    TimePoint ping_sent_time_;
    double rtt_ms_ = 0.0;
    bool idr_needed_ = false;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace deskbeam::host
