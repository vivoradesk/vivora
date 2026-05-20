#pragma once

#include "common/audio/audio_codec.h"
#include "common/audio/resampler.h"
#include "common/crypto/noise_nk.h"
#include "common/net/socket.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace vivora::host {

// Consumes device-native PCM from AudioCapture, resamples to 48kHz stereo,
// Opus-encodes 10ms frames, and sends each as PacketType::Audio to every
// registered destination over the given UDP socket.
//
// feed() is called from the capture thread — it is the only mutator of the
// internal accumulator. Destination list is guarded by a mutex for the
// multi-client case.
class AudioSender {
public:
    explicit AudioSender(net::IUdpSocket& socket);

    bool init(int bitrate_bps);

    // Register a destination.  `send_cs` — if non-null — is used to AEAD-seal
    // every outbound packet for that destination (one nonce counter per
    // client).  Pointer lifetime must outlive the destination registration;
    // in practice it lives inside the ClientInfo entry owned by HostSession.
    // When send_cs is null the packet ships plaintext (test / future use).
    void add_destination(const net::SocketAddr& dest,
                         crypto::CipherState* send_cs = nullptr);
    void remove_destination(const net::SocketAddr& dest);
    size_t destination_count() const;

    // Called from capture callback. samples is interleaved float.
    void feed(const float* samples, uint32_t frames,
              uint32_t sample_rate, uint16_t channels);

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t bytes_sent()   const { return bytes_sent_;   }

private:
    bool ensure_resampler(uint32_t src_rate, uint16_t src_channels);
    void convert_to_stereo(const float* in, uint32_t frames, uint16_t in_channels,
                           std::vector<float>& out);
    void emit_packet();

    net::IUdpSocket& socket_;
    audio::OpusAudioEncoder encoder_;
    audio::Resampler resampler_;

    uint32_t src_rate_     = 0;
    uint16_t src_channels_ = 0;

    std::vector<float> stereo_scratch_;   // src in stereo form
    std::vector<float> resampled_;        // output of resampler
    std::vector<float> accum_;            // 48kHz stereo waiting to be framed

    struct Dest {
        net::SocketAddr      addr;
        crypto::CipherState* send_cs;  // nullable — plaintext when null
    };
    mutable std::mutex dests_mu_;
    std::vector<Dest>  dests_;

    uint16_t audio_seq_   = 0;
    uint32_t start_us_    = 0;
    uint64_t packets_sent_ = 0;
    uint64_t bytes_sent_   = 0;
    uint64_t feeds_called_ = 0;          // total feed() invocations
    uint64_t send_fail_count_ = 0;       // send_to rc<=0 occurrences
    uint64_t last_log_pkts_ = 0;
    std::chrono::steady_clock::time_point last_log_time_{};
};

} // namespace vivora::host
