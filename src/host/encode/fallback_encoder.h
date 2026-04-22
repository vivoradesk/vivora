#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include <memory>

namespace deskbeam {

// Wraps a primary encoder and swaps in a secondary one if the primary
// returns encode() failures for long enough.  Purpose: NVENC on hybrid
// Intel+NVidia laptops enters a multi-second "can't Map input" state
// when certain games start — fresh session and fresh D3D device do not
// recover it (see `project_nvenc_hybrid_limitation`).  When that happens
// we hot-swap to QSV so the stream keeps flowing instead of freezing for
// 20-75 seconds waiting for NVENC to unwedge itself.  The swap happens
// once; we don't oscillate back to the primary even if it recovers.
class FallbackEncoder : public IVideoEncoder {
public:
    FallbackEncoder(std::unique_ptr<IVideoEncoder> primary,
                    EncoderKind fallback_kind);
    ~FallbackEncoder() override;

    bool init(const EncoderConfig& config, ID3D11Device* device) override;
    bool encode(ID3D11Texture2D* texture, uint64_t pts_us) override;
    bool get_packet(EncodedPacket& packet) override;
    void request_idr() override;
    void set_bitrate(uint32_t bitrate_bps) override;
    const EncoderConfig& get_config() const override;

private:
    bool try_switch_to_fallback();

    std::unique_ptr<IVideoEncoder> primary_;
    std::unique_ptr<IVideoEncoder> secondary_;  // created lazily on failure
    IVideoEncoder* active_ = nullptr;           // aliases primary_ or secondary_

    EncoderKind fallback_kind_;
    EncoderConfig config_;
    ID3D11Device* device_ = nullptr;

    // Consecutive encode() failures on the primary.  NVENC returns false from
    // encode() on every Map failure, so this naturally counts the outage.
    uint32_t consecutive_encode_fails_ = 0;
    static constexpr uint32_t kFailureThreshold = 120;  // ~2 sec at 60 fps

    bool switched_ = false;   // one-way: once we fall back, stay fallen back
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
