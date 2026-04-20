#ifdef DESKBEAM_WINDOWS

#include "host/encode/nvenc_encoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstring>

#include "nvEncodeAPI.h"

namespace deskbeam {

static const char* TAG = "NVENC";

// Helper to get function list pointer from storage.
static NV_ENCODE_API_FUNCTION_LIST* fn(void* storage) {
    return reinterpret_cast<NV_ENCODE_API_FUNCTION_LIST*>(storage);
}

NvencEncoder::NvencEncoder() = default;

// Struct version layout: 0xEmSS00MM (E = 0x7 or 0xF with bit31 flag)
//   bit  31:    extended flag (some structs set this)
//   bits 28-30: magic 0x7
//   bits 24-27: minor API version
//   bits 16-23: struct version number
//   bits 0-15:  major API version
// Patch API version while keeping struct version, magic, and bit31.
uint32_t NvencEncoder::ver(uint32_t compiled_ver) const {
    return (compiled_ver & 0xF0FF0000) | (api_version_ & 0x0F00FFFF);
}

NvencEncoder::~NvencEncoder() {
    if (encoder_) {
        // Send EOS to flush.
        NV_ENC_PIC_PARAMS eos = {};
        eos.version = ver(NV_ENC_PIC_PARAMS_VER);
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        fn(fn_list_storage_)->nvEncEncodePicture(encoder_, &eos);

        destroy_io_buffers();

        fn(fn_list_storage_)->nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    staging_texture_.Reset();
    cpu_read_staging_.Reset();
    capture_context_.Reset();
    d3d_context_.Reset();
    own_context_.Reset();
    own_device_.Reset();
    if (nvenc_dll_) {
        FreeLibrary(nvenc_dll_);
        nvenc_dll_ = nullptr;
    }
}

bool NvencEncoder::load_nvenc() {
    nvenc_dll_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!nvenc_dll_) {
        log::error(TAG, "Failed to load nvEncodeAPI64.dll");
        return false;
    }

    // Query the maximum API version the driver supports.
    auto getMaxVer = (NvEncodeAPIGetMaxSupportedVersion_t)
        GetProcAddress(nvenc_dll_, "NvEncodeAPIGetMaxSupportedVersion");
    if (getMaxVer) {
        uint32_t max_ver = 0;
        NVENCSTATUS st = getMaxVer(&max_ver);
        if (st == NV_ENC_SUCCESS) {
            // GetMaxSupportedVersion returns compact format: (major << 4) | minor
            uint32_t major = (max_ver >> 4) & 0xFFF;
            uint32_t minor = max_ver & 0xF;
            log::info(TAG, "Driver supports NVENC API v%u.%u (compiled: v%u.%u)",
                      major, minor, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            // Convert to expanded format used in struct versions: major | (minor << 24)
            api_version_ = major | (minor << 24);
        } else {
            log::warn(TAG, "NvEncodeAPIGetMaxSupportedVersion failed: %d, using compiled version", (int)st);
            api_version_ = NVENCAPI_VERSION;
        }
    } else {
        log::warn(TAG, "NvEncodeAPIGetMaxSupportedVersion not found, using compiled version");
        api_version_ = NVENCAPI_VERSION;
    }

    auto createInstance = (NvEncodeAPICreateInstance_t)
        GetProcAddress(nvenc_dll_, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        log::error(TAG, "NvEncodeAPICreateInstance not found");
        return false;
    }

    auto* api = fn(fn_list_storage_);
    memset(api, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    api->version = ver(NV_ENCODE_API_FUNCTION_LIST_VER);

    NVENCSTATUS st = createInstance(api);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "NvEncodeAPICreateInstance failed: %d", (int)st);
        return false;
    }
    return true;
}

bool NvencEncoder::find_nvidia_device(ID3D11Device* capture_device) {
    // Check if the capture device is already on NVidia (vendor 0x10DE).
    ComPtr<IDXGIDevice> dxgi_dev;
    HRESULT hr = capture_device->QueryInterface(IID_PPV_ARGS(dxgi_dev.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_dev->GetAdapter(adapter.GetAddressOf());
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC desc;
            adapter->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) {
                // Capture device is already on NVidia — use it directly.
                device_ = capture_device;
                capture_device->GetImmediateContext(d3d_context_.GetAddressOf());
                log::info(TAG, "Capture device is on NVidia adapter — zero-copy mode");
                return true;
            }
            char name[128];
            wcstombs(name, desc.Description, sizeof(name));
            log::info(TAG, "Capture device on non-NVidia adapter: %s (vendor 0x%04X)",
                      name, desc.VendorId);
        }
    }

    // Capture is on a different GPU (e.g. Intel iGPU on hybrid laptop).
    // Enumerate adapters to find NVidia and create a dedicated D3D11 device.
    ComPtr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        log::error(TAG, "CreateDXGIFactory1 failed: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIAdapter> nv_adapter;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter> adapter;
        hr = factory->EnumAdapters(i, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        char name[128];
        wcstombs(name, desc.Description, sizeof(name));
        log::info(TAG, "Adapter %u: %s (vendor 0x%04X)", i, name, desc.VendorId);

        if (desc.VendorId == 0x10DE) {
            nv_adapter = adapter;
            log::info(TAG, "Found NVidia adapter: %s", name);
            break;
        }
    }

    if (!nv_adapter) {
        log::error(TAG, "No NVidia adapter found in system");
        return false;
    }

    // Create D3D11 device on the NVidia adapter.
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    hr = D3D11CreateDevice(
        nv_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,  // must be UNKNOWN when specifying adapter
        nullptr, 0,
        feature_levels, _countof(feature_levels),
        D3D11_SDK_VERSION,
        own_device_.GetAddressOf(), nullptr,
        own_context_.GetAddressOf());
    if (FAILED(hr)) {
        log::error(TAG, "D3D11CreateDevice on NVidia adapter failed: 0x%08X", hr);
        return false;
    }

    device_ = own_device_.Get();
    own_device_->GetImmediateContext(d3d_context_.GetAddressOf());
    cross_device_ = true;
    capture_device_ = capture_device;
    capture_device->GetImmediateContext(capture_context_.GetAddressOf());

    log::info(TAG, "Cross-device mode: capture on Intel, NVENC on NVidia");
    return true;
}

bool NvencEncoder::open_session() {
    auto* api = fn(fn_list_storage_);

    log::info(TAG, "device_=%p, featureLevel=0x%x, fnList sizeof=%zu",
              device_, device_ ? device_->GetFeatureLevel() : 0,
              sizeof(NV_ENCODE_API_FUNCTION_LIST));

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version    = ver(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER);
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device     = device_;
    params.apiVersion = api_version_;

    log::info(TAG, "OpenSession: version=0x%08X, sizeof(params)=%zu, apiVersion=0x%02X",
              params.version, sizeof(params), api_version_);

    NVENCSTATUS st = api->nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncOpenEncodeSessionEx failed: %d", (int)st);

        // If cross-device, also try the capture device for diagnostics.
        if (cross_device_ && capture_device_) {
            params.device = capture_device_;
            log::info(TAG, "Retrying with capture device (Intel) for diagnostics...");
            NVENCSTATUS st2 = api->nvEncOpenEncodeSessionEx(&params, &encoder_);
            log::info(TAG, "Capture device result: %d", (int)st2);
            if (st2 == NV_ENC_SUCCESS) {
                // Unexpected: Intel device worked! Use it.
                log::warn(TAG, "NVENC opened on Intel device (unexpected but using it)");
                return true;
            }
        }
        return false;
    }
    log::info(TAG, "NVENC session opened successfully");
    return true;
}

bool NvencEncoder::configure_encoder() {
    auto* api = fn(fn_list_storage_);

    is_hdr_ = (config_.input_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
               config_.input_format == DXGI_FORMAT_R10G10B10A2_UNORM);

    // NVENC path is HEVC-only. Our vendored nvEncodeAPI.h is a HEVC-only subset,
    // so if the caller asked for H.264 we fall back and update the config so the
    // client handshake advertises the actual codec.
    if (config_.codec != VideoCodec::HEVC) {
        log::warn(TAG, "NVENC backend currently supports only HEVC; ignoring --codec=h264 request");
        config_.codec = VideoCodec::HEVC;
    }

    // Get preset config as starting point.
    NV_ENC_PRESET_CONFIG preset_cfg = {};
    preset_cfg.version = ver(NV_ENC_PRESET_CONFIG_VER);
    preset_cfg.presetCfg.version = ver(NV_ENC_CONFIG_VER);

    NVENCSTATUS st = api->nvEncGetEncodePresetConfigEx(
        encoder_, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_cfg);
    if (st != NV_ENC_SUCCESS) {
        const char* e = api->nvEncGetLastErrorString
            ? api->nvEncGetLastErrorString(encoder_) : "";
        log::error(TAG, "nvEncGetEncodePresetConfigEx failed: %d: %s", (int)st, e);
        return false;
    }

    NV_ENC_CONFIG enc_cfg = preset_cfg.presetCfg;

    // Apply our custom settings on top of preset defaults.
    enc_cfg.rcParams.version = ver(NV_ENC_RC_PARAMS_VER);
    enc_cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_cfg.rcParams.averageBitRate  = config_.bitrate_bps;
    enc_cfg.rcParams.maxBitRate      = config_.bitrate_bps;
    enc_cfg.rcParams.vbvBufferSize   = config_.bitrate_bps;
    enc_cfg.rcParams.vbvInitialDelay = enc_cfg.rcParams.vbvBufferSize;
    enc_cfg.rcParams.zeroReorderDelay = 1;
    enc_cfg.gopLength    = config_.idr_period > 0 ? config_.idr_period : UINT32_MAX;
    enc_cfg.frameIntervalP = 1;

    {
        auto& hevc = enc_cfg.encodeCodecConfig.hevcConfig;
        hevc.repeatSPSPPS = 1;
        hevc.idrPeriod    = enc_cfg.gopLength;
        // Continuous intra refresh: every frame carries ~1/fps of the
        // intra macroblocks, so a full picture refresh happens each second
        // without the burst of a traditional IDR. Kills burst-loss on the
        // network and BRC thrash on the encoder.
        hevc.enableIntraRefresh = 1;
        hevc.intraRefreshPeriod = config_.fps;
        hevc.intraRefreshCnt    = config_.fps;
        if (is_hdr_) {
            hevc.pixelBitDepthMinus8 = 2;
            enc_cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
        } else {
            hevc.pixelBitDepthMinus8 = 0;
            enc_cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
        }
    }

    NV_ENC_INITIALIZE_PARAMS init_params = {};
    init_params.version       = ver(NV_ENC_INITIALIZE_PARAMS_VER);
    init_params.encodeGUID    = NV_ENC_CODEC_HEVC_GUID;
    init_params.presetGUID    = NV_ENC_PRESET_P1_GUID;
    init_params.encodeWidth   = config_.width;
    init_params.encodeHeight  = config_.height;
    init_params.darWidth      = config_.width;
    init_params.darHeight     = config_.height;
    init_params.frameRateNum  = config_.fps;
    init_params.frameRateDen  = 1;
    init_params.enablePTD     = 1;
    init_params.tuningInfo    = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_params.encodeConfig  = &enc_cfg;

    log::info(TAG, "Init: %ux%u @ %ufps, initVer=0x%08X, cfgVer=0x%08X, rcVer=0x%08X",
              config_.width, config_.height, config_.fps,
              init_params.version, enc_cfg.version, enc_cfg.rcParams.version);
    log::info(TAG, "rcMode=%d avgBR=%u maxBR=%u vbvBuf=%u gopLen=%u frameInterP=%d",
              (int)enc_cfg.rcParams.rateControlMode,
              enc_cfg.rcParams.averageBitRate, enc_cfg.rcParams.maxBitRate,
              enc_cfg.rcParams.vbvBufferSize, enc_cfg.gopLength,
              enc_cfg.frameIntervalP);
    st = api->nvEncInitializeEncoder(encoder_, &init_params);

    if (st != NV_ENC_SUCCESS) {
        const char* e = api->nvEncGetLastErrorString
            ? api->nvEncGetLastErrorString(encoder_) : "";
        log::error(TAG, "nvEncInitializeEncoder failed: %d: %s", (int)st, e);
        return false;
    }

    init_state_ = { config_.width, config_.height, config_.fps, config_.bitrate_bps };
    return true;
}

bool NvencEncoder::create_io_buffers() {
    auto* api = fn(fn_list_storage_);

    // Create a staging texture that NVENC can register (must have bind flags).
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width  = config_.width;
    desc.Height = config_.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = config_.input_format;
    desc.SampleDesc.Count = 1;
    desc.Usage  = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, staging_texture_.GetAddressOf());
    if (FAILED(hr)) {
        log::error(TAG, "CreateTexture2D (staging) failed: 0x%08X", hr);
        return false;
    }

    // Register the texture with NVENC.
    NV_ENC_BUFFER_FORMAT nvenc_fmt;
    if (is_hdr_) {
        if (config_.input_format == DXGI_FORMAT_R10G10B10A2_UNORM)
            nvenc_fmt = NV_ENC_BUFFER_FORMAT_ARGB10;
        else
            nvenc_fmt = NV_ENC_BUFFER_FORMAT_ARGB10;  // FP16 → treat as 10-bit
    } else {
        nvenc_fmt = NV_ENC_BUFFER_FORMAT_ARGB;
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version        = ver(NV_ENC_REGISTER_RESOURCE_VER);
    reg.resourceType   = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width          = config_.width;
    reg.height         = config_.height;
    reg.resourceToRegister = staging_texture_.Get();
    reg.bufferFormat   = nvenc_fmt;
    reg.bufferUsage    = 0;

    NVENCSTATUS st = api->nvEncRegisterResource(encoder_, &reg);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncRegisterResource failed: %d", (int)st);
        return false;
    }
    registered_resource_ = reg.registeredResource;

    // Create output bitstream buffer.
    NV_ENC_CREATE_BITSTREAM_BUFFER bsb = {};
    bsb.version = ver(NV_ENC_CREATE_BITSTREAM_BUFFER_VER);
    st = api->nvEncCreateBitstreamBuffer(encoder_, &bsb);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncCreateBitstreamBuffer failed: %d", (int)st);
        return false;
    }
    output_bitstream_ = bsb.bitstreamBuffer;

    return true;
}

void NvencEncoder::destroy_io_buffers() {
    auto* api = fn(fn_list_storage_);
    if (!encoder_) return;

    if (registered_resource_) {
        api->nvEncUnregisterResource(encoder_,
            reinterpret_cast<NV_ENC_REGISTERED_PTR>(registered_resource_));
        registered_resource_ = nullptr;
    }
    if (output_bitstream_) {
        api->nvEncDestroyBitstreamBuffer(encoder_, output_bitstream_);
        output_bitstream_ = nullptr;
    }
}

bool NvencEncoder::init(const EncoderConfig& config, ID3D11Device* device) {
    config_ = config;

    if (!load_nvenc()) return false;

    // Find or create D3D11 device on NVidia adapter (handles hybrid GPU).
    if (!find_nvidia_device(device)) return false;

    if (!open_session()) return false;
    if (!configure_encoder()) return false;
    if (!create_io_buffers()) return false;

    // In cross-device mode, create a CPU-readable staging texture on the capture device
    // for reading back captured frames before uploading to the NVidia device.
    if (cross_device_) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width  = config_.width;
        sd.Height = config_.height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = config_.input_format;
        sd.SampleDesc.Count = 1;
        sd.Usage  = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = capture_device_->CreateTexture2D(&sd, nullptr,
                                                       cpu_read_staging_.GetAddressOf());
        if (FAILED(hr)) {
            log::error(TAG, "CreateTexture2D (CPU staging) failed: 0x%08X", hr);
            return false;
        }
    }

    log::info(TAG, "Initialized: %ux%u @ %u fps, %u kbps, HEVC %s%s",
              config_.width, config_.height, config_.fps,
              config_.bitrate_bps / 1000,
              is_hdr_ ? "Main10 (HDR)" : "Main (SDR)",
              cross_device_ ? " [cross-device]" : "");
    // Force first frame to be IDR with SPS/PPS so decoder can start.
    idr_requested_ = true;
    return true;
}

bool NvencEncoder::encode(ID3D11Texture2D* texture, uint64_t pts_us) {
    if (!encoder_) return false;
    auto* api = fn(fn_list_storage_);

    if (cross_device_) {
        // Cross-device path: capture texture (Intel) → CPU → NVENC staging (NVidia).
        // Step 1: Copy capture texture to CPU-readable staging on the capture device.
        capture_context_->CopyResource(cpu_read_staging_.Get(), texture);

        // Step 2: Map CPU staging and read pixel data.
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = capture_context_->Map(cpu_read_staging_.Get(), 0,
                                           D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            log::error(TAG, "Map CPU staging failed: 0x%08X", hr);
            return false;
        }

        // Step 3: Upload to NVENC staging texture on NVidia device.
        // Use UpdateSubresource — copies from CPU memory into GPU texture.
        d3d_context_->UpdateSubresource(staging_texture_.Get(), 0, nullptr,
                                        mapped.pData, mapped.RowPitch, 0);

        capture_context_->Unmap(cpu_read_staging_.Get(), 0);
    } else {
        // Same-device path: direct GPU copy (zero-copy).
        d3d_context_->CopyResource(staging_texture_.Get(), texture);
    }

    // Map registered resource.
    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = ver(NV_ENC_MAP_INPUT_RESOURCE_VER);
    map.registeredResource = reinterpret_cast<NV_ENC_REGISTERED_PTR>(registered_resource_);

    NVENCSTATUS st = api->nvEncMapInputResource(encoder_, &map);
    if (st != NV_ENC_SUCCESS) {
        const char* e = api->nvEncGetLastErrorString ? api->nvEncGetLastErrorString(encoder_) : "";
        log::error(TAG, "nvEncMapInputResource failed: %d: %s", (int)st, e);
        return false;
    }

    // Encode.
    NV_ENC_PIC_PARAMS pic = {};
    pic.version       = ver(NV_ENC_PIC_PARAMS_VER);
    pic.inputWidth    = config_.width;
    pic.inputHeight   = config_.height;
    pic.inputPitch    = 0;  // NVENC figures it out for D3D11
    pic.inputBuffer   = map.mappedResource;
    pic.outputBitstream = output_bitstream_;
    pic.bufferFmt     = map.mappedBufferFmt;
    pic.pictureStruct = 0x01;  // frame
    pic.inputTimeStamp = pts_us;

    if (idr_requested_) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        idr_requested_ = false;
    }

    st = api->nvEncEncodePicture(encoder_, &pic);

    // Unmap regardless of encode result.
    api->nvEncUnmapInputResource(encoder_, map.mappedResource);

    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        log::error(TAG, "nvEncEncodePicture failed: %d", (int)st);
        return false;
    }

    // In synchronous mode, output is ready immediately after nvEncEncodePicture
    // returns NV_ENC_SUCCESS. Lock the bitstream and copy.
    if (st == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock = {};
        lock.version = ver(NV_ENC_LOCK_BITSTREAM_VER);
        lock.outputBitstream = output_bitstream_;

        st = api->nvEncLockBitstream(encoder_, &lock);
        if (st == NV_ENC_SUCCESS) {
            EncodedPacket pkt;
            pkt.data.assign(
                static_cast<uint8_t*>(lock.bitstreamBufferPtr),
                static_cast<uint8_t*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes);
            pkt.pts = lock.outputTimeStamp;
            pkt.keyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                            lock.pictureType == NV_ENC_PIC_TYPE_I);

            output_packets_.push(std::move(pkt));
            api->nvEncUnlockBitstream(encoder_, output_bitstream_);
        } else {
            const char* e = api->nvEncGetLastErrorString
                ? api->nvEncGetLastErrorString(encoder_) : "";
            log::error(TAG, "nvEncLockBitstream failed: %d: %s", (int)st, e);
        }
    }

    return true;
}

bool NvencEncoder::get_packet(EncodedPacket& packet) {
    if (output_packets_.empty()) return false;
    packet = std::move(output_packets_.front());
    output_packets_.pop();
    return true;
}

void NvencEncoder::request_idr() {
    idr_requested_ = true;
}

void NvencEncoder::set_bitrate(uint32_t bitrate_bps) {
    config_.bitrate_bps = bitrate_bps;
    if (!encoder_) return;

    auto* api = fn(fn_list_storage_);

    // Rebuild init params for reconfigure.
    NV_ENC_CONFIG enc_cfg = {};
    enc_cfg.version = ver(NV_ENC_CONFIG_VER);

    NV_ENC_PRESET_CONFIG preset_cfg = {};
    preset_cfg.version = ver(NV_ENC_PRESET_CONFIG_VER);
    preset_cfg.presetCfg.version = ver(NV_ENC_CONFIG_VER);

    api->nvEncGetEncodePresetConfigEx(
        encoder_, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_cfg);

    enc_cfg = preset_cfg.presetCfg;
    enc_cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    enc_cfg.rcParams.averageBitRate  = bitrate_bps;
    enc_cfg.rcParams.maxBitRate      = bitrate_bps;
    enc_cfg.rcParams.vbvBufferSize   = bitrate_bps;
    enc_cfg.rcParams.vbvInitialDelay = enc_cfg.rcParams.vbvBufferSize;
    enc_cfg.rcParams.zeroReorderDelay = 1;
    enc_cfg.gopLength    = config_.idr_period > 0 ? config_.idr_period : UINT32_MAX;
    enc_cfg.frameIntervalP = 1;

    {
        auto& hevc = enc_cfg.encodeCodecConfig.hevcConfig;
        hevc.repeatSPSPPS = 1;
        hevc.idrPeriod    = enc_cfg.gopLength;
        hevc.enableIntraRefresh = 1;
        hevc.intraRefreshPeriod = config_.fps;
        hevc.intraRefreshCnt    = config_.fps;
        if (is_hdr_) {
            hevc.pixelBitDepthMinus8 = 2;
            enc_cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
        } else {
            hevc.pixelBitDepthMinus8 = 0;
            enc_cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
        }
    }

    NV_ENC_INITIALIZE_PARAMS init_params = {};
    init_params.version       = ver(NV_ENC_INITIALIZE_PARAMS_VER);
    init_params.encodeGUID    = NV_ENC_CODEC_HEVC_GUID;
    init_params.presetGUID    = NV_ENC_PRESET_P1_GUID;
    init_params.encodeWidth   = config_.width;
    init_params.encodeHeight  = config_.height;
    init_params.darWidth      = config_.width;
    init_params.darHeight     = config_.height;
    init_params.frameRateNum  = config_.fps;
    init_params.frameRateDen  = 1;
    init_params.enablePTD     = 1;
    init_params.tuningInfo    = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init_params.encodeConfig  = &enc_cfg;

    NV_ENC_RECONFIGURE_PARAMS reconf = {};
    reconf.version = ver(NV_ENC_RECONFIGURE_PARAMS_VER);
    reconf.reInitEncodeParams = init_params;

    NVENCSTATUS st = api->nvEncReconfigureEncoder(encoder_, &reconf);
    if (st != NV_ENC_SUCCESS) {
        log::warn(TAG, "nvEncReconfigureEncoder failed: %d (bitrate change ignored)", (int)st);
    }
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
