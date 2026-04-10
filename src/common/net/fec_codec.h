#pragma once

#include "common/protocol/packet.h"
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace deskbeam::net {

// ---------------------------------------------------------------------------
// FEC parity packet payload layout (inside a PacketType::Video, FLAG_FEC):
//
//   [group_id    2B LE]   monotonic group counter
//   [K           1B    ]  data packets in this group
//   [first_idx   2B LE]   global packet index of first data packet
//   [pkt_len[0]  2B LE]   wire-length of data packet 0
//   ...
//   [pkt_len[K-1] 2B LE]
//   [xor_parity   NB   ]  XOR of all K wire-packets, zero-padded to max_len
//
// Header overhead: 5 + 2*K bytes.
// ---------------------------------------------------------------------------

// ---- Encoder (host side) --------------------------------------------------

class FecEncoder {
public:
    void set_group_size(uint8_t k);
    uint8_t group_size() const { return k_; }
    uint16_t global_index() const { return global_idx_; }

    // Feed a serialized wire packet (header+payload).
    // Returns true if an FEC packet was produced and written to `fec_out`.
    bool feed(const uint8_t* wire, size_t len,
              uint16_t frame_seq, uint32_t timestamp,
              std::vector<uint8_t>& fec_out);

    // Flush a partial group (< K packets). Call at keyframe boundary or K change.
    // Returns true if FEC packet produced.
    bool flush(uint16_t frame_seq, uint32_t timestamp,
               std::vector<uint8_t>& fec_out);

private:
    void xor_accumulate(const uint8_t* data, size_t len);
    std::vector<uint8_t> build_fec_wire(uint16_t frame_seq, uint32_t timestamp);
    void reset_group();

    uint8_t k_ = 10;
    uint16_t group_id_ = 0;
    uint16_t global_idx_ = 0;
    uint16_t first_idx_in_group_ = 0;

    std::vector<uint16_t> pkt_lens_;          // wire-length of each pkt in group
    std::vector<uint8_t>  parity_;            // running XOR accumulator
    uint8_t               count_ = 0;         // packets fed into current group
};

// ---- Decoder (client side) ------------------------------------------------

class FecDecoder {
public:
    // Feed a raw wire packet (data or FEC).
    // Any recovered packets are appended to `recovered` as raw wire bytes.
    void feed(const uint8_t* wire, size_t len,
              std::vector<std::vector<uint8_t>>& recovered);

    // EWMA of packet loss rate (0.0 – 1.0).
    float loss_rate() const { return ewma_loss_; }

private:
    struct FecGroup {
        uint8_t  k = 0;
        uint16_t first_idx = 0;
        std::vector<std::vector<uint8_t>> packets;   // [k] slots, empty = missing
        std::vector<uint16_t>             pkt_lens;   // from FEC header
        std::vector<uint8_t>              parity;
        uint8_t  received_data = 0;
        bool     fec_received = false;
        bool     resolved = false;           // recovery attempted or not needed
    };

    void populate_group_from_ring(FecGroup& group);
    void try_recover(FecGroup& group,
                     std::vector<std::vector<uint8_t>>& recovered);
    void expire_old_groups();
    void update_loss(const FecGroup& group);

    std::unordered_map<uint16_t, FecGroup> groups_;   // group_id -> group
    uint16_t global_idx_ = 0;                         // monotonic data-pkt counter
    float    ewma_loss_ = 0.0f;

    // Ring buffer of recent data packets keyed by global_idx.
    // Data packets arrive BEFORE the FEC packet, so we must buffer them
    // to populate the group when FEC arrives and reveals first_idx + K.
    static constexpr size_t RING_SIZE = 256;          // covers ~4 frames @ 60 frags/frame
    struct RingEntry {
        uint16_t idx = 0;
        std::vector<uint8_t> wire;
        bool valid = false;
    };
    std::vector<RingEntry> ring_{RING_SIZE};

    static constexpr float  EWMA_ALPHA = 0.05f;
    static constexpr size_t MAX_GROUPS = 64;          // expire beyond this
};

} // namespace deskbeam::net
