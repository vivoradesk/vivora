#include "host/session/video_sender.h"
#include "common/utils/log.h"

namespace deskbeam::host {

int VideoSender::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp,
                            bool keyframe, const net::SocketAddr& dest)
{
    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp, keyframe);

    int sent = 0;
    uint16_t frag_idx = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();
        int r = socket_.send_to(wire.data(), wire.size(), dest);
        if (r < 0) {
            log::error("VideoSender", "send_to failed at packet %d/%zu", sent, packets.size());
            return -1;
        }
        bytes_sent_ += r;
        sent++;

        // Store in retransmit buffer (keyed by frame seq + fragment index)
        uint32_t key = retx_key(frame_seq, frag_idx);
        retx_index_[key] = std::move(wire);
        retx_fifo_.push_back(key);
        while (retx_fifo_.size() > RETX_BUFFER_CAPACITY) {
            retx_index_.erase(retx_fifo_.front());
            retx_fifo_.pop_front();
        }
        frag_idx++;
    }

    packets_sent_ += sent;
    return sent;
}

int VideoSender::handle_nack(uint16_t seq_no, const uint16_t* frag_indices, size_t count,
                             const net::SocketAddr& dest)
{
    int resent = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t key = retx_key(seq_no, frag_indices[i]);
        auto it = retx_index_.find(key);
        if (it == retx_index_.end()) continue; // aged out or never existed

        int r = socket_.send_to(it->second.data(), it->second.size(), dest);
        if (r < 0) continue;
        bytes_sent_ += r;
        packets_sent_++;
        retransmits_++;
        resent++;
    }
    return resent;
}

} // namespace deskbeam::host
