// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/net/socket.h"
#include "common/net/frame_assembler.h"
#include "common/net/fec_codec.h"
#include "common/protocol/packet.h"
#include <cstdint>
#include <cstdlib>

namespace vivora::client {

// Polls UDP socket for video packets, reassembles frames.
// FEC decoder sits between the socket and the assembler: raw wire packets
// are fed to FecDecoder first, which may recover lost packets; recovered
// packets are then fed to the assembler alongside normal ones.
class VideoReceiver {
public:
    explicit VideoReceiver(net::IUdpSocket& socket) : socket_(socket) {}

    // Poll socket and feed any received packets to the assembler.
    // Returns number of packets received this call.  Used by
    // transport_test; production uses ClientSession::handle_packet directly.
    int poll();

    // Feed raw wire bytes through FEC decoder (may recover lost packets).
    void fec_feed(const uint8_t* wire, size_t len,
                  std::vector<std::vector<uint8_t>>& recovered) {
        fec_decoder_.feed(wire, len, recovered);
    }

    // Trigger deferred FEC recoveries after grace period.
    void fec_tick(std::vector<std::vector<uint8_t>>& recovered) {
        fec_decoder_.tick(recovered);
    }

    // Feed an already-deserialized video packet to the assembler.
    bool feed(const protocol::Packet& packet) { return assembler_.feed(packet); }

    // Pop next complete reassembled frame. Returns false if none available.
    //
    // Slice grouping (VIV-82): with multi-slice encode each picture arrives as
    // several frames (separate seq, SAME timestamp).  ffmpeg won't assemble a
    // picture from partial access units fed across separate decode() calls, so
    // we concatenate the slices of one picture into a single AU here and emit
    // it when the next picture starts.  Gated by VIVORA_SLICE_GROUP; off =
    // legacy 1:1 passthrough (no added latency).
    bool pop_frame(net::AssembledFrame& frame) {
        if (!slice_group_) return assembler_.pop_frame(frame);
        net::AssembledFrame f;
        while (assembler_.pop_frame(f)) {
            if (!have_accum_) { accum_ = std::move(f); have_accum_ = true; continue; }
            if (!f.heartbeat && !accum_.heartbeat &&
                f.timestamp == accum_.timestamp) {
                accum_.data.insert(accum_.data.end(), f.data.begin(), f.data.end());
                accum_.keyframe = accum_.keyframe || f.keyframe;
            } else {
                frame = std::move(accum_);
                accum_ = std::move(f);
                return true;
            }
        }
        return false;
    }

    // Collect pending fragments that need retransmit (see FrameAssembler::collect_nacks).
    std::vector<net::NackBatch> collect_nacks(int64_t gap_ms, int64_t rate_limit_ms) {
        return assembler_.collect_nacks(gap_ms, rate_limit_ms);
    }

    // VIV-88: wire keys for FEC groups sitting one or two shards short of
    // recovery (see FecDecoder::collect_rescue_keys).  Complements the
    // assembler NACK above, which can only see frames that already have a
    // fragment in hand — a group wiped by a burst is invisible to it.
    void collect_rescue_keys(std::vector<uint32_t>& out, int64_t grace_ms,
                             int64_t rate_limit_ms, size_t max_keys) {
        fec_decoder_.collect_rescue_keys(out, grace_ms, rate_limit_ms, max_keys);
    }

    // VIV-88: how long a near-complete group waits for its rescued shard before
    // being declared lost (see FecDecoder::set_rescue_window_ms).
    void set_fec_rescue_window_ms(int64_t ms) { fec_decoder_.set_rescue_window_ms(ms); }

    // Packet loss rate from FEC decoder (EWMA, 0.0–1.0).
    float    loss_rate()     const { return fec_decoder_.loss_rate(); }
    uint64_t fec_recovered() const { return fec_decoder_.total_recovered(); }
    uint64_t fec_failed()    const { return fec_decoder_.total_failed(); }

    uint64_t packets_received() const { return packets_received_; }
    uint64_t bytes_received() const { return bytes_received_; }
    uint64_t frames_completed() const { return assembler_.frames_completed(); }
    uint64_t frames_dropped() const { return assembler_.frames_dropped(); }

    // Discard every buffered frame — done after loss detection so no
    // post-loss P-frames reach the decoder before the next IDR arrives.
    // Also drops FEC decoder state so old groups + ring entries don't
    // linger across the IDR boundary.
    void reset_stream() {
        // Preserve the delivery cursor: this is only ever called mid-stream for
        // loss/decode-error recovery, and a full reset re-delivered already-seen
        // frames → duplicate POC → decoder reject → IDR churn (VIV-82).
        assembler_.reset(/*preserve_position=*/true);
        fec_decoder_.reset();
        have_accum_ = false;  // drop any half-grouped picture on IDR reset
    }

private:
    net::IUdpSocket& socket_;
    net::FecDecoder fec_decoder_;
    net::FrameAssembler assembler_;
    std::vector<std::vector<uint8_t>> recovered_scratch_;  // reused across poll() calls

    // Slice grouping (VIV-82) — see pop_frame().
    bool slice_group_ = [] {
        const char* s = std::getenv("VIVORA_SLICE_GROUP");
        return s && std::atoi(s) != 0;
    }();
    net::AssembledFrame accum_;
    bool have_accum_ = false;

    uint64_t packets_received_ = 0;
    uint64_t bytes_received_ = 0;

    static constexpr size_t RECV_BUF_SIZE = 2048;
};

} // namespace vivora::client
