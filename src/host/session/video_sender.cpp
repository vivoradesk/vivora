#include "host/session/video_sender.h"
#include "common/utils/log.h"

namespace deskbeam::host {

int VideoSender::send_frame(const uint8_t* data, size_t data_len,
                            uint16_t frame_seq, uint32_t timestamp,
                            bool keyframe, const net::SocketAddr& dest)
{
    auto packets = fragmenter_.fragment(data, data_len, frame_seq, timestamp, keyframe);

    int sent = 0;
    for (auto& pkt : packets) {
        auto wire = pkt.serialize();
        int r = socket_.send_to(wire.data(), wire.size(), dest);
        if (r < 0) {
            log::error("VideoSender", "send_to failed at packet %d/%zu", sent, packets.size());
            return -1;
        }
        bytes_sent_ += r;
        sent++;
    }

    packets_sent_ += sent;
    return sent;
}

} // namespace deskbeam::host
