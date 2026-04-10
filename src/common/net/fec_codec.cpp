#include "common/net/fec_codec.h"
#include "common/utils/log.h"
#include <algorithm>
#include <cstring>

namespace deskbeam::net {

using protocol::PacketHeader;
using protocol::PacketType;
using protocol::FLAG_FEC;

// ============================================================================
// FecEncoder
// ============================================================================

void FecEncoder::set_group_size(uint8_t k) {
    if (k < 2) k = 2;
    if (k > 128) k = 128;
    k_ = k;
}

void FecEncoder::xor_accumulate(const uint8_t* data, size_t len) {
    if (len > parity_.size())
        parity_.resize(len, 0);
    for (size_t i = 0; i < len; ++i)
        parity_[i] ^= data[i];
}

void FecEncoder::reset_group() {
    count_ = 0;
    pkt_lens_.clear();
    parity_.clear();
    first_idx_in_group_ = global_idx_;
}

bool FecEncoder::feed(const uint8_t* wire, size_t len,
                      uint16_t frame_seq, uint32_t timestamp,
                      std::vector<uint8_t>& fec_out) {
    if (count_ == 0)
        first_idx_in_group_ = global_idx_;

    pkt_lens_.push_back(static_cast<uint16_t>(len));
    xor_accumulate(wire, len);
    ++count_;
    ++global_idx_;

    if (count_ >= k_) {
        fec_out = build_fec_wire(frame_seq, timestamp);
        ++group_id_;
        reset_group();
        return true;
    }
    return false;
}

bool FecEncoder::flush(uint16_t frame_seq, uint32_t timestamp,
                       std::vector<uint8_t>& fec_out) {
    if (count_ == 0) return false;
    fec_out = build_fec_wire(frame_seq, timestamp);
    ++group_id_;
    reset_group();
    return true;
}

std::vector<uint8_t> FecEncoder::build_fec_wire(uint16_t frame_seq,
                                                 uint32_t timestamp) {
    // FEC payload:
    //   group_id(2) + K(1) + first_idx(2) + pkt_lens[K](2 each) + parity(N)
    const size_t fec_header_size = 5 + count_ * 2;
    const size_t fec_payload_size = fec_header_size + parity_.size();

    // Build wire = PacketHeader(10) + FEC payload
    std::vector<uint8_t> wire(PacketHeader::WIRE_SIZE + fec_payload_size);

    PacketHeader hdr;
    hdr.type = PacketType::Video;
    hdr.seq_no = frame_seq;
    hdr.timestamp = timestamp;
    hdr.flags = FLAG_FEC;
    hdr.payload_len = static_cast<uint16_t>(fec_payload_size);
    hdr.serialize(wire.data());

    uint8_t* p = wire.data() + PacketHeader::WIRE_SIZE;

    // group_id (2B LE)
    p[0] = static_cast<uint8_t>(group_id_ & 0xFF);
    p[1] = static_cast<uint8_t>(group_id_ >> 8);
    p += 2;

    // K (1B)
    *p++ = count_;

    // first_idx (2B LE)
    p[0] = static_cast<uint8_t>(first_idx_in_group_ & 0xFF);
    p[1] = static_cast<uint8_t>(first_idx_in_group_ >> 8);
    p += 2;

    // pkt_lens[K] (2B LE each)
    for (uint8_t i = 0; i < count_; ++i) {
        uint16_t l = pkt_lens_[i];
        p[0] = static_cast<uint8_t>(l & 0xFF);
        p[1] = static_cast<uint8_t>(l >> 8);
        p += 2;
    }

    // parity
    std::memcpy(p, parity_.data(), parity_.size());

    return wire;
}

// ============================================================================
// FecDecoder
// ============================================================================

void FecDecoder::feed(const uint8_t* wire, size_t len,
                      std::vector<std::vector<uint8_t>>& recovered) {
    if (len < PacketHeader::WIRE_SIZE) return;

    PacketHeader hdr = PacketHeader::deserialize(wire);

    if (hdr.flags & FLAG_FEC) {
        // ---- FEC parity packet ----
        const uint8_t* p = wire + PacketHeader::WIRE_SIZE;
        const size_t payload_len = len - PacketHeader::WIRE_SIZE;
        if (payload_len < 5) return;

        uint16_t group_id   = p[0] | (static_cast<uint16_t>(p[1]) << 8);
        uint8_t  k          = p[2];
        uint16_t first_idx  = p[3] | (static_cast<uint16_t>(p[4]) << 8);

        if (k < 1 || k > 128) return;
        const size_t fec_hdr = 5 + k * 2;
        if (payload_len < fec_hdr) return;

        auto& group = groups_[group_id];
        if (!group.fec_received) {
            group.k = k;
            group.first_idx = first_idx;
            group.fec_received = true;
            group.packets.resize(k);

            // Read pkt_lens
            group.pkt_lens.resize(k);
            const uint8_t* lp = p + 5;
            for (uint8_t i = 0; i < k; ++i) {
                group.pkt_lens[i] = lp[0] | (static_cast<uint16_t>(lp[1]) << 8);
                lp += 2;
            }

            // Read parity
            const size_t parity_len = payload_len - fec_hdr;
            group.parity.assign(p + fec_hdr, p + fec_hdr + parity_len);

            // Pull data packets from ring buffer into group slots
            populate_group_from_ring(group);
        }

        try_recover(group, recovered);
    } else if (hdr.type == PacketType::Video) {
        // ---- Data packet ----
        uint16_t idx = global_idx_++;

        // Store in ring buffer so we can retrieve it when FEC arrives.
        size_t slot = idx % RING_SIZE;
        ring_[slot].idx = idx;
        ring_[slot].wire.assign(wire, wire + len);
        ring_[slot].valid = true;

        // If this packet belongs to an already-known group (rare: FEC arrived
        // before this late data packet, e.g. via NACK retransmit), slot it in.
        for (auto& [gid, group] : groups_) {
            if (group.resolved || !group.fec_received) continue;
            uint16_t offset = static_cast<uint16_t>(idx - group.first_idx);
            if (offset < group.k && group.packets[offset].empty()) {
                group.packets[offset].assign(wire, wire + len);
                ++group.received_data;
                try_recover(group, recovered);
                break;
            }
        }
    }

    expire_old_groups();
}

void FecDecoder::populate_group_from_ring(FecGroup& group) {
    for (uint8_t i = 0; i < group.k; ++i) {
        uint16_t target_idx = static_cast<uint16_t>(group.first_idx + i);
        size_t slot = target_idx % RING_SIZE;
        auto& entry = ring_[slot];
        if (entry.valid && entry.idx == target_idx && group.packets[i].empty()) {
            group.packets[i] = entry.wire;  // copy
            ++group.received_data;
        }
    }
}

void FecDecoder::try_recover(FecGroup& group,
                             std::vector<std::vector<uint8_t>>& recovered) {
    if (group.resolved) return;
    if (!group.fec_received) return;

    // Count how many data packets we have
    int missing_count = 0;
    int missing_idx = -1;
    for (uint8_t i = 0; i < group.k; ++i) {
        if (group.packets[i].empty()) {
            ++missing_count;
            missing_idx = i;
        }
    }

    if (missing_count == 0) {
        // All received, no recovery needed.
        group.resolved = true;
        update_loss(group);
        return;
    }

    if (missing_count == 1) {
        // Can recover! XOR parity with all (K-1) received packets.
        std::vector<uint8_t> result = group.parity;  // start with parity

        for (uint8_t i = 0; i < group.k; ++i) {
            if (i == missing_idx) continue;
            const auto& pkt = group.packets[i];
            if (result.size() < pkt.size())
                result.resize(pkt.size(), 0);
            for (size_t b = 0; b < pkt.size(); ++b)
                result[b] ^= pkt[b];
        }

        // Truncate to original wire length
        uint16_t orig_len = group.pkt_lens[missing_idx];
        if (result.size() > orig_len)
            result.resize(orig_len);

        log::info("FEC", "Recovered pkt %d in group %d (K=%d)",
                  missing_idx,
                  static_cast<int>(group.first_idx),
                  static_cast<int>(group.k));

        recovered.push_back(std::move(result));
        group.resolved = true;
        update_loss(group);
        return;
    }

    // 2+ missing — can't recover with XOR alone. Don't mark resolved yet;
    // more data packets may still arrive (via NACK retransmit).
}

void FecDecoder::update_loss(const FecGroup& group) {
    float group_loss = 0.0f;
    for (uint8_t i = 0; i < group.k; ++i) {
        if (group.packets[i].empty())
            group_loss += 1.0f;
    }
    group_loss /= group.k;
    ewma_loss_ = EWMA_ALPHA * group_loss + (1.0f - EWMA_ALPHA) * ewma_loss_;
}

void FecDecoder::expire_old_groups() {
    if (groups_.size() <= MAX_GROUPS) return;

    // Remove oldest resolved groups first, then oldest unresolved.
    // Simple strategy: find the group with the lowest group_id that's resolved.
    while (groups_.size() > MAX_GROUPS) {
        auto oldest = groups_.end();
        uint16_t oldest_id = 0xFFFF;
        for (auto it = groups_.begin(); it != groups_.end(); ++it) {
            // Prefer removing resolved groups
            if (it->second.resolved &&
                (oldest == groups_.end() || it->first < oldest_id ||
                 !oldest->second.resolved)) {
                oldest = it;
                oldest_id = it->first;
            }
        }
        // If no resolved found, remove any oldest
        if (oldest == groups_.end() || !oldest->second.resolved) {
            for (auto it = groups_.begin(); it != groups_.end(); ++it) {
                if (oldest == groups_.end() || it->first < oldest_id) {
                    oldest = it;
                    oldest_id = it->first;
                }
            }
        }
        if (oldest != groups_.end())
            groups_.erase(oldest);
        else
            break;
    }
}

} // namespace deskbeam::net
