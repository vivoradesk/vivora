#pragma once

#ifdef VIVORA_WINDOWS

#include "host/encode/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <queue>
#include <vector>

// Forward declarations — oneVPL types live in third_party/onevpl but we
// don't want to drag them into the header. Pointer members are void* and
// the .cpp reinterpret_casts them.

namespace vivora {

using Microsoft::WRL::ComPtr;

// Intel Quick Sync encoder via oneVPL (libvpl.dll). Accepts BGRA input
// textures, performs GPU-side BGRA -> NV12 conversion via VPP, then
// encodes H.264 or HEVC. CBR with low-delay BRC for streaming.
class QsvEncoder : public IVideoEncoder {
public:
    QsvEncoder();
    ~QsvEncoder() override;

    bool init(const EncoderConfig& config, ID3D11Device* device) override;
    bool encode(ID3D11Texture2D* texture, uint64_t pts_us) override;
    bool get_packet(EncodedPacket& packet) override;
    void request_idr() override;
    void set_bitrate(uint32_t bitrate_bps) override;
    const EncoderConfig& get_config() const override { return config_; }

private:
    bool load_dispatcher();
    bool create_session();
    bool configure_vpp();
    bool configure_encoder();
    bool run_vpp(ID3D11Texture2D* bgra_input, void** nv12_surface_out);
    void drain_bitstream();

    EncoderConfig config_;
    bool idr_requested_ = false;
    bool have_encoder_ = false;
    bool have_vpp_ = false;

    // Dispatcher module.
    void* vpl_dll_ = nullptr;

    // oneVPL handles (opaque pointers, real types in .cpp).
    void* loader_ = nullptr;
    void* session_ = nullptr;

    // D3D11 (not owned).
    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11DeviceContext> d3d_context_;

    // Output bitstream — pre-allocated so EncodeFrameAsync can fill it.
    std::vector<uint8_t> bitstream_buf_;
    // Used for constructing mfxBitstream (pimpl-ish; real storage in cpp).
    void* bitstream_ = nullptr;

    // Cached encoder params (mfxVideoParam + ExtBuffers) kept alive so
    // set_bitrate() can call MFXVideoENCODE_Reset with the same structure
    // that was used at Init — otherwise Intel returns MFX_ERR_INCOMPATIBLE.
    void* enc_params_ = nullptr;

    // VBV buffer size chosen at init (KB); output bitstream buffer must be
    // at least this big or EncodeFrameAsync returns NOT_ENOUGH_BUFFER.
    uint32_t vbv_kb_ = 0;

    std::queue<EncodedPacket> output_packets_;
};

} // namespace vivora

#endif // VIVORA_WINDOWS
