#pragma once

#ifdef DESKBEAM_WINDOWS

#include "client/decode/video_decoder.h"
#include <wrl/client.h>
#include <d3d11.h>
#include <queue>

struct IMFTransform;
struct IMFDXGIDeviceManager;

namespace deskbeam {

using Microsoft::WRL::ComPtr;

class MfDecoder : public IVideoDecoder {
public:
    MfDecoder() = default;
    ~MfDecoder() override;

    bool init(VideoCodec codec, ID3D11Device* device = nullptr) override;
    bool decode(const uint8_t* data, size_t len, uint64_t pts) override;
    bool get_frame(DecodedFrame& frame) override;
    void flush() override;
    ID3D11Device* get_device() override { return device_.Get(); }

private:
    bool create_device();
    bool create_decoder();
    bool configure_output();
    void drain_output();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    IMFTransform* transform_ = nullptr;
    IMFDXGIDeviceManager* device_manager_ = nullptr;
    UINT reset_token_ = 0;
    bool started_ = false;
    bool failed_ = false;  // set after any SEH crash; decoder becomes inert
    VideoCodec codec_ = VideoCodec::HEVC;

    std::queue<DecodedFrame> output_frames_;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
