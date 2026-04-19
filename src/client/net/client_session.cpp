#include "client/net/client_session.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/packet.h"
#include "common/utils/log.h"
#include <cstring>
#include <chrono>

namespace deskbeam::client {

static const uint8_t HELLO_MAGIC[] = { 'D','E','S','K','B','E','A','M', 0x01 };
static const uint8_t HELLO_ACK[]   = { 'D','E','S','K','B','E','A','M', 0x01, 0x00 };

bool ClientSession::start(const char* host_ip, uint16_t port) {
    socket_ = net::IUdpSocket::create();
    if (!socket_) return false;

    // Bind to any port
    if (!socket_->bind(0)) {
        log::error("ClientSession", "Failed to bind");
        return false;
    }

    socket_->set_nonblocking(true);
    socket_->set_recvbuf(1024 * 1024);

    host_addr_.ip = net::parse_ip(host_ip);
    host_addr_.port = port;

    if (host_addr_.ip == 0) {
        log::error("ClientSession", "Invalid host IP: %s", host_ip);
        return false;
    }

    receiver_ = std::make_unique<VideoReceiver>(*socket_);

    // Audio socket: ephemeral port. Used to receive PacketType::Audio.
    audio_socket_ = net::IUdpSocket::create();
    if (audio_socket_ && audio_socket_->bind(0)) {
        audio_socket_->set_nonblocking(true);
        audio_socket_->set_recvbuf(256 * 1024);
        audio_local_port_ = audio_socket_->local_port();
        log::info("ClientSession", "Audio socket bound to port %u", audio_local_port_);
    } else {
        log::warn("ClientSession", "Failed to bind audio socket — audio disabled");
        audio_socket_.reset();
        audio_local_port_ = 0;
    }

    state_ = SessionState::Connecting;
    connect_start_ = Clock::now();
    last_hello_time_ = {};
    last_recv_time_ = Clock::now();

    log::info("ClientSession", "Connecting to %s:%u", host_ip, port);
    send_hello();
    return true;
}

void ClientSession::stop() {
    stop_audio();
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    if (audio_socket_) {
        audio_socket_->close();
        audio_socket_.reset();
    }
    receiver_.reset();
    state_ = SessionState::Disconnected;
}

bool ClientSession::start_audio() {
    if (!audio_socket_) return false;
    if (audio_receiver_) return true;

    auto output = audio::create_default_audio_output();
    if (!output) {
        log::warn("ClientSession", "No default audio output available");
        return false;
    }
    audio_receiver_ = std::make_unique<AudioReceiver>();
    if (!audio_receiver_->start(std::move(output), /*jitter_target_ms=*/20)) {
        audio_receiver_.reset();
        return false;
    }
    log::info("ClientSession", "Audio playback started");
    return true;
}

void ClientSession::stop_audio() {
    if (audio_receiver_) {
        audio_receiver_->stop();
        audio_receiver_.reset();
    }
}

void ClientSession::poll() {
    if (!socket_) return;

    uint8_t buf[RECV_BUF_SIZE];
    net::SocketAddr sender;

    for (;;) {
        int n = socket_->recv_from(buf, sizeof(buf), sender);
        if (n <= 0) break;
        last_recv_time_ = Clock::now();
        handle_packet(buf, static_cast<size_t>(n));
    }

    // Punch firewall hole: once connected, send a tiny packet FROM the audio
    // socket TO the host's audio port (port+1). This ensures that Windows
    // firewall / NAT allows the return UDP traffic.  Sent once.
    if (state_ == SessionState::Connected && audio_socket_ && !audio_hole_punched_) {
        net::SocketAddr audio_host = host_addr_;
        audio_host.port = host_addr_.port + 1;
        uint8_t punch[1] = {0};
        audio_socket_->send_to(punch, 1, audio_host);
        audio_hole_punched_ = true;
        log::info("ClientSession", "Audio firewall punch sent to port %u",
                  audio_host.port);
    }

    // Drain audio socket: each packet is a PacketType::Audio wrapper holding
    // an Opus frame. Feed the Opus payload directly into the jitter buffer.
    if (audio_socket_ && audio_receiver_) {
        net::SocketAddr a_sender;
        for (;;) {
            int n = audio_socket_->recv_from(buf, sizeof(buf), a_sender);
            if (n <= 0) break;
            if (n < static_cast<int>(protocol::PacketHeader::WIRE_SIZE)) continue;
            auto h = protocol::PacketHeader::deserialize(buf);
            if (h.type != protocol::PacketType::Audio) continue;
            size_t plen = static_cast<size_t>(n) - protocol::PacketHeader::WIRE_SIZE;
            if (plen == 0 || plen != h.payload_len) continue;
            audio_receiver_->feed(h.seq_no,
                                  buf + protocol::PacketHeader::WIRE_SIZE,
                                  plen);
        }
    }

    auto now = Clock::now();

    if (state_ == SessionState::Connecting) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - connect_start_).count();
        if (elapsed > CONNECT_TIMEOUT_MS) {
            log::error("ClientSession", "Connection timed out");
            state_ = SessionState::Disconnected;
            return;
        }
        auto since_hello = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_hello_time_).count();
        if (since_hello > HELLO_RETRY_MS) {
            send_hello();
        }
    }

    if (state_ == SessionState::Connected) {
        auto since_recv = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_recv_time_).count();
        if (since_recv > DISCONNECT_TIMEOUT_MS) {
            log::warn("ClientSession", "Host timed out");
            state_ = SessionState::Disconnected;
            return;
        }

        if (receiver_) {
            // FEC recovery FIRST: recover lost packets before NACK fires.
            // The recv loop above already drained all buffered packets, so
            // any FEC group still missing exactly 1 packet = true loss.
            std::vector<std::vector<uint8_t>> fec_recovered;
            receiver_->fec_tick(fec_recovered);
            for (const auto& rec : fec_recovered) {
                if (rec.size() >= protocol::PacketHeader::WIRE_SIZE) {
                    auto pkt = protocol::Packet::deserialize(rec.data(), rec.size());
                    receiver_->feed(pkt);
                }
            }

            // NACK processing: retransmit only what FEC couldn't recover.
            int64_t gap_ms = 4;
            int64_t rl_ms = rtt_ms_ > 0 ? static_cast<int64_t>(rtt_ms_ * 1.5) : 8;
            if (rl_ms < 8) rl_ms = 8;
            auto batches = receiver_->collect_nacks(gap_ms, rl_ms);
            for (const auto& b : batches) {
                send_nack(b.seq_no, b.frag_indices.data(), b.frag_indices.size());
            }
        }

        // Periodic FEC loss report — host uses this to adapt FEC group size K.
        {
            auto since_report = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_fec_report_time_).count();
            if (since_report >= FEC_REPORT_INTERVAL_MS) {
                send_fec_report();
                last_fec_report_time_ = now;
            }
        }

        // BW probe flush: host sends N packets in a burst. If the tail is
        // lost we'd never hit the last-index trigger in handle_bw_probe and
        // the probe would time out. After 500ms of silence with partial
        // receipts, ACK with what we got.
        if (probe_received_ > 0 && !probe_ack_sent_) {
            auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - probe_last_time_).count();
            if (since_last > 500) {
                send_bw_probe_ack();
                probe_ack_sent_ = true;
            }
        }
    }
}

bool ClientSession::pop_frame(net::AssembledFrame& frame) {
    if (!receiver_) return false;
    return receiver_->pop_frame(frame);
}

void ClientSession::handle_packet(const uint8_t* data, size_t len) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return;

    auto header = protocol::PacketHeader::deserialize(data);
    const uint8_t* payload = data + protocol::PacketHeader::WIRE_SIZE;
    size_t payload_len = len - protocol::PacketHeader::WIRE_SIZE;

    switch (header.type) {
        case protocol::PacketType::Control:
            handle_control(payload, payload_len);
            break;
        case protocol::PacketType::Ping:
            handle_ping(payload, payload_len);
            break;
        case protocol::PacketType::BwProbe:
            handle_bw_probe(payload, payload_len);
            break;
        case protocol::PacketType::CursorShape:
            handle_cursor_shape(payload, payload_len);
            break;
        case protocol::PacketType::CursorPosition:
            handle_cursor_position(payload, payload_len);
            break;
        case protocol::PacketType::StreamInfo:
            handle_stream_info(payload, payload_len);
            break;
        case protocol::PacketType::Video: {
            // Feed raw wire bytes through FEC decoder → assembler.
            // FEC packets (FLAG_FEC) are consumed by the decoder and
            // don't reach the assembler; recovered packets are injected.
            std::vector<std::vector<uint8_t>> recovered;
            if (receiver_) {
                receiver_->fec_feed(data, len, recovered);
                for (const auto& rec : recovered) {
                    if (rec.size() >= protocol::PacketHeader::WIRE_SIZE) {
                        auto pkt = protocol::Packet::deserialize(rec.data(), rec.size());
                        receiver_->feed(pkt);
                    }
                }
                // Feed original to assembler (skip FEC parity packets)
                if (!(header.flags & protocol::FLAG_FEC)) {
                    auto packet = protocol::Packet::deserialize(data, len);
                    receiver_->feed(packet);
                }
            }
            break;
        }
        default:
            break;
    }
}

void ClientSession::handle_control(const uint8_t* payload, size_t len) {
    if (len < sizeof(HELLO_ACK)) return;
    if (std::memcmp(payload, HELLO_ACK, sizeof(HELLO_ACK)) != 0) return;

    // Newer hosts append a codec byte after the legacy ACK: 0 = H.264, 1 = HEVC.
    // Legacy hosts omit the byte — fall back to HEVC.
    if (len >= sizeof(HELLO_ACK) + 1) {
        uint8_t codec_byte = payload[sizeof(HELLO_ACK)];
        host_codec_ = (codec_byte == static_cast<uint8_t>(VideoCodec::H264))
                          ? VideoCodec::H264
                          : VideoCodec::HEVC;
    }

    if (state_ == SessionState::Connecting) {
        state_ = SessionState::Connected;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - connect_start_).count();
        log::info("ClientSession", "Connected (handshake took %lldms, host codec=%s)",
                  elapsed, host_codec_ == VideoCodec::HEVC ? "HEVC" : "H.264");
    }
}

void ClientSession::handle_cursor_shape(const uint8_t* payload, size_t len) {
    // Fragmented payload:
    //   [shape_id:4B LE][frag_idx:1B][frag_count:1B][chunk bytes...]
    // Host fragments because cursor bitmaps exceed MTU and fragmented
    // UDP is frequently dropped by WiFi gear.
    constexpr size_t FRAG_HEADER = 6;
    if (len < FRAG_HEADER) return;
    uint32_t shape_id = 0;
    std::memcpy(&shape_id, payload, 4);
    uint8_t frag_idx   = payload[4];
    uint8_t frag_count = payload[5];
    if (frag_count == 0 || frag_idx >= frag_count) return;

    // Skip fragments for shapes we already delivered — host retries for
    // loss protection and we don't want to repeatedly re-reassemble.
    if (shape_id == last_delivered_shape_id_ && !pending_shape_valid_) return;

    auto& r = shape_reassembly_[shape_id];
    if (r.fragments.size() != frag_count) {
        r.fragments.assign(frag_count, std::vector<uint8_t>{});
        r.received_count = 0;
    }
    if (!r.fragments[frag_idx].empty()) return;  // duplicate fragment
    r.fragments[frag_idx].assign(payload + FRAG_HEADER, payload + len);
    r.received_count++;

    if (r.received_count < frag_count) return;

    // All chunks arrived — concatenate and deserialize.
    std::vector<uint8_t> body;
    size_t total = 0;
    for (auto& f : r.fragments) total += f.size();
    body.reserve(total);
    for (auto& f : r.fragments) body.insert(body.end(), f.begin(), f.end());

    protocol::CursorShapeMessage msg;
    if (protocol::CursorShapeMessage::deserialize(body.data(), body.size(), msg)) {
        pending_shape_ = std::move(msg);
        pending_shape_valid_ = true;
    }
    // Drop the buffer whether or not deserialization succeeded — we
    // can't do anything more with these fragments.
    shape_reassembly_.erase(shape_id);
}

void ClientSession::handle_cursor_position(const uint8_t* payload, size_t len) {
    protocol::CursorPositionMessage msg;
    if (!protocol::CursorPositionMessage::deserialize(payload, len, msg)) return;
    cursor_pos_ = msg;
}

void ClientSession::handle_stream_info(const uint8_t* payload, size_t len) {
    protocol::StreamInfoMessage msg;
    if (!protocol::StreamInfoMessage::deserialize(payload, len, msg)) {
        log::warn("ClientSession", "StreamInfo deserialize failed (len=%zu)", len);
        return;
    }
    if (msg.width == 0 || msg.height == 0) return;
    if (msg.width  != stream_info_.width ||
        msg.height != stream_info_.height) {
        stream_info_ = msg;
        new_stream_info_ = true;
        log::info("ClientSession", "Got StreamInfo %ux%u", msg.width, msg.height);
    }
}

bool ClientSession::take_new_stream_info(protocol::StreamInfoMessage& out) {
    if (!new_stream_info_) return false;
    out = stream_info_;
    new_stream_info_ = false;
    return true;
}

bool ClientSession::take_new_cursor_shape(protocol::CursorShapeMessage& out) {
    if (!pending_shape_valid_) return false;
    out = std::move(pending_shape_);
    pending_shape_ = protocol::CursorShapeMessage{};
    pending_shape_valid_ = false;
    last_delivered_shape_id_ = out.shape_id;
    return true;
}

void ClientSession::handle_ping(const uint8_t* payload, size_t len) {
    // Ping payload carries host-measured RTT (microseconds) in bytes [4..8).
    if (len >= 8) {
        uint32_t rtt_us = static_cast<uint32_t>(payload[4])
                        | (static_cast<uint32_t>(payload[5]) << 8)
                        | (static_cast<uint32_t>(payload[6]) << 16)
                        | (static_cast<uint32_t>(payload[7]) << 24);
        rtt_ms_ = rtt_us / 1000.0;
    }
    // Reply with Pong, echoing the ping payload
    protocol::Packet pong;
    pong.header.type = protocol::PacketType::Pong;
    pong.header.seq_no = 0;
    pong.header.timestamp = 0;
    pong.header.flags = 0;
    pong.payload.assign(payload, payload + len);
    pong.header.payload_len = static_cast<uint16_t>(len);

    auto wire = pong.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::send_hello() {
    protocol::Packet hello;
    hello.header.type = protocol::PacketType::Control;
    hello.header.seq_no = 0;
    hello.header.timestamp = 0;
    hello.header.flags = 0;
    hello.payload.assign(HELLO_MAGIC, HELLO_MAGIC + sizeof(HELLO_MAGIC));
    // Extension: append our local audio port (u16 LE) so the host can target
    // audio packets at it. Zero signals "no audio".
    hello.payload.push_back(static_cast<uint8_t>(audio_local_port_ & 0xFF));
    hello.payload.push_back(static_cast<uint8_t>((audio_local_port_ >> 8) & 0xFF));
    hello.header.payload_len = static_cast<uint16_t>(hello.payload.size());

    auto wire = hello.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
    last_hello_time_ = Clock::now();
}

void ClientSession::send_input(const protocol::InputEvent& event) {
    if (state_ != SessionState::Connected || !socket_) return;

    auto payload = event.serialize();

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::Input;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload = std::move(payload);
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::send_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count) {
    if (state_ != SessionState::Connected || !socket_ || count == 0 || count > 255) return;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::NackRequest;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload.resize(3 + count * 2);
    pkt.payload[0] = static_cast<uint8_t>(seq_no & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((seq_no >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        pkt.payload[3 + i * 2]     = static_cast<uint8_t>(frag_indices[i] & 0xFF);
        pkt.payload[3 + i * 2 + 1] = static_cast<uint8_t>((frag_indices[i] >> 8) & 0xFF);
    }
    pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::request_idr() {
    if (state_ != SessionState::Connected || !socket_) return;

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::IdrRequest;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.header.payload_len = 0;

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::reset_video_stream() {
    if (receiver_) receiver_->reset_stream();
}

void ClientSession::handle_bw_probe(const uint8_t* payload, size_t len) {
    if (len < 6) return;
    uint16_t id    = payload[0] | (static_cast<uint16_t>(payload[1]) << 8);
    uint16_t index = payload[2] | (static_cast<uint16_t>(payload[3]) << 8);
    uint16_t count = payload[4] | (static_cast<uint16_t>(payload[5]) << 8);

    if (id != probe_id_) {
        probe_id_ = id;
        probe_received_ = 0;
        probe_count_ = count;
        probe_ack_sent_ = false;
        probe_first_time_ = Clock::now();
    }

    probe_received_++;
    probe_last_time_ = Clock::now();

    if (!probe_ack_sent_ && (index == count - 1 || probe_received_ == count)) {
        send_bw_probe_ack();
        probe_ack_sent_ = true;
    }
}

void ClientSession::send_bw_probe_ack() {
    if (!socket_ || probe_received_ < 2) return;

    auto span = std::chrono::duration<double>(probe_last_time_ - probe_first_time_);
    double span_sec = span.count();
    if (span_sec < 0.0001) span_sec = 0.0001;

    double bits = static_cast<double>(probe_received_) * 1200.0 * 8.0;
    uint32_t bw_bps = static_cast<uint32_t>(bits / span_sec);

    log::info("ClientSession", "BW probe: %u/%u received in %.1fms -> %u kbps",
              probe_received_, probe_count_, span_sec * 1000.0, bw_bps / 1000);

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::BwProbeAck;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    // Payload: probe_id(2B) + received(2B LE) + bw_bps(4B LE)
    pkt.payload.resize(8);
    pkt.payload[0] = static_cast<uint8_t>(probe_id_ & 0xFF);
    pkt.payload[1] = static_cast<uint8_t>((probe_id_ >> 8) & 0xFF);
    pkt.payload[2] = static_cast<uint8_t>(probe_received_ & 0xFF);
    pkt.payload[3] = static_cast<uint8_t>((probe_received_ >> 8) & 0xFF);
    pkt.payload[4] = static_cast<uint8_t>(bw_bps & 0xFF);
    pkt.payload[5] = static_cast<uint8_t>((bw_bps >> 8) & 0xFF);
    pkt.payload[6] = static_cast<uint8_t>((bw_bps >> 16) & 0xFF);
    pkt.payload[7] = static_cast<uint8_t>((bw_bps >> 24) & 0xFF);
    pkt.header.payload_len = 8;

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

void ClientSession::send_fec_report() {
    if (state_ != SessionState::Connected || !socket_ || !receiver_) return;

    float loss = receiver_->loss_rate();

    protocol::Packet pkt;
    pkt.header.type = protocol::PacketType::FecReport;
    pkt.header.seq_no = 0;
    pkt.header.timestamp = 0;
    pkt.header.flags = 0;
    pkt.payload.resize(4);
    std::memcpy(pkt.payload.data(), &loss, 4);  // float32 LE
    pkt.header.payload_len = 4;

    auto wire = pkt.serialize();
    socket_->send_to(wire.data(), wire.size(), host_addr_);
}

uint64_t ClientSession::frames_dropped() const {
    return receiver_ ? receiver_->frames_dropped() : 0;
}

} // namespace deskbeam::client
