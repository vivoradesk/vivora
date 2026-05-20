#include "common/net/frame_fragmenter.h"
#include <algorithm>
#include <cstring>

namespace vivora::net {

std::vector<protocol::Packet> FrameFragmenter::fragment(
    const uint8_t* data, size_t data_len,
    uint16_t frame_seq_no, uint32_t timestamp,
    bool keyframe, bool heartbeat)
{
    std::vector<protocol::Packet> packets;
    const uint8_t hb_flag = heartbeat ? protocol::FLAG_HEARTBEAT : 0;

    // Small frame: no fragmentation needed
    if (data_len <= MAX_PAYLOAD) {
        protocol::Packet pkt;
        pkt.header.type = protocol::PacketType::Video;
        pkt.header.seq_no = frame_seq_no;
        pkt.header.timestamp = timestamp;
        pkt.header.flags = (keyframe ? protocol::FLAG_KEYFRAME : protocol::FLAG_NONE)
                          | hb_flag;
        pkt.header.payload_len = static_cast<uint16_t>(data_len);
        pkt.payload.assign(data, data + data_len);
        packets.push_back(std::move(pkt));
        return packets;
    }

    // Compute fragment count
    uint16_t frag_count = static_cast<uint16_t>(
        (data_len + DATA_PER_FRAGMENT - 1) / DATA_PER_FRAGMENT);
    packets.reserve(frag_count);

    size_t offset = 0;
    for (uint16_t i = 0; i < frag_count; ++i) {
        size_t chunk = std::min(DATA_PER_FRAGMENT, data_len - offset);

        protocol::Packet pkt;
        pkt.header.type = protocol::PacketType::Video;
        pkt.header.seq_no = frame_seq_no;
        pkt.header.timestamp = timestamp;

        uint8_t flags = protocol::FLAG_FRAGMENT | hb_flag;
        if (i == 0 && keyframe) flags |= protocol::FLAG_KEYFRAME;
        if (i == frag_count - 1) flags |= protocol::FLAG_LAST_FRAG;
        pkt.header.flags = flags;

        // Payload: FragIndex(2B) + FragCount(2B) + data
        pkt.payload.resize(FRAG_HEADER_SIZE + chunk);
        pkt.payload[0] = static_cast<uint8_t>(i & 0xFF);
        pkt.payload[1] = static_cast<uint8_t>(i >> 8);
        pkt.payload[2] = static_cast<uint8_t>(frag_count & 0xFF);
        pkt.payload[3] = static_cast<uint8_t>(frag_count >> 8);
        std::memcpy(pkt.payload.data() + FRAG_HEADER_SIZE, data + offset, chunk);

        pkt.header.payload_len = static_cast<uint16_t>(pkt.payload.size());
        packets.push_back(std::move(pkt));
        offset += chunk;
    }

    return packets;
}

} // namespace vivora::net
