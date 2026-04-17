#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <queue>

namespace deskbeam {

using Microsoft::WRL::ComPtr;

class NvencEncoder : public IVideoEncoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

    bool init(const EncoderConfig& config, ID3D11Device* device) override;
    bool encode(ID3D11Texture2D* texture, uint64_t pts_us) override;
    bool get_packet(EncodedPacket& packet) override;
    void request_idr() override;
    void set_bitrate(uint32_t bitrate_bps) override;
    const EncoderConfig& get_config() const override { return config_; }

private:
    bool load_nvenc();
    bool find_nvidia_device(ID3D11Device* capture_device);
    bool open_session();
    bool configure_encoder();
    bool create_io_buffers();
    void destroy_io_buffers();

    EncoderConfig config_;
    bool idr_requested_ = false;

    // Runtime struct version helper (patches compiled API version to match driver).
    uint32_t api_version_ = 0;  // actual NVENC API version from driver
    uint32_t ver(uint32_t compiled_ver) const;

    // NVENC handles
    HMODULE nvenc_dll_ = nullptr;
    void*   encoder_   = nullptr;  // opaque NvEnc session handle
    void*   fn_list_storage_[512] = {};  // backing for NV_ENCODE_API_FUNCTION_LIST

    // D3D11 device used by NVENC (may differ from capture device on hybrid GPUs)
    ID3D11Device* device_ = nullptr;  // points to either capture device or own_device_
    ComPtr<ID3D11DeviceContext> d3d_context_;

    // Own D3D11 device on NVidia adapter (hybrid GPU: capture on Intel, encode on NVidia)
    ComPtr<ID3D11Device> own_device_;
    ComPtr<ID3D11DeviceContext> own_context_;
    bool cross_device_ = false;

    // CPU staging texture on the CAPTURE device (for cross-device read-back)
    ID3D11Device* capture_device_ = nullptr;  // not owned, only used in cross-device mode
    ComPtr<ID3D11DeviceContext> capture_context_;
    ComPtr<ID3D11Texture2D> cpu_read_staging_;  // D3D11_USAGE_STAGING on capture device

    // I/O resources — single-buffered (synchronous low-latency mode)
    void* registered_resource_ = nullptr;  // NV_ENC_REGISTERED_PTR
    void* output_bitstream_    = nullptr;  // NV_ENC_OUTPUT_PTR
    ComPtr<ID3D11Texture2D> staging_texture_;  // on NVENC device

    // Output queue
    std::queue<EncodedPacket> output_packets_;

    // Track init state for encoder params (needed for reconfigure)
    struct InitState {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t bitrate = 0;
    } init_state_;

    bool is_hdr_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
