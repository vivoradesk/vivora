#pragma once

#include "common/protocol/packet.h"
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>

namespace deskbeam::net {

// Extract a unique 32-bit key from a data packet's wire bytes:
// (seq_no << 16) | frag_index. Works for both fragmented and non-fragmented.
inline uint32_t wire_pkt_key(const uint8_t* wire, size_t len) {
    if (len < protocol::PacketHeader::WIRE_SIZE) return 0;
    uint16_t seq_no = wire[1] | (static_cast<uint16_t>(wire[2]) << 8);
    uint8_t flags = wire[7];
    uint16_t frag_idx = 0;
    if ((flags & protocol::FLAG_FRAGMENT) &&
        len > protocol::PacketHeader::WIRE_SIZE + 2) {
        const uint8_t* p = wire + protocol::PacketHeader::WIRE_SIZE;
        frag_idx = p[0] | (static_cast<uint16_t>(p[1]) << 8);
    }
    return (static_cast<uint32_t>(seq_no) << 16) | frag_idx;
}

// ---- Encoder (host side) --------------------------------------------------

class FecEncoder {
public:
    void set_group_size(uint8_t k);
    uint8_t group_size() const { return k_; }

    bool feed(const uint8_t* wire, size_t len,
              uint16_t frame_seq, uint32_t timestamp,
              std::vector<uint8_t>& fec_out);

    bool flush(uint16_t frame_seq, uint32_t timestamp,
               std::vector<uint8_t>& fec_out);

private:
    void xor_accumulate(const uint8_t* data, size_t len);
    std::vector<uint8_t> build_fec_wire(uint16_t frame_seq, uint32_t timestamp);
    void reset_group();

    uint8_t k_ = 10;
    uint16_t group_id_ = 0;

    std::vector<uint32_t> pkt_keys_;
    std::vector<uint16_t> pkt_lens_;
    std::vector<uint8_t>  parity_;
    uint8_t               count_ = 0;
};

// ---- Decoder (client side) ------------------------------------------------

class FecDecoder {
public:
    // Feed a raw wire packet (data or FEC).
    // Any recovered packets are appended to `recovered` as raw wire bytes.
    void feed(const uint8_t* wire, size_t len,
              std::vector<std::vector<uint8_t>>& recovered);

    // Must be called periodically (e.g. every poll cycle) to trigger
    // deferred recoveries after the reordering grace period expires.
    void tick(std::vector<std::vector<uint8_t>>& recovered);

    float loss_rate() const { return ewma_loss_; }

private:
    struct FecGroup {
        uint8_t  k = 0;
        std::vector<uint32_t>             pkt_keys;
        std::vector<uint16_t>             pkt_lens;
        std::vector<std::vector<uint8_t>> packets;   // [k] slots
        std::vector<uint8_t>              parity;
        uint8_t  received_data = 0;
        // Count of slots filled by ORIGINAL-transmission packets (not retx).
        // Used to measure channel loss BEFORE retransmissions mask it.
        // received_data - fresh_received = retx packets that filled gaps.
        uint8_t  fresh_received = 0;
        bool     fec_received = false;
        bool     resolved = false;
    };

    void populate_group_from_ring(FecGroup& group);
    void try_recover(FecGroup& group,
                     std::vector<std::vector<uint8_t>>& recovered,
                     bool attempt_xor);
    void expire_old_groups();
    void update_loss(int missing, int k);

    std::unordered_map<uint16_t, FecGroup> groups_;

    // Ring buffer: pkt_key -> wire bytes.  FIFO eviction via deque.
    std::unordered_map<uint32_t, std::vector<uint8_t>> ring_;
    std::deque<uint32_t> ring_fifo_;
    static constexpr size_t MAX_RING = 512;

    float ewma_loss_ = 0.0f;
    static constexpr float  EWMA_ALPHA = 0.15f;
    static constexpr size_t MAX_GROUPS = 64;
};

} // namespace deskbeam::net
