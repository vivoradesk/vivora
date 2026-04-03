#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <queue>

// Forward declarations — AMF loaded dynamically
namespace amf {
    class AMFFactory;
    class AMFContext;
    class AMFComponent;
    class AMFData;
}

namespace deskbeam {

using Microsoft::WRL::ComPtr;

class AmfEncoder : public IVideoEncoder {
public:
    AmfEncoder();
    ~AmfEncoder() override;

    bool init(const EncoderConfig& config, ID3D11Device* device) override;
    bool encode(ID3D11Texture2D* texture, uint64_t pts_us) override;
    bool get_packet(EncodedPacket& packet) override;
    void request_idr() override;
    void set_bitrate(uint32_t bitrate_bps) override;
    const EncoderConfig& get_config() const override { return config_; }

private:
    bool load_amf();
    bool create_encoder();
    void drain_output();

    EncoderConfig config_;
    bool idr_requested_ = false;

    // AMF handles
    HMODULE amf_dll_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContext* context_ = nullptr;
    amf::AMFComponent* encoder_ = nullptr;

    // D3D11 references
    ID3D11Device* device_ = nullptr;  // not owned
    ComPtr<ID3D11DeviceContext> d3d_context_;

    // Staging texture for DXGI frames (DXGI textures lack bind flags)
    ComPtr<ID3D11Texture2D> staging_texture_;

    // Output queue
    std::queue<EncodedPacket> output_packets_;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
