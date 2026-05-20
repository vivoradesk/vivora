#pragma once

#include "common/protocol/packet.h"
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>

namespace vivora::net {

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
//
// Reed-Solomon systematic FEC over GF(256).  Each closed group of K data
// packets produces M parity packets (K + M ≤ 255).  Recovery tolerates any M
// missing shards out of the K + M.  M is adaptive — host raises it under loss.

class FecEncoder {
public:
    // Set K (group size).  Clamped to [2, 128].
    void set_group_size(uint8_t k);
    // Set M (parity count).  Clamped to [1, 64].
    void set_parity_count(uint8_t m);

    uint8_t group_size()   const { return k_; }
    uint8_t parity_count() const { return m_; }

    // Accept one data packet.  Returns M parity wire packets when the group
    // just closed (count reached K), otherwise an empty vector.
    std::vector<std::vector<uint8_t>> feed(
        const uint8_t* wire, size_t len,
        uint16_t frame_seq, uint32_t timestamp);

    // Close a partial group early.  Returns M parity wire packets covering
    // whatever was accumulated (with effective K = count so far), or empty
    // if no data packets pending.
    std::vector<std::vector<uint8_t>> flush(uint16_t frame_seq,
                                            uint32_t timestamp);

private:
    std::vector<std::vector<uint8_t>> emit_parities(uint16_t frame_seq,
                                                    uint32_t timestamp);
    void reset_group();

    uint8_t  k_ = 10;
    uint8_t  m_ = 2;
    // 16-bit group id wraps every ~65k groups.  At K=10 / 60fps that's
    // ~18 minutes of continuous streaming; in practice IDRs fire far
    // more often than that and each IDR triggers FecDecoder::reset()
    // on the client, so collisions across a wrap are effectively
    // impossible.  Upgrade to 32-bit + epoch if ever running multi-hour
    // sessions without an IDR.
    uint16_t group_id_ = 0;

    std::vector<std::vector<uint8_t>> data_shards_;  // raw wire bytes of data pkts
    std::vector<uint32_t>             pkt_keys_;
    std::vector<uint16_t>             pkt_lens_;
    uint8_t                           count_ = 0;
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

    float    loss_rate()       const { return ewma_loss_; }
    // Cumulative counters for the HUD.  recovered = data packets that FEC
    // reconstructed for us; failed = groups whose missing-packet count
    // exceeded the parity budget (M) and could not be reconstructed.
    uint64_t total_recovered() const { return total_recovered_; }
    uint64_t total_failed()    const { return total_failed_; }

    // Drop all state — call after an IDR / stream reset so stale groups
    // and old ring entries don't match against the fresh packet stream.
    // Also guards against 16-bit group_id_ wraparound on very long
    // sessions (see note in encoder), since in practice an IDR fires far
    // more often than every ~18 minutes.
    void reset();

private:
    struct FecGroup {
        uint8_t  k = 0;
        uint8_t  m = 0;
        std::vector<uint32_t>             pkt_keys;    // [k]
        std::vector<uint16_t>             pkt_lens;    // [k]
        std::vector<std::vector<uint8_t>> data_shards;   // [k]  (empty = missing)
        std::vector<std::vector<uint8_t>> parity_shards; // [m]  (empty = missing)
        uint8_t  received_data   = 0;
        uint8_t  fresh_received  = 0;  // original-transmission count (not retx)
        uint8_t  received_parity = 0;
        bool     header_received = false;  // true once any parity arrived
        bool     resolved = false;
        // EWMA must see each group exactly once — set the first time
        // update_loss() fires for this group, then every later try_recover()
        // branch skips the loss update.  Guards against both the
        // missing_data==0 shortcut and the later decode path double-counting
        // if they ever overlap for the same group.
        bool     loss_counted = false;
    };

    void populate_group_from_ring(FecGroup& group);
    void try_recover(FecGroup& group,
                     std::vector<std::vector<uint8_t>>& recovered,
                     bool attempt_decode);
    void expire_old_groups();
    void update_loss(int missing, int k);

    std::unordered_map<uint16_t, FecGroup> groups_;

    // Ring buffer: pkt_key -> (wire bytes, is_retx).  The is_retx flag lets
    // populate_group_from_ring() distinguish originals from retransmits so
    // fresh_received reflects only the first-transmission count — critical
    // for loss_rate accuracy.
    //
    // Eviction is time-based: a parity packet may arrive long after its
    // data burst, and evicting by raw count (e.g. 512 entries at 60fps ×
    // tens of fragments) drops data shards before their parity arrives,
    // blocking recovery on tail-heavy bursts.  We keep entries for up to
    // RING_TTL_MS; MAX_RING remains as a safety cap in case a storm pushes
    // past the budget before TTL cleanup runs.
    struct RingEntry {
        std::vector<uint8_t> wire;
        bool is_retx = false;
    };
    std::unordered_map<uint32_t, RingEntry> ring_;
    // {key, added_at_ms from steady_clock}.  Kept sorted by insertion time
    // so the front is always the oldest candidate for TTL eviction.
    std::deque<std::pair<uint32_t, int64_t>> ring_fifo_;
    static constexpr size_t  MAX_RING    = 2048;
    static constexpr int64_t RING_TTL_MS = 300;

    float ewma_loss_ = 0.0f;
    static constexpr float  EWMA_ALPHA = 0.15f;
    uint64_t total_recovered_ = 0;
    uint64_t total_failed_    = 0;
    static constexpr size_t MAX_GROUPS = 64;
};

} // namespace vivora::net
