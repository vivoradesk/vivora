#pragma once

#include "common/net/socket.h"
#include "host/session/video_sender.h"
#include "host/audio/audio_sender.h"
#include "host/input/input_injector.h"
#include "common/utils/types.h"
#include <cstdint>
#include <map>
#include <memory>
#include <functional>

namespace deskbeam::host {

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
    TimePoint probe_sent_time;
};

class HostSession {
public:
    static constexpr uint16_t DEFAULT_PORT = 9876;
    static constexpr int64_t DISCONNECT_TIMEOUT_MS = 5000;
    static constexpr int64_t PING_INTERVAL_MS = 1000;

    bool start(uint16_t port = DEFAULT_PORT);
    void stop();

    // Process incoming packets (handshake, pong). Call frequently.
    void poll();

    // Send an encoded frame to ALL connected clients.
    // Returns number of packets sent (sum), or -1 if no clients.
    int send_frame(const uint8_t* data, size_t data_len,
                   uint16_t frame_seq, uint32_t timestamp, bool keyframe);

    void set_screen_resolution(uint32_t w, uint32_t h) {
        pending_screen_w_ = w;
        pending_screen_h_ = h;
        if (input_injector_) input_injector_->set_screen_resolution(w, h);
    }

    // Aggregate state across all clients.
    SessionState state() const { return state_; }
    size_t client_count() const { return clients_.size(); }

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

    VideoSender* sender() { return sender_.get(); }
    AudioSender* audio_sender() { return audio_sender_.get(); }

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

    uint32_t pending_screen_w_ = 0;
    uint32_t pending_screen_h_ = 0;

    // Bandwidth probe constants.
    static constexpr uint8_t  BW_PROBE_COUNT = 20;
    static constexpr uint16_t BW_PROBE_SIZE  = 1200;
    static constexpr int64_t  BW_PROBE_TIMEOUT_MS = 500;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace deskbeam::host
