#include "common/net/fec_codec.h"
#include "common/utils/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

namespace vivora::net {

using protocol::PacketHeader;
using protocol::PacketType;
using protocol::FLAG_FEC;
using protocol::FLAG_FEC_RANGED;

namespace {

// ============================================================================
// GF(256) arithmetic  (primitive poly 0x11D = x^8 + x^4 + x^3 + x^2 + 1)
// ============================================================================

struct Gf256Tables {
    std::array<uint8_t, 256> exp{};  // exp[i] = a^i for a = 2  (i in [0, 255])
    std::array<uint8_t, 256> log{};  // log[a^i] = i             (log[0] undefined)

    Gf256Tables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        exp[255] = exp[0];  // wrap — convenient for mul
    }
};

const Gf256Tables& gf() {
    static const Gf256Tables t;
    return t;
}

inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    const auto& t = gf();
    uint16_t s = t.log[a] + t.log[b];
    if (s >= 255) s -= 255;
    return t.exp[s];
}

inline uint8_t gf_inv(uint8_t a) {
    // a != 0
    const auto& t = gf();
    return t.exp[255 - t.log[a]];
}

// ============================================================================
// Reed-Solomon matrix helpers  (Cauchy construction, systematic decoding)
// ============================================================================
//
// Cauchy matrix C[i][j] = 1 / (x_i ⊕ y_j) with x_i, y_j distinct in GF(256).
// We pick x_i = K + i (i ∈ [0, M)) and y_j = j (j ∈ [0, K)).  Requires
// K + M ≤ 255 so all indices are distinct.  Any square submatrix of a Cauchy
// matrix is invertible — this is what lets us decode any K of the (K+M)
// shards.

// Build the M×K parity encoder matrix.
void build_parity_matrix(int k, int m, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(m) * k, 0);
    for (int i = 0; i < m; ++i) {
        uint8_t xi = static_cast<uint8_t>(k + i);
        for (int j = 0; j < k; ++j) {
            uint8_t yj = static_cast<uint8_t>(j);
            out[i * k + j] = gf_inv(xi ^ yj);
        }
    }
}

// Invert an N×N matrix in GF(256) in place via Gauss-Jordan.
// Returns false if singular (shouldn't happen for Cauchy submatrices).
bool invert_matrix(std::vector<uint8_t>& mat, int n) {
    std::vector<uint8_t> inv(static_cast<size_t>(n) * n, 0);
    for (int i = 0; i < n; ++i) inv[i * n + i] = 1;

    for (int col = 0; col < n; ++col) {
        // Find pivot
        int pivot = -1;
        for (int r = col; r < n; ++r) {
            if (mat[r * n + col] != 0) { pivot = r; break; }
        }
        if (pivot < 0) return false;
        if (pivot != col) {
            for (int j = 0; j < n; ++j) {
                std::swap(mat[col * n + j], mat[pivot * n + j]);
                std::swap(inv[col * n + j], inv[pivot * n + j]);
            }
        }
        // Scale row to make pivot 1
        uint8_t inv_pivot = gf_inv(mat[col * n + col]);
        for (int j = 0; j < n; ++j) {
            mat[col * n + j] = gf_mul(mat[col * n + j], inv_pivot);
            inv[col * n + j] = gf_mul(inv[col * n + j], inv_pivot);
        }
        // Eliminate other rows
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            uint8_t factor = mat[r * n + col];
            if (factor == 0) continue;
            for (int j = 0; j < n; ++j) {
                mat[r * n + j] ^= gf_mul(factor, mat[col * n + j]);
                inv[r * n + j] ^= gf_mul(factor, inv[col * n + j]);
            }
        }
    }
    mat = std::move(inv);
    return true;
}

// Encode M parity shards from K data shards (all same length = shard_len).
// parity[i] = sum_j G[i][j] * data[j]  (in GF(256), byte-wise).
void rs_encode_parity(const std::vector<std::vector<uint8_t>>& data,
                      int k, int m, size_t shard_len,
                      std::vector<std::vector<uint8_t>>& parity) {
    std::vector<uint8_t> G;
    build_parity_matrix(k, m, G);

    parity.assign(m, std::vector<uint8_t>(shard_len, 0));
    for (int i = 0; i < m; ++i) {
        auto& p = parity[i];
        for (int j = 0; j < k; ++j) {
            uint8_t coeff = G[i * k + j];
            if (coeff == 0) continue;
            const auto& d = data[j];
            for (size_t b = 0; b < shard_len; ++b) {
                p[b] ^= gf_mul(coeff, d[b]);
            }
        }
    }
}

// Recover missing data shards given the survivors.
// `data` has length K; missing slots are empty vectors, present are filled.
// `parity` has length M; empty slots are missing.
// `shard_len` is the common padded length of all shards.
// Returns true if all missing data shards could be recovered (they are written
// back into `data`).  Required: total_present >= K.
bool rs_decode(std::vector<std::vector<uint8_t>>& data,
               const std::vector<std::vector<uint8_t>>& parity,
               int k, int m, size_t shard_len) {
    // Collect K present shards: up to K from data, rest from parity.
    // Build a K×K submatrix of the full (K+M)×K encoding matrix; that
    // matrix is [I_k ; G_mk].  Row index in the full matrix for a data
    // shard j is j; for parity shard i is k + i.
    std::vector<int> present_rows;
    present_rows.reserve(k);

    // Prefer data shards (they're identity rows, cheapest).
    std::vector<bool> data_present(k, false);
    for (int j = 0; j < k; ++j) {
        if (!data[j].empty()) {
            data_present[j] = true;
            present_rows.push_back(j);
            if (static_cast<int>(present_rows.size()) == k) break;
        }
    }
    // Fill from parity for the missing data slots.
    if (static_cast<int>(present_rows.size()) < k) {
        for (int i = 0; i < m; ++i) {
            if (!parity[i].empty()) {
                present_rows.push_back(k + i);
                if (static_cast<int>(present_rows.size()) == k) break;
            }
        }
    }
    if (static_cast<int>(present_rows.size()) < k) return false;

    // Build the K×K decode matrix M_dec from [I; G] rows selected above.
    std::vector<uint8_t> G;  // M×K
    build_parity_matrix(k, m, G);

    std::vector<uint8_t> M_dec(static_cast<size_t>(k) * k, 0);
    for (int i = 0; i < k; ++i) {
        int row = present_rows[i];
        if (row < k) {
            // Identity row: 1 at column `row`.
            M_dec[i * k + row] = 1;
        } else {
            int pi = row - k;
            std::memcpy(&M_dec[i * k], &G[pi * k], k);
        }
    }

    if (!invert_matrix(M_dec, k)) return false;

    // Assemble the "received" vector of shards in the same order as present_rows.
    std::vector<const std::vector<uint8_t>*> recv(k);
    for (int i = 0; i < k; ++i) {
        int row = present_rows[i];
        recv[i] = (row < k) ? &data[row] : &parity[row - k];
    }

    // For each originally-missing data slot j, recompute:
    //   data[j] = sum_i M_dec[j][i] * recv[i]
    // (M_dec is the inverse of the present-row encoding submatrix, so it
    // directly recovers the K original data shards.  Rows where the data
    // was already present return the original bytes — we only write the
    // missing ones, to avoid surprising the caller.)
    for (int j = 0; j < k; ++j) {
        if (data_present[j]) continue;
        std::vector<uint8_t> out(shard_len, 0);
        for (int i = 0; i < k; ++i) {
            uint8_t coeff = M_dec[j * k + i];
            if (coeff == 0) continue;
            const auto& s = *recv[i];
            for (size_t b = 0; b < shard_len; ++b) {
                out[b] ^= gf_mul(coeff, s[b]);
            }
        }
        data[j] = std::move(out);
    }
    return true;
}

// ============================================================================
// FEC wire layout
// ----------------------------------------------------------------------------
//  [group_id   2B LE]
//  [K          1B   ]
//  [M          1B   ]
//  [parity_idx 1B   ]   0..M-1
//  [pkt_keys   4B * K]  (identifies each data packet in the group)
//  [pkt_lens   2B * K]  (original wire length of each data packet)
//  [parity     N bytes] (shard_len = max pkt_len, zero-padded)
// ============================================================================

constexpr size_t FEC_HEADER_FIXED = 2 + 1 + 1 + 1;  // group_id + K + M + idx

} // anonymous namespace

// ============================================================================
// FecEncoder
// ============================================================================

void FecEncoder::set_group_size(uint8_t k) {
    if (k < 2) k = 2;
    // 200 supports whole-frame pooled groups (ranged mode, VIV-82); legacy
    // never sets more than ~10.
    if (k > 200) k = 200;
    // GF(256) Reed-Solomon requires K + M <= 255.  All current callers keep the
    // sum in range, but enforce it here so a future caller (e.g. a VIVORA_FEC_*
    // override) can't drive an out-of-field encode (VIV-96).  Shrink parity, not
    // data: dropping data shards would silently truncate the frame.
    k_ = k;
    if (static_cast<int>(k_) + static_cast<int>(m_) > 255)
        m_ = static_cast<uint8_t>(255 - k_);
}

void FecEncoder::set_parity_count(uint8_t m) {
    if (m < 1) m = 1;
    if (m > 200) m = 200;
    if (static_cast<int>(k_) + static_cast<int>(m) > 255)
        m = static_cast<uint8_t>(255 - k_);
    m_ = m;
}

void FecEncoder::reset_group() {
    count_ = 0;
    data_shards_.clear();
    pkt_keys_.clear();
    pkt_lens_.clear();
}

std::vector<std::vector<uint8_t>> FecEncoder::feed(
        const uint8_t* wire, size_t len,
        uint16_t frame_seq, uint32_t timestamp)
{
    uint32_t key = wire_pkt_key(wire, len);
    data_shards_.emplace_back(wire, wire + len);
    pkt_keys_.push_back(key);
    pkt_lens_.push_back(static_cast<uint16_t>(len));
    ++count_;

    if (count_ >= k_) {
        auto out = emit_parities(frame_seq, timestamp);
        ++group_id_;
        reset_group();
        return out;
    }
    return {};
}

std::vector<std::vector<uint8_t>> FecEncoder::flush(uint16_t frame_seq,
                                                    uint32_t timestamp)
{
    if (count_ == 0) return {};
    auto out = emit_parities(frame_seq, timestamp);
    ++group_id_;
    reset_group();
    return out;
}

std::vector<std::vector<uint8_t>> FecEncoder::emit_parities(
        uint16_t frame_seq, uint32_t timestamp)
{
    const int k_eff = count_;         // may be < k_ on flush
    const int m_eff = m_;

    // Pad all data shards to the max wire length.
    size_t shard_len = 0;
    for (const auto& d : data_shards_) shard_len = std::max(shard_len, d.size());
    std::vector<std::vector<uint8_t>> data_padded(k_eff);
    for (int j = 0; j < k_eff; ++j) {
        data_padded[j].assign(shard_len, 0);
        std::memcpy(data_padded[j].data(), data_shards_[j].data(), data_shards_[j].size());
    }

    // Encode M parity shards.
    std::vector<std::vector<uint8_t>> parity;
    rs_encode_parity(data_padded, k_eff, m_eff, shard_len, parity);

    // Ranged (VIV-82): header carries only base_key (4B) — the K data packets
    // have consecutive keys base_key .. base_key+K-1.  Legacy: full key+len
    // list (6B per data packet), which caps K at ~16 before the parity packet
    // exceeds the MTU.
    const size_t hdr_var = ranged_ ? 4u
                                   : static_cast<size_t>(k_eff) * (4 + 2);
    const size_t fec_payload_size = FEC_HEADER_FIXED + hdr_var + shard_len;

    std::vector<std::vector<uint8_t>> out;
    out.reserve(m_eff);

    for (int pi = 0; pi < m_eff; ++pi) {
        std::vector<uint8_t> wire(PacketHeader::WIRE_SIZE + fec_payload_size);

        PacketHeader hdr;
        hdr.type = PacketType::Video;
        hdr.seq_no = frame_seq;
        hdr.timestamp = timestamp;
        hdr.flags = ranged_ ? (FLAG_FEC | FLAG_FEC_RANGED) : FLAG_FEC;
        hdr.payload_len = static_cast<uint16_t>(fec_payload_size);
        hdr.serialize(wire.data());

        uint8_t* p = wire.data() + PacketHeader::WIRE_SIZE;

        // group_id
        p[0] = static_cast<uint8_t>(group_id_ & 0xFF);
        p[1] = static_cast<uint8_t>(group_id_ >> 8);
        p += 2;

        // K, M, parity_idx
        *p++ = static_cast<uint8_t>(k_eff);
        *p++ = static_cast<uint8_t>(m_eff);
        *p++ = static_cast<uint8_t>(pi);

        if (ranged_) {
            uint32_t base = pkt_keys_.empty() ? 0u : pkt_keys_[0];
            p[0] = static_cast<uint8_t>(base);
            p[1] = static_cast<uint8_t>(base >> 8);
            p[2] = static_cast<uint8_t>(base >> 16);
            p[3] = static_cast<uint8_t>(base >> 24);
            p += 4;
        } else {
            for (int j = 0; j < k_eff; ++j) {
                uint32_t key = pkt_keys_[j];
                p[0] = static_cast<uint8_t>(key);
                p[1] = static_cast<uint8_t>(key >> 8);
                p[2] = static_cast<uint8_t>(key >> 16);
                p[3] = static_cast<uint8_t>(key >> 24);
                p += 4;
            }
            for (int j = 0; j < k_eff; ++j) {
                uint16_t l = pkt_lens_[j];
                p[0] = static_cast<uint8_t>(l & 0xFF);
                p[1] = static_cast<uint8_t>(l >> 8);
                p += 2;
            }
        }
        // parity shard
        std::memcpy(p, parity[pi].data(), shard_len);

        out.push_back(std::move(wire));
    }
    return out;
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
        const bool ranged = (hdr.flags & FLAG_FEC_RANGED) != 0;
        const uint8_t* p = wire + PacketHeader::WIRE_SIZE;
        const size_t payload_len = len - PacketHeader::WIRE_SIZE;
        if (payload_len < FEC_HEADER_FIXED) return;

        uint16_t group_id  = p[0] | (static_cast<uint16_t>(p[1]) << 8);
        uint8_t  k         = p[2];
        uint8_t  m         = p[3];
        uint8_t  parity_idx = p[4];
        p += FEC_HEADER_FIXED;

        if (k < 1 || k > 200) return;
        if (m < 1 || m > 200) return;
        if (parity_idx >= m) return;

        // Ranged header carries only base_key (4B); legacy carries the full
        // K-long key+len list (6B per data packet).
        const size_t hdr_var = ranged ? 4u : static_cast<size_t>(k) * (4 + 2);
        if (payload_len < FEC_HEADER_FIXED + hdr_var) return;
        const size_t shard_len = payload_len - FEC_HEADER_FIXED - hdr_var;

        auto& group = groups_[group_id];

        if (!group.header_received) {
            group.k = k;
            group.m = m;
            group.ranged = ranged;
            group.header_received = true;
            // VIV-88: the group's composition is known from this moment, and
            // with the production transmit order (all parity in the frame's
            // tail) its data has already come and gone — so this is the clock
            // the rescue-NACK grace window runs off.
            group.first_seen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            group.data_shards.assign(k, {});
            group.parity_shards.assign(m, {});
            group.pkt_keys.resize(k);

            if (ranged) {
                // Consecutive keys base_key .. base_key+K-1.  Lengths come from
                // each recovered packet's own header (PayloadLen), so no list.
                uint32_t base = p[0]
                    | (static_cast<uint32_t>(p[1]) << 8)
                    | (static_cast<uint32_t>(p[2]) << 16)
                    | (static_cast<uint32_t>(p[3]) << 24);
                for (int j = 0; j < k; ++j)
                    group.pkt_keys[j] = base + static_cast<uint32_t>(j);
                p += 4;
            } else {
                group.pkt_lens.resize(k);
                for (int j = 0; j < k; ++j) {
                    group.pkt_keys[j] = p[0]
                        | (static_cast<uint32_t>(p[1]) << 8)
                        | (static_cast<uint32_t>(p[2]) << 16)
                        | (static_cast<uint32_t>(p[3]) << 24);
                    p += 4;
                }
                for (int j = 0; j < k; ++j) {
                    group.pkt_lens[j] = p[0] | (static_cast<uint16_t>(p[1]) << 8);
                    p += 2;
                }
            }
            // Pull any already-received data packets from the ring.
            populate_group_from_ring(group);
        } else {
            // A second FEC header for an existing group must agree on K/M
            // with the first one we saw.  Under extreme loss a corrupted
            // packet (or a stale group_id reused before its 300ms TTL) can
            // present a *different* M; parity_idx was range-checked against
            // this packet's M (line above) but indexes parity_shards, which
            // was sized to the ORIGINAL M at header_received.  A larger M
            // then writes past the vector -> heap overflow / segfault
            // (VIV-11, reproducible on Linux under netem loss 50%).  Drop
            // the mismatching packet; the group still recovers from shards
            // that do agree.
            // `ranged` must also match: it selects the variable-header layout
            // (hdr_var above was computed from THIS packet's flag), and it
            // governs how the recovered shard bytes map back to packet
            // keys/lengths.  A mismatch feeds garbage into rs_decode(), which
            // would emit silently-corrupted "recovered" packets — a no-artifact
            // violation, same class as the VIV-11 K/M mismatch (VIV-96).
            if (k != group.k || m != group.m || ranged != group.ranged) return;
            // Skip the header we already have.
            p += hdr_var;
        }

        // Belt-and-braces: never index parity_shards past its real size,
        // even if a future change lets a mismatching M slip through above.
        if (parity_idx >= group.parity_shards.size()) return;

        // Store parity shard (first copy wins).
        if (group.parity_shards[parity_idx].empty()) {
            group.parity_shards[parity_idx].assign(p, p + shard_len);
            ++group.received_parity;
        }

        // Recover in-line the instant we hold k shards, instead of deferring to
        // the next tick() poll cycle — that deferral added ~16ms (one 60fps
        // frame) of latency to every FEC recovery, enough to miss the decode
        // deadline and stall the frame (VIV-82).  RS decode still only runs when
        // actually recoverable (try_recover guards on k shards present).
        try_recover(group, recovered,
                    group.received_data + group.received_parity >= group.k);

    } else if (hdr.type == PacketType::Video) {
        // ---- Data packet ----
        const bool is_retx = (hdr.flags & protocol::FLAG_RETX) != 0;

        // Strip FLAG_RETX so stored bytes match what the sender fed into
        // FEC on first transmission.
        std::vector<uint8_t> clean_wire(wire, wire + len);
        if (is_retx && clean_wire.size() > 7) {
            clean_wire[7] &= ~protocol::FLAG_RETX;
        }

        uint32_t key = wire_pkt_key(clean_wire.data(), clean_wire.size());

        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        auto existing = ring_.find(key);
        if (existing == ring_.end()) {
            ring_fifo_.push_back({key, now_ms});
            RingEntry entry;
            entry.wire = clean_wire;
            entry.is_retx = is_retx;
            ring_[key] = std::move(entry);
        } else {
            // An original arriving after a retx "promotes" the entry —
            // but in practice FEC groups freeze their is_retx snapshot
            // at populate time, so we only update if still useful.
            existing->second.wire = clean_wire;
            if (!is_retx) existing->second.is_retx = false;
        }

        // Evict by age first (parity can lag behind data by a burst's
        // worth — TTL must outlive that window), then enforce the hard
        // count cap as a safety net.
        while (!ring_fifo_.empty() &&
               now_ms - ring_fifo_.front().second > RING_TTL_MS) {
            uint32_t oldest = ring_fifo_.front().first;
            ring_fifo_.pop_front();
            ring_.erase(oldest);
        }
        while (ring_.size() > MAX_RING && !ring_fifo_.empty()) {
            uint32_t oldest = ring_fifo_.front().first;
            ring_fifo_.pop_front();
            ring_.erase(oldest);
        }

        for (auto& [gid, group] : groups_) {
            if (group.resolved || !group.header_received) continue;
            for (int j = 0; j < group.k; ++j) {
                if (group.pkt_keys[j] == key && group.data_shards[j].empty()) {
                    group.data_shards[j] = clean_wire;
                    ++group.received_data;
                    if (!is_retx) ++group.fresh_received;
                    // Recover in-line once we hold k shards (VIV-82) — see the
                    // parity path above for why the tick()-deferred decode hurt.
                    try_recover(group, recovered,
                                group.received_data + group.received_parity >= group.k);
                    break;
                }
            }
        }
    }

    expire_old_groups();
}

void FecDecoder::tick(std::vector<std::vector<uint8_t>>& recovered) {
    for (auto& [gid, group] : groups_) {
        if (group.resolved || !group.header_received) continue;
        try_recover(group, recovered, true);
    }
}

void FecDecoder::populate_group_from_ring(FecGroup& group) {
    for (int j = 0; j < group.k; ++j) {
        uint32_t key = group.pkt_keys[j];
        auto it = ring_.find(key);
        if (it != ring_.end() && group.data_shards[j].empty()) {
            group.data_shards[j] = it->second.wire;
            ++group.received_data;
            // Only original transmissions count toward fresh_received —
            // a retx filling the shard means the fragment was lost on
            // first send and loss_rate must reflect that.
            if (!it->second.is_retx) ++group.fresh_received;
        }
    }
}

void FecDecoder::try_recover(FecGroup& group,
                             std::vector<std::vector<uint8_t>>& recovered,
                             bool attempt_decode) {
    if (group.resolved) return;
    if (!group.header_received) return;

    const int k = group.k;
    const int m = group.m;
    const int missing_data = k - group.received_data;
    const int fresh_missing = k - group.fresh_received;

    if (missing_data == 0) {
        group.resolved = true;
        if (!group.loss_counted) {
            update_loss(fresh_missing, k);
            group.loss_counted = true;
        }
        return;
    }

    // During recv loop: wait for tick() to drain the rest of the burst.
    if (!attempt_decode) return;

    if (!group.loss_counted) {
        update_loss(fresh_missing, k);
        group.loss_counted = true;
    }

    // Need at least K total shards (data or parity) to decode.
    const int total_present = group.received_data + group.received_parity;
    if (total_present < k) {
        // VIV-88: tick() runs every poll, so without this a group that is one
        // shard short is buried within milliseconds — long before a targeted
        // retransmit could arrive, which would make the rescue NACK dead code.
        // Hold a near-complete group open for the rescue window instead; the
        // retx lands via the normal data path and closes it.  Bounded, and only
        // for groups actually worth chasing: everything else fails immediately,
        // exactly as before.  With the window at 0 (the default, and what any
        // non-client user of the decoder gets) behaviour is unchanged.
        const int shortfall = k - total_present;
        if (rescue_window_ms_ > 0 && shortfall <= MAX_RESCUE_SHORTFALL) {
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - group.first_seen_ms < rescue_window_ms_) return;
        }
        // Not enough — give up on RS but keep group marked resolved so
        // we don't retry.  Frame assembler will handle the gap.
        ++total_failed_;
        // Diagnostic: tells us whether failures cluster at the M cap
        // (need a stronger response than parity) or below it (the
        // adaptation logic was too slow / decayed too far).
        log::warn("FEC", "Group %d FAILED: K=%d M=%d, missing %d data + %d parity (need %d more)",
                  static_cast<int>(group.pkt_keys[0] >> 16),
                  k, m, missing_data,
                  m - group.received_parity,
                  k - total_present);
        group.resolved = true;
        return;
    }

    // Pad data shards (present) and parity shards to shard_len.
    size_t shard_len = 0;
    for (const auto& d : group.data_shards)   shard_len = std::max(shard_len, d.size());
    for (const auto& pS : group.parity_shards) shard_len = std::max(shard_len, pS.size());

    std::vector<std::vector<uint8_t>> data_pad(k);
    std::vector<bool> was_present(k, false);
    for (int j = 0; j < k; ++j) {
        if (!group.data_shards[j].empty()) {
            data_pad[j].assign(shard_len, 0);
            std::memcpy(data_pad[j].data(),
                        group.data_shards[j].data(),
                        group.data_shards[j].size());
            was_present[j] = true;
        }
    }
    std::vector<std::vector<uint8_t>> parity_pad = group.parity_shards;
    for (auto& ps : parity_pad) {
        if (!ps.empty() && ps.size() < shard_len) ps.resize(shard_len, 0);
    }

    if (!rs_decode(data_pad, parity_pad, k, m, shard_len)) {
        ++total_failed_;
        group.resolved = true;
        return;
    }

    int recovered_count = 0;
    for (int j = 0; j < k; ++j) {
        if (was_present[j]) continue;
        // Truncate back to original wire length.  Ranged groups don't carry a
        // length list — the recovered shard IS the full data wire, so read its
        // own PacketHeader.PayloadLen (bytes 8-9) to find the real length.
        size_t orig_len;
        if (group.ranged) {
            if (data_pad[j].size() >= PacketHeader::WIRE_SIZE) {
                uint16_t pl = data_pad[j][8]
                            | (static_cast<uint16_t>(data_pad[j][9]) << 8);
                orig_len = PacketHeader::WIRE_SIZE + pl;
            } else {
                orig_len = data_pad[j].size();
            }
        } else {
            orig_len = group.pkt_lens[j];
        }
        if (data_pad[j].size() > orig_len)
            data_pad[j].resize(orig_len);
        recovered.push_back(std::move(data_pad[j]));
        ++recovered_count;
    }

    if (recovered_count > 0) {
        total_recovered_ += static_cast<uint64_t>(recovered_count);
        log::info("FEC", "Recovered %d pkts in group %d (K=%d, M=%d)",
                  recovered_count,
                  static_cast<int>(group.pkt_keys[0] >> 16),
                  k, m);
    }
    group.resolved = true;
}

void FecDecoder::update_loss(int missing, int k) {
    float group_loss = static_cast<float>(missing) / k;
    ewma_loss_ = EWMA_ALPHA * group_loss + (1.0f - EWMA_ALPHA) * ewma_loss_;
}

void FecDecoder::reset() {
    groups_.clear();
    ring_.clear();
    ring_fifo_.clear();
    ewma_loss_ = 0.0f;
}

void FecDecoder::collect_rescue_keys(std::vector<uint32_t>& out, int64_t grace_ms,
                                     int64_t rl_ms, size_t max_keys) {
    if (max_keys == 0) return;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (auto& [gid, group] : groups_) {
        if (out.size() >= max_keys) break;
        (void)gid;
        // Unresolved groups whose layout we know. A group with no parity yet
        // may still be mid-flight and we could not name its keys anyway.
        if (group.resolved || !group.header_received) continue;

        // How many more shards would resolve it: RS needs any k of k+m.
        const int present = static_cast<int>(group.received_data)
                          + static_cast<int>(group.received_parity);
        const int shortfall = static_cast<int>(group.k) - present;
        if (shortfall < 1 || shortfall > MAX_RESCUE_SHORTFALL) continue;

        // Never chase packets that may simply still be in flight or reordered.
        if (now_ms - group.first_seen_ms < grace_ms) continue;
        if (group.last_nack_ms != 0 && now_ms - group.last_nack_ms < rl_ms) continue;

        // Ask for exactly `shortfall` missing DATA shards — any of them closes
        // the group, and only data packets live in the host's retransmit ring
        // (parity is never stored there).
        int asked = 0;
        for (int j = 0; j < static_cast<int>(group.k) && asked < shortfall; ++j) {
            if (!group.data_shards[j].empty()) continue;
            if (out.size() >= max_keys) break;
            out.push_back(group.pkt_keys[j]);
            ++asked;
        }
        if (asked > 0) group.last_nack_ms = now_ms;
    }
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

} // namespace vivora::net
