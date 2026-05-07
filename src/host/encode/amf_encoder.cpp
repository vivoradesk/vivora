#ifdef DESKBEAM_WINDOWS

#include "host/encode/amf_encoder.h"
#include "common/utils/log.h"

// AMF headers
#include "core/Factory.h"
#include "core/Context.h"
#include "core/Surface.h"
#include "core/Buffer.h"
#include "core/Version.h"
#include "components/Component.h"
#include "components/VideoEncoderHEVC.h"
#include "components/VideoEncoderVCE.h"
#include "components/ColorSpace.h"

#include <windows.h>
#include <algorithm>

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

    const bool is_hevc = (config_.codec == VideoCodec::HEVC);
    bool hdr_log = is_hevc && (config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    log::info(TAG, "AMF %s %s encoder initialized: %ux%u @ %u fps, %u kbps, %s",
              is_hevc ? "HEVC" : "H.264",
              hdr_log ? "Main10 (10-bit)" : (is_hevc ? "Main (8-bit)" : "High (8-bit)"),
              config_.width, config_.height, config_.fps, config_.bitrate_bps / 1000,
              hdr_log ? "HDR" : "SDR");
    return true;
}

bool AmfEncoder::create_encoder() {
    const bool is_hevc = (config_.codec == VideoCodec::HEVC);

    AMF_RESULT res = factory_->CreateComponent(
        context_,
        is_hevc ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC,
        &encoder_);
    if (res != AMF_OK) {
        log::error(TAG, "CreateComponent(%s) failed: %d", is_hevc ? "HEVC" : "AVC", res);
        return false;
    }

    bool hdr = is_hevc && (config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!is_hevc && config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        log::warn(TAG, "HDR requested with --codec=h264; AMF H.264 can't carry 10-bit HDR, encoding as 8-bit");
    }
    AMF_SURFACE_FORMAT amf_fmt = hdr ? AMF_SURFACE_RGBA_F16 : AMF_SURFACE_BGRA;

    if (is_hevc) {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, (amf_int64)AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);

        // HDR: Main10 profile, 10-bit. SDR: Main profile, 8-bit (standard NV12).
        if (hdr) {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, (amf_int64)AMF_COLOR_BIT_DEPTH_10);
        } else {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, (amf_int64)AMF_COLOR_BIT_DEPTH_8);
        }

        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, (amf_int64)AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, (amf_int64)config_.bitrate_bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, (amf_int64)config_.bitrate_bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, AMFConstructRate(config_.fps, 1));
        // Force one output per input. Default for ULTRA_LOW_LATENCY is true,
        // which lets AMF coalesce identical inputs into fewer outputs and
        // halves the wire packet rate on a static screen — which is
        // exactly what we don't want for the keep-alive heartbeat.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_SKIP_FRAME_ENABLE, false);
        // VBV = 1s of bitrate (matches NVENC/QSV) — smooths burstiness so
        // CBR can't dump a 300KB IDR onto the wire in one packet train.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, (amf_int64)config_.bitrate_bps);
        // Hard cap on a single frame ~1.5× avg-frame budget. At
        // 35Mbps@60fps that's ~110KB — keeps IDRs from spiralling FEC on
        // WiFi where 300KB IDRs lose at least one fragment per group.
        const amf_int64 avg_frame_bits = (amf_int64)config_.bitrate_bps / std::max<uint32_t>(1, config_.fps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_AU_SIZE, (avg_frame_bits * 3) / 2);

        // AMF intra-refresh attempt was reverted: in our config the
        // first P-frame after init still ballooned to 100KB+, header
        // insertion didn't fire (no GOP boundaries), and recovery
        // IDRs were never tagged correctly in the bitstream.  Stick
        // with a classical short GOP / periodic IDR — combined with
        // MAX_AU_SIZE caps above the IDRs stay small enough to traverse
        // a lossy WiFi link.  See project_amf_hdr_broken / project_intra_refresh.
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64)1);
        if (config_.idr_period > 0) {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64)config_.idr_period);
        }

        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_FULL_RANGE_COLOR, true);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR,
                              (amf_int64)(hdr ? AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO
                                              : AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_FULL));
    } else {
        encoder_->SetProperty(AMF_VIDEO_ENCODER_USAGE, (amf_int64)AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64)AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE, (amf_int64)AMF_VIDEO_ENCODER_PROFILE_HIGH);

        encoder_->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64)AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)config_.bitrate_bps);
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(config_.fps, 1));
        encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0);

        if (config_.idr_period > 0) {
            encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)config_.idr_period);
        }

        // H.264 uses single FullRangeColor bool (deprecated alias, but only one exposed).
        encoder_->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, true);
    }

    res = encoder_->Init(amf_fmt, config_.width, config_.height);
    if (res != AMF_OK) {
        log::error(TAG, "Encoder Init(%s, %ux%u) failed: %d",
                   hdr ? "RGBA_F16" : "BGRA", config_.width, config_.height, res);
        return false;
    }

    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* texture, uint64_t pts_us) {
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
        const bool is_hevc = (config_.codec == VideoCodec::HEVC);
        if (is_hevc) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
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

bool AmfEncoder::encode_skip(ID3D11Texture2D* texture, uint64_t pts_us) {
    if (!encoder_) return false;

    AMFSurface* surface = nullptr;
    AMF_RESULT res = context_->AllocSurface(AMF_MEMORY_DX11,
        config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? AMF_SURFACE_RGBA_F16 : AMF_SURFACE_BGRA,
        config_.width, config_.height, &surface);
    if (res != AMF_OK) {
        log::error(TAG, "AllocSurface (heartbeat) failed: %d", res);
        return false;
    }

    ID3D11Texture2D* amf_texture = (ID3D11Texture2D*)surface->GetPlaneAt(0)->GetNative();
    d3d_context_->CopyResource(amf_texture, texture);
    surface->SetPts(pts_us);

    // If a client recovery IDR is pending and we're on a static screen,
    // real captures aren't going to fire — so the heartbeat tick must
    // service the IDR itself, otherwise the client sits in pre-keyframe
    // forever and decoded FPS hits zero.
    const bool service_idr = idr_requested_;
    const bool tag_as_heartbeat = !service_idr;

    if (config_.codec == VideoCodec::HEVC) {
        if (service_idr) {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else {
            // Force P-frame to stop AMF coalescing identical-content
            // inputs into AMF_REPEAT no-output (halves wire rate).
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_P);
        }
    } else {
        if (service_idr) {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                                 (amf_int64)AMF_VIDEO_ENCODER_PICTURE_TYPE_P);
        }
    }
    if (service_idr) idr_requested_ = false;

    // Drain stale outputs FIRST so the heartbeat-tag counter only applies
    // to outputs from the submit below.
    drain_output();
    if (tag_as_heartbeat) ++pending_skip_inputs_;

    res = encoder_->SubmitInput(surface);
    if (res == AMF_INPUT_FULL) {
        drain_output();
        res = encoder_->SubmitInput(surface);
    }
    surface->Release();
    if (res != AMF_OK) {
        log::error(TAG, "SubmitInput (heartbeat) failed: %d", res);
        if (tag_as_heartbeat) --pending_skip_inputs_;  // unwind
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
            if (config_.codec == VideoCodec::HEVC) {
                data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &pic_type);
                pkt.keyframe = (pic_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ||
                                pic_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I);
                // Diagnostic: log every distinct pic_type seen so we can tell
                // if AMF in intra-refresh mode tags forced IDRs as IDR/I/P.
                static amf_int64 last_logged = -1;
                if (pic_type != last_logged) {
                    last_logged = pic_type;
                    log::info(TAG, "HEVC pic_type=%lld size=%zu keyframe=%d",
                              (long long)pic_type, pkt.data.size(), (int)pkt.keyframe);
                }
            } else {
                data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &pic_type);
                pkt.keyframe = (pic_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
                                pic_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I);
            }

            // Tag heartbeats: pending_skip_inputs_ tracks how many skip
            // submits haven't been drained yet (AMF processes in-order).
            if (pending_skip_inputs_ > 0) {
                pkt.heartbeat = true;
                --pending_skip_inputs_;
            }

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

// Factory moved to encoder_factory.cpp

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
