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
    pkt_keys_.clear();
    pkt_lens_.clear();
    parity_.clear();
}

bool FecEncoder::feed(const uint8_t* wire, size_t len,
                      uint16_t frame_seq, uint32_t timestamp,
                      std::vector<uint8_t>& fec_out) {
    uint32_t key = wire_pkt_key(wire, len);
    pkt_keys_.push_back(key);
    pkt_lens_.push_back(static_cast<uint16_t>(len));
    xor_accumulate(wire, len);
    ++count_;

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
    const size_t fec_header_size = 3 + count_ * 4 + count_ * 2;
    const size_t fec_payload_size = fec_header_size + parity_.size();

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

    // pkt_keys[K] (4B LE each)
    for (uint8_t i = 0; i < count_; ++i) {
        uint32_t key = pkt_keys_[i];
        p[0] = static_cast<uint8_t>(key);
        p[1] = static_cast<uint8_t>(key >> 8);
        p[2] = static_cast<uint8_t>(key >> 16);
        p[3] = static_cast<uint8_t>(key >> 24);
        p += 4;
    }

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
        if (payload_len < 3) return;

        uint16_t group_id = p[0] | (static_cast<uint16_t>(p[1]) << 8);
        uint8_t  k        = p[2];
        p += 3;

        if (k < 1 || k > 128) return;
        const size_t fec_hdr = 3 + k * 4 + k * 2;
        if (payload_len < fec_hdr) return;

        auto& group = groups_[group_id];
        if (!group.fec_received) {
            group.k = k;
            group.fec_received = true;
            group.packets.resize(k);

            // Read pkt_keys
            group.pkt_keys.resize(k);
            for (uint8_t i = 0; i < k; ++i) {
                group.pkt_keys[i] = p[0]
                    | (static_cast<uint32_t>(p[1]) << 8)
                    | (static_cast<uint32_t>(p[2]) << 16)
                    | (static_cast<uint32_t>(p[3]) << 24);
                p += 4;
            }

            // Read pkt_lens
            group.pkt_lens.resize(k);
            for (uint8_t i = 0; i < k; ++i) {
                group.pkt_lens[i] = p[0] | (static_cast<uint16_t>(p[1]) << 8);
                p += 2;
            }

            // Read parity
            const uint8_t* parity_start = wire + PacketHeader::WIRE_SIZE + fec_hdr;
            const size_t parity_len = payload_len - fec_hdr;
            group.parity.assign(parity_start, parity_start + parity_len);

            // Pull data packets from ring buffer into group slots
            populate_group_from_ring(group);
        }

        // Don't attempt XOR here — more data packets may follow in
        // the same recv loop.  Just check if 0 missing (all arrived).
        try_recover(group, recovered, false);
    } else if (hdr.type == PacketType::Video) {
        // ---- Data packet ----
        const bool is_retx = (hdr.flags & protocol::FLAG_RETX) != 0;

        // Strip FLAG_RETX into a clean copy so stored bytes match what
        // the sender fed into the FEC XOR parity on first transmission.
        // Without this, XOR recovery using a retx'd packet would be off
        // by one bit in byte 7.
        std::vector<uint8_t> clean_wire(wire, wire + len);
        if (is_retx && clean_wire.size() > 7) {
            clean_wire[7] &= ~protocol::FLAG_RETX;
        }

        uint32_t key = wire_pkt_key(clean_wire.data(), clean_wire.size());

        // Store in ring buffer with FIFO eviction
        if (ring_.find(key) == ring_.end()) {
            ring_fifo_.push_back(key);
        }
        ring_[key] = clean_wire;

        while (ring_.size() > MAX_RING) {
            uint32_t oldest = ring_fifo_.front();
            ring_fifo_.pop_front();
            ring_.erase(oldest);
        }

        // Check if this packet belongs to an already-known group
        for (auto& [gid, group] : groups_) {
            if (group.resolved || !group.fec_received) continue;
            for (uint8_t i = 0; i < group.k; ++i) {
                if (group.pkt_keys[i] == key && group.packets[i].empty()) {
                    group.packets[i] = clean_wire;
                    ++group.received_data;
                    // Only count original transmissions toward "fresh"
                    // reception — retx packets are filling losses we
                    // already suffered, so the channel was lossy.
                    if (!is_retx) ++group.fresh_received;
                    // Check if group is now complete (no XOR needed)
                    try_recover(group, recovered, false);
                    break;
                }
            }
        }
    }

    expire_old_groups();
}

void FecDecoder::tick(std::vector<std::vector<uint8_t>>& recovered) {
    // Called after the recv loop has drained all buffered packets.
    // Any group still missing exactly 1 packet → truly lost, recover via XOR.
    for (auto& [gid, group] : groups_) {
        if (group.resolved || !group.fec_received) continue;
        try_recover(group, recovered, true);
    }
}

void FecDecoder::populate_group_from_ring(FecGroup& group) {
    // Ring contents at FEC-arrival time are all original-transmission
    // packets (retx responses to NACK require the client to first notice
    // a gap, which takes at least `collect_nacks` gap_ms + RTT — longer
    // than the sender's FEC parity flight time).  So anything pulled
    // from the ring here counts as "fresh" reception.
    for (uint8_t i = 0; i < group.k; ++i) {
        uint32_t key = group.pkt_keys[i];
        auto it = ring_.find(key);
        if (it != ring_.end() && group.packets[i].empty()) {
            group.packets[i] = it->second;
            ++group.received_data;
            ++group.fresh_received;
        }
    }
}

void FecDecoder::try_recover(FecGroup& group,
                             std::vector<std::vector<uint8_t>>& recovered,
                             bool attempt_xor) {
    if (group.resolved) return;
    if (!group.fec_received) return;

    int missing_count = 0;
    int missing_idx = -1;
    for (uint8_t i = 0; i < group.k; ++i) {
        if (group.packets[i].empty()) {
            ++missing_count;
            missing_idx = i;
        }
    }

    // Channel loss = packets that did NOT arrive as original transmission.
    // This is what drives adaptive K and bitrate.  Retransmits filling
    // slots don't reduce this number — otherwise NACK would mask real
    // channel degradation from the controller.
    const int fresh_missing = group.k - group.fresh_received;

    if (missing_count == 0) {
        group.resolved = true;
        update_loss(fresh_missing, group.k);
        return;
    }

    // During recv loop (attempt_xor=false): don't recover yet — more
    // packets may arrive in the same loop iteration.
    if (!attempt_xor) return;

    update_loss(fresh_missing, group.k);

    // After recv loop drained all buffered packets (attempt_xor=true):
    // any remaining gap is a true loss.
    if (missing_count == 1) {
        // XOR parity with all (K-1) received packets to recover the missing one.
        std::vector<uint8_t> result = group.parity;

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
                  static_cast<int>(group.pkt_keys[0] >> 16),
                  static_cast<int>(group.k));

        recovered.push_back(std::move(result));
        group.resolved = true;
        return;
    }

    // 2+ missing — can't recover with XOR alone.
    group.resolved = true;
}

void FecDecoder::update_loss(int missing, int k) {
    float group_loss = static_cast<float>(missing) / k;
    ewma_loss_ = EWMA_ALPHA * group_loss + (1.0f - EWMA_ALPHA) * ewma_loss_;
}

void FecDecoder::expire_old_groups() {
    if (groups_.size() <= MAX_GROUPS) return;

    while (groups_.size() > MAX_GROUPS) {
        auto best = groups_.end();
        for (auto it = groups_.begin(); it != groups_.end(); ++it) {
            if (it->second.resolved) {
                if (best == groups_.end() || !best->second.resolved ||
                    it->first < best->first) {
                    best = it;
                }
            }
        }
        if (best == groups_.end()) {
            best = groups_.begin();
            for (auto it = groups_.begin(); it != groups_.end(); ++it) {
                if (it->first < best->first) best = it;
            }
        }
        if (best != groups_.end())
            groups_.erase(best);
        else
            break;
    }
}

} // namespace deskbeam::net
