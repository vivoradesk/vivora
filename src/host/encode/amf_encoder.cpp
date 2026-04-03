#ifdef DESKBEAM_WINDOWS

#include "host/encode/amf_encoder.h"
#include "common/utils/log.h"
#include "common/utils/metrics.h"

// AMF headers
#include "core/Factory.h"
#include "core/Context.h"
#include "core/Surface.h"
#include "core/Buffer.h"
#include "core/Version.h"
#include "components/Component.h"
#include "components/VideoEncoderHEVC.h"
#include "components/ColorSpace.h"

#include <windows.h>

using namespace amf;

namespace deskbeam {

static const char* TAG = "ENCODE";

AmfEncoder::AmfEncoder() = default;

AmfEncoder::~AmfEncoder() {
    staging_texture_.Reset();
    d3d_context_.Reset();

    if (encoder_) {
        encoder_->Drain();
        encoder_->Terminate();
        encoder_->Release();
        encoder_ = nullptr;
    }
    if (context_) {
        context_->Terminate();
        context_->Release();
        context_ = nullptr;
    }
    if (amf_dll_) {
        FreeLibrary(amf_dll_);
        amf_dll_ = nullptr;
    }
}

bool AmfEncoder::load_amf() {
    amf_dll_ = LoadLibraryA(AMF_DLL_NAMEA);
    if (!amf_dll_) {
        log::error(TAG, "Failed to load %s", AMF_DLL_NAMEA);
        return false;
    }

    auto init_fn = (AMFInit_Fn)GetProcAddress(amf_dll_, AMF_INIT_FUNCTION_NAME);
    if (!init_fn) {
        log::error(TAG, "AMFInit not found in %s", AMF_DLL_NAMEA);
        return false;
    }

    AMF_RESULT res = init_fn(AMF_FULL_VERSION, &factory_);
    if (res != AMF_OK) {
        log::error(TAG, "AMFInit failed: %d", res);
        return false;
    }

    log::info(TAG, "AMF loaded successfully");
    return true;
}

bool AmfEncoder::init(const EncoderConfig& config, ID3D11Device* device) {
    config_ = config;
    device_ = device;
    device_->GetImmediateContext(d3d_context_.GetAddressOf());

    if (!load_amf()) return false;

    AMF_RESULT res = factory_->CreateContext(&context_);
    if (res != AMF_OK) {
        log::error(TAG, "CreateContext failed: %d", res);
        return false;
    }

    res = context_->InitDX11(device_);
    if (res != AMF_OK) {
        log::error(TAG, "InitDX11 failed: %d", res);
        return false;
    }

    if (!create_encoder()) return false;

    log::info(TAG, "AMF HEVC 10-bit encoder initialized: %ux%u @ %u fps, %u kbps, %s",
              config_.width, config_.height, config_.fps, config_.bitrate_bps / 1000,
              config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "HDR" : "SDR");
    return true;
}

bool AmfEncoder::create_encoder() {
    AMF_RESULT res = factory_->CreateComponent(context_, AMFVideoEncoder_HEVC, &encoder_);
    if (res != AMF_OK) {
        log::error(TAG, "CreateComponent(HEVC) failed: %d", res);
        return false;
    }

    // HEVC Main10 profile for 10-bit HDR
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, (amf_int64)AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, (amf_int64)AMF_COLOR_BIT_DEPTH_10);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, (amf_int64)AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, (amf_int64)config_.bitrate_bps);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(config_.fps, 1));
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64)1);

    if (config_.idr_period > 0) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64)config_.idr_period);
    }

    bool hdr = (config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    AMF_SURFACE_FORMAT amf_fmt = hdr ? AMF_SURFACE_RGBA_F16 : AMF_SURFACE_BGRA;

    // Let AMF auto-detect color conversion — just set full range
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_FULL_RANGE_COLOR, true);
    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR,
                          (amf_int64)AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_FULL);

    res = encoder_->Init(amf_fmt, config_.width, config_.height);
    if (res != AMF_OK) {
        log::error(TAG, "Encoder Init(%s, %ux%u) failed: %d",
                   hdr ? "RGBA_F16" : "BGRA", config_.width, config_.height, res);
        return false;
    }

    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* texture, uint64_t pts_us) {
    ScopedTimer timer(TAG, "encode");

    if (!encoder_) return false;

    // Allocate AMF surface (AMF manages its own textures to avoid conflicts)
    AMFSurface* surface = nullptr;
    AMF_RESULT res = context_->AllocSurface(AMF_MEMORY_DX11,
        config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? AMF_SURFACE_RGBA_F16 : AMF_SURFACE_BGRA,
        config_.width, config_.height, &surface);
    if (res != AMF_OK) {
        log::error(TAG, "AllocSurface failed: %d", res);
        return false;
    }

    // Copy DXGI texture into the AMF surface's texture
    ID3D11Texture2D* amf_texture = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative();
    d3d_context_->CopyResource(amf_texture, texture);
    if (res != AMF_OK) {
        log::error(TAG, "CreateSurfaceFromDX11Native failed: %d", res);
        return false;
    }

    surface->SetPts(pts_us);

    // Request IDR if needed
    if (idr_requested_) {
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                             (amf_int64)AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        idr_requested_ = false;
    }

    drain_output();

    res = encoder_->SubmitInput(surface);
    if (res == AMF_INPUT_FULL) {
        drain_output();
        res = encoder_->SubmitInput(surface);
    }

    surface->Release();

    if (res != AMF_OK) {
        log::error(TAG, "SubmitInput failed: %d", res);
        return false;
    }

    drain_output();
    return true;
}

void AmfEncoder::drain_output() {
    AMFData* data = nullptr;
    while (true) {
        AMF_RESULT res = encoder_->QueryOutput(&data);
        if (res == AMF_EOF || res == AMF_REPEAT || data == nullptr) break;
        if (res != AMF_OK) break;

        AMFBuffer* buffer = nullptr;
        res = data->QueryInterface(AMFBuffer::IID(), (void**)&buffer);
        if (res == AMF_OK && buffer) {
            EncodedPacket pkt;
            pkt.data.assign(
                static_cast<uint8_t*>(buffer->GetNative()),
                static_cast<uint8_t*>(buffer->GetNative()) + buffer->GetSize()
            );
            pkt.pts = data->GetPts();

            amf_int64 pic_type = 0;
            data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &pic_type);
            pkt.keyframe = (pic_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ||
                            pic_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I);

            output_packets_.push(std::move(pkt));
            buffer->Release();
        }
        data->Release();
    }
}

bool AmfEncoder::get_packet(EncodedPacket& packet) {
    if (output_packets_.empty()) return false;
    packet = std::move(output_packets_.front());
    output_packets_.pop();
    return true;
}

void AmfEncoder::request_idr() {
    idr_requested_ = true;
}

void AmfEncoder::set_bitrate(uint32_t bitrate_bps) {
    config_.bitrate_bps = bitrate_bps;
    if (encoder_) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, (amf_int64)bitrate_bps);
    }
}

// Factory
std::unique_ptr<IVideoEncoder> IVideoEncoder::create() {
    return std::make_unique<AmfEncoder>();
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
