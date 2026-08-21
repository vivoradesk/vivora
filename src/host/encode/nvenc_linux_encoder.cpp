// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_LINUX

#include "host/encode/nvenc_linux_encoder.h"
#include "common/utils/log.h"

#include <cstring>
#include <dlfcn.h>

#include "nvEncodeAPI.h"

namespace vivora::host {

static const char* TAG = "NVENC";

namespace {

// ---- Minimal CUDA Driver API (resolved via dlsym from libcuda) -----------
// We only need enough to spin up a context for NVENC; no CUDA toolkit header
// is required.  CUDA_SUCCESS == 0.
typedef int   CUresult;
typedef int   CUdevice;
typedef void* CUcontext;

typedef CUresult (*cuInit_t)(unsigned int);
typedef CUresult (*cuDeviceGetCount_t)(int*);
typedef CUresult (*cuDeviceGet_t)(CUdevice*, int);
typedef CUresult (*cuCtxCreate_t)(CUcontext*, unsigned int, CUdevice);
typedef CUresult (*cuCtxDestroy_t)(CUcontext);

// Helper to view the function-list backing store as the real type.
NV_ENCODE_API_FUNCTION_LIST* fn(void* storage) {
    return reinterpret_cast<NV_ENCODE_API_FUNCTION_LIST*>(storage);
}

bool is_h264(VideoCodec c) { return c == VideoCodec::H264; }

} // namespace

NvencLinuxEncoder::NvencLinuxEncoder() = default;

NvencLinuxEncoder::~NvencLinuxEncoder() {
    shutdown();
}

// Patch the API version into a compiled struct version while keeping the
// struct-version number, magic and bit31 flag — identical scheme to the
// Windows encoder so the driver accepts our structs.
uint32_t NvencLinuxEncoder::ver(uint32_t compiled_ver) const {
    return (compiled_ver & 0xF0FF0000) | (api_version_ & 0x0F00FFFF);
}

bool NvencLinuxEncoder::load_libs() {
    // libcuda for the device context, libnvidia-encode for NVENC itself.
    // Both ship with the proprietary NVIDIA driver; absence → no NVENC.
    cuda_lib_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!cuda_lib_) cuda_lib_ = dlopen("libcuda.so", RTLD_NOW | RTLD_GLOBAL);
    if (!cuda_lib_) {
        log::info(TAG, "libcuda not present (%s) — NVENC unavailable", dlerror());
        return false;
    }
    nvenc_lib_ = dlopen("libnvidia-encode.so.1", RTLD_NOW);
    if (!nvenc_lib_) nvenc_lib_ = dlopen("libnvidia-encode.so", RTLD_NOW);
    if (!nvenc_lib_) {
        log::info(TAG, "libnvidia-encode not present (%s) — NVENC unavailable", dlerror());
        return false;
    }

    // Resolve the NVENC API version + function list.
    auto getMaxVer = reinterpret_cast<NvEncodeAPIGetMaxSupportedVersion_t>(
        dlsym(nvenc_lib_, "NvEncodeAPIGetMaxSupportedVersion"));
    if (getMaxVer) {
        uint32_t max_ver = 0;
        if (getMaxVer(&max_ver) == NV_ENC_SUCCESS) {
            uint32_t major = (max_ver >> 4) & 0xFFF;
            uint32_t minor = max_ver & 0xF;
            log::info(TAG, "Driver supports NVENC API v%u.%u (compiled v%u.%u)",
                      major, minor, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            api_version_ = major | (minor << 24);
        } else {
            api_version_ = NVENCAPI_VERSION;
        }
    } else {
        api_version_ = NVENCAPI_VERSION;
    }

    auto createInstance = reinterpret_cast<NvEncodeAPICreateInstance_t>(
        dlsym(nvenc_lib_, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        log::error(TAG, "NvEncodeAPICreateInstance not found in libnvidia-encode");
        return false;
    }
    auto* api = fn(fn_list_storage_);
    std::memset(api, 0, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    api->version = ver(NV_ENCODE_API_FUNCTION_LIST_VER);
    NVENCSTATUS st = createInstance(api);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "NvEncodeAPICreateInstance failed: %d", (int)st);
        return false;
    }
    return true;
}

bool NvencLinuxEncoder::create_cuda_context() {
    auto cuInit         = reinterpret_cast<cuInit_t>(dlsym(cuda_lib_, "cuInit"));
    auto cuDeviceGetCnt = reinterpret_cast<cuDeviceGetCount_t>(dlsym(cuda_lib_, "cuDeviceGetCount"));
    auto cuDeviceGet    = reinterpret_cast<cuDeviceGet_t>(dlsym(cuda_lib_, "cuDeviceGet"));
    // cuCtxCreate is versioned in the driver ABI — the exported symbol is _v2.
    auto cuCtxCreate    = reinterpret_cast<cuCtxCreate_t>(dlsym(cuda_lib_, "cuCtxCreate_v2"));
    if (!cuInit || !cuDeviceGetCnt || !cuDeviceGet || !cuCtxCreate) {
        log::error(TAG, "CUDA driver entry points missing");
        return false;
    }

    if (cuInit(0) != 0) {
        log::info(TAG, "cuInit failed — no usable NVIDIA device");
        return false;
    }
    int count = 0;
    if (cuDeviceGetCnt(&count) != 0 || count <= 0) {
        log::info(TAG, "no CUDA devices");
        return false;
    }
    CUdevice dev = 0;
    if (cuDeviceGet(&dev, 0) != 0) {
        log::error(TAG, "cuDeviceGet(0) failed");
        return false;
    }
    CUcontext ctx = nullptr;
    // CU_CTX_SCHED_BLOCKING_SYNC = 0x04 — encode calls are synchronous.
    if (cuCtxCreate(&ctx, 0x04, dev) != 0 || !ctx) {
        log::error(TAG, "cuCtxCreate failed");
        return false;
    }
    cu_ctx_ = ctx;
    return true;
}

bool NvencLinuxEncoder::open_session() {
    auto* api = fn(fn_list_storage_);
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version    = ver(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER);
    params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    params.device     = cu_ctx_;
    params.apiVersion = api_version_;
    NVENCSTATUS st = api->nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncOpenEncodeSessionEx failed: %d", (int)st);
        encoder_ = nullptr;
        return false;
    }
    return true;
}

bool NvencLinuxEncoder::configure_encoder() {
    auto* api = fn(fn_list_storage_);
    const bool h264 = is_h264(cfg_.codec);
    const GUID codec_guid = h264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;

    NV_ENC_PRESET_CONFIG preset_cfg = {};
    preset_cfg.version = ver(NV_ENC_PRESET_CONFIG_VER);
    preset_cfg.presetCfg.version = ver(NV_ENC_CONFIG_VER);
    NVENCSTATUS st = api->nvEncGetEncodePresetConfigEx(
        encoder_, codec_guid, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_cfg);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncGetEncodePresetConfigEx failed: %d", (int)st);
        return false;
    }

    NV_ENC_CONFIG enc_cfg = preset_cfg.presetCfg;
    enc_cfg.version = ver(NV_ENC_CONFIG_VER);
    enc_cfg.rcParams.version          = ver(NV_ENC_RC_PARAMS_VER);
    enc_cfg.rcParams.rateControlMode  = NV_ENC_PARAMS_RC_CBR;
    enc_cfg.rcParams.averageBitRate   = cfg_.bitrate_bps;
    enc_cfg.rcParams.maxBitRate       = cfg_.bitrate_bps;
    enc_cfg.rcParams.vbvBufferSize    = cfg_.bitrate_bps;   // 1s of bitrate
    enc_cfg.rcParams.vbvInitialDelay  = cfg_.bitrate_bps;
    enc_cfg.rcParams.zeroReorderDelay = 1;
    enc_cfg.gopLength      = UINT32_MAX;   // intra-refresh, no periodic IDR burst
    enc_cfg.frameIntervalP = 1;            // no B-frames

    if (h264) {
        auto& h = enc_cfg.encodeCodecConfig.h264Config;
        h.repeatSPSPPS       = 1;
        h.idrPeriod          = enc_cfg.gopLength;
        h.enableIntraRefresh = 1;
        h.intraRefreshPeriod = cfg_.fps;   // one full refresh per second
        h.intraRefreshCnt    = cfg_.fps;
        enc_cfg.profileGUID  = NV_ENC_H264_PROFILE_HIGH_GUID;
    } else {
        auto& h = enc_cfg.encodeCodecConfig.hevcConfig;
        h.repeatSPSPPS       = 1;
        h.idrPeriod          = enc_cfg.gopLength;
        h.enableIntraRefresh = 1;
        h.intraRefreshPeriod = cfg_.fps;
        h.intraRefreshCnt    = cfg_.fps;
        h.pixelBitDepthMinus8 = 0;
        enc_cfg.profileGUID  = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    }

    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version       = ver(NV_ENC_INITIALIZE_PARAMS_VER);
    init.encodeGUID    = codec_guid;
    init.presetGUID    = NV_ENC_PRESET_P1_GUID;
    init.encodeWidth   = cfg_.width;
    init.encodeHeight  = cfg_.height;
    init.darWidth      = cfg_.width;
    init.darHeight     = cfg_.height;
    init.frameRateNum  = cfg_.fps;
    init.frameRateDen  = 1;
    init.enablePTD     = 1;
    init.tuningInfo    = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.bufferFormat  = NV_ENC_BUFFER_FORMAT_ARGB;
    init.encodeConfig  = &enc_cfg;

    st = api->nvEncInitializeEncoder(encoder_, &init);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncInitializeEncoder failed: %d", (int)st);
        return false;
    }
    return true;
}

bool NvencLinuxEncoder::create_io_buffers() {
    auto* api = fn(fn_list_storage_);

    NV_ENC_CREATE_INPUT_BUFFER ib = {};
    ib.version   = ver(NV_ENC_CREATE_INPUT_BUFFER_VER);
    ib.width     = cfg_.width;
    ib.height    = cfg_.height;
    ib.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    NVENCSTATUS st = api->nvEncCreateInputBuffer(encoder_, &ib);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncCreateInputBuffer failed: %d", (int)st);
        return false;
    }
    input_buffer_ = ib.inputBuffer;

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

void NvencLinuxEncoder::destroy_io_buffers() {
    auto* api = fn(fn_list_storage_);
    if (!encoder_) return;
    if (input_buffer_) {
        api->nvEncDestroyInputBuffer(encoder_, input_buffer_);
        input_buffer_ = nullptr;
    }
    if (output_bitstream_) {
        api->nvEncDestroyBitstreamBuffer(encoder_, output_bitstream_);
        output_bitstream_ = nullptr;
    }
}

bool NvencLinuxEncoder::is_available() {
    NvencLinuxEncoder e;
    e.cfg_.width = 64; e.cfg_.height = 64; e.cfg_.fps = 30;
    e.cfg_.bitrate_bps = 1'000'000; e.cfg_.codec = VideoCodec::HEVC;
    if (!e.load_libs())           return false;
    if (!e.create_cuda_context()) return false;
    if (!e.open_session())        return false;
    log::info(TAG, "NVENC probe OK — available");
    return true;   // e's destructor tears everything down
}

bool NvencLinuxEncoder::init(const Config& cfg) {
    cfg_ = cfg;
    if (!load_libs())           return false;
    if (!create_cuda_context()) return false;
    if (!open_session())        return false;
    if (!configure_encoder())   return false;
    if (!create_io_buffers())   return false;
    idr_requested_ = true;   // first frame must be IDR with SPS/PPS
    log::info(TAG, "Initialized: %dx%d @ %dfps, %d kbps, %s (Linux/CUDA)",
              cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrate_bps / 1000,
              is_h264(cfg_.codec) ? "H.264 High" : "HEVC Main");
    return true;
}

void NvencLinuxEncoder::shutdown() {
    auto* api = fn(fn_list_storage_);
    if (encoder_) {
        // Flush with an EOS picture before teardown.
        NV_ENC_PIC_PARAMS eos = {};
        eos.version = ver(NV_ENC_PIC_PARAMS_VER);
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        api->nvEncEncodePicture(encoder_, &eos);
        destroy_io_buffers();
        api->nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    if (cu_ctx_ && cuda_lib_) {
        auto cuCtxDestroy = reinterpret_cast<cuCtxDestroy_t>(
            dlsym(cuda_lib_, "cuCtxDestroy_v2"));
        if (cuCtxDestroy) cuCtxDestroy(static_cast<CUcontext>(cu_ctx_));
        cu_ctx_ = nullptr;
    }
    if (nvenc_lib_) { dlclose(nvenc_lib_); nvenc_lib_ = nullptr; }
    if (cuda_lib_)  { dlclose(cuda_lib_);  cuda_lib_  = nullptr; }
    have_last_frame_ = false;
}

bool NvencLinuxEncoder::encode_bgrx(const uint8_t* bgrx_data, int stride, uint64_t pts_us) {
    if (!encoder_ || !input_buffer_ || !bgrx_data) return false;
    auto* api = fn(fn_list_storage_);

    // Lock the NVENC input buffer, copy BGRx rows in (respecting both the
    // source PipeWire stride and NVENC's own pitch), then unlock.
    NV_ENC_LOCK_INPUT_BUFFER lock = {};
    lock.version = ver(NV_ENC_LOCK_INPUT_BUFFER_VER);
    lock.inputBuffer = input_buffer_;
    NVENCSTATUS st = api->nvEncLockInputBuffer(encoder_, &lock);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncLockInputBuffer failed: %d", (int)st);
        return false;
    }
    input_pitch_ = static_cast<int>(lock.pitch);
    const int row_bytes = cfg_.width * 4;
    auto* dst = static_cast<uint8_t*>(lock.bufferDataPtr);
    for (int y = 0; y < cfg_.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * lock.pitch,
                    bgrx_data + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(row_bytes));
    }
    api->nvEncUnlockInputBuffer(encoder_, input_buffer_);
    have_last_frame_ = true;

    return submit_picture(pts_us);
}

bool NvencLinuxEncoder::reencode_last(uint64_t pts_us) {
    // The input buffer still holds the last frame's pixels — just re-submit
    // it with a fresh PTS (no lock / copy).  Cheap static-screen heartbeat.
    if (!encoder_ || !have_last_frame_) return false;
    return submit_picture(pts_us);
}

bool NvencLinuxEncoder::submit_picture(uint64_t pts_us) {
    auto* api = fn(fn_list_storage_);

    NV_ENC_PIC_PARAMS pic = {};
    pic.version        = ver(NV_ENC_PIC_PARAMS_VER);
    pic.inputWidth     = cfg_.width;
    pic.inputHeight    = cfg_.height;
    pic.inputPitch     = static_cast<uint32_t>(input_pitch_);
    pic.inputBuffer    = input_buffer_;
    pic.outputBitstream = output_bitstream_;
    pic.bufferFmt      = NV_ENC_BUFFER_FORMAT_ARGB;
    pic.pictureStruct  = 0x01;   // frame
    pic.inputTimeStamp = pts_us;
    if (idr_requested_) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        idr_requested_ = false;
    }

    NVENCSTATUS st = api->nvEncEncodePicture(encoder_, &pic);
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        log::error(TAG, "nvEncEncodePicture failed: %d", (int)st);
        return false;
    }
    if (st != NV_ENC_SUCCESS) return true;   // buffered (shouldn't happen, ULL sync)

    NV_ENC_LOCK_BITSTREAM lb = {};
    lb.version = ver(NV_ENC_LOCK_BITSTREAM_VER);
    lb.outputBitstream = output_bitstream_;
    st = api->nvEncLockBitstream(encoder_, &lb);
    if (st != NV_ENC_SUCCESS) {
        log::error(TAG, "nvEncLockBitstream failed: %d", (int)st);
        return false;
    }
    Packet pkt;
    pkt.data.assign(static_cast<uint8_t*>(lb.bitstreamBufferPtr),
                    static_cast<uint8_t*>(lb.bitstreamBufferPtr) + lb.bitstreamSizeInBytes);
    pkt.pts_us   = lb.outputTimeStamp;
    pkt.keyframe = (lb.pictureType == NV_ENC_PIC_TYPE_IDR ||
                    lb.pictureType == NV_ENC_PIC_TYPE_I);
    api->nvEncUnlockBitstream(encoder_, output_bitstream_);

    output_packets_.push(std::move(pkt));
    return true;
}

bool NvencLinuxEncoder::get_packet(Packet& out) {
    if (output_packets_.empty()) return false;
    out = std::move(output_packets_.front());
    output_packets_.pop();
    return true;
}

void NvencLinuxEncoder::set_bitrate(int bps) {
    cfg_.bitrate_bps = bps;
    if (!encoder_) return;
    auto* api = fn(fn_list_storage_);
    const bool h264 = is_h264(cfg_.codec);
    const GUID codec_guid = h264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;

    NV_ENC_PRESET_CONFIG preset_cfg = {};
    preset_cfg.version = ver(NV_ENC_PRESET_CONFIG_VER);
    preset_cfg.presetCfg.version = ver(NV_ENC_CONFIG_VER);
    if (api->nvEncGetEncodePresetConfigEx(
            encoder_, codec_guid, NV_ENC_PRESET_P1_GUID,
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_cfg) != NV_ENC_SUCCESS) {
        return;
    }

    NV_ENC_CONFIG enc_cfg = preset_cfg.presetCfg;
    enc_cfg.version = ver(NV_ENC_CONFIG_VER);
    enc_cfg.rcParams.version          = ver(NV_ENC_RC_PARAMS_VER);
    enc_cfg.rcParams.rateControlMode  = NV_ENC_PARAMS_RC_CBR;
    enc_cfg.rcParams.averageBitRate   = bps;
    enc_cfg.rcParams.maxBitRate       = bps;
    enc_cfg.rcParams.vbvBufferSize    = bps;
    enc_cfg.rcParams.vbvInitialDelay  = bps;
    enc_cfg.rcParams.zeroReorderDelay = 1;
    enc_cfg.gopLength      = UINT32_MAX;
    enc_cfg.frameIntervalP = 1;
    if (h264) {
        auto& h = enc_cfg.encodeCodecConfig.h264Config;
        h.repeatSPSPPS = 1; h.idrPeriod = enc_cfg.gopLength;
        h.enableIntraRefresh = 1; h.intraRefreshPeriod = cfg_.fps; h.intraRefreshCnt = cfg_.fps;
        enc_cfg.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    } else {
        auto& h = enc_cfg.encodeCodecConfig.hevcConfig;
        h.repeatSPSPPS = 1; h.idrPeriod = enc_cfg.gopLength;
        h.enableIntraRefresh = 1; h.intraRefreshPeriod = cfg_.fps; h.intraRefreshCnt = cfg_.fps;
        enc_cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    }

    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version      = ver(NV_ENC_INITIALIZE_PARAMS_VER);
    init.encodeGUID   = codec_guid;
    init.presetGUID   = NV_ENC_PRESET_P1_GUID;
    init.encodeWidth  = cfg_.width;  init.encodeHeight = cfg_.height;
    init.darWidth     = cfg_.width;  init.darHeight    = cfg_.height;
    init.frameRateNum = cfg_.fps;    init.frameRateDen = 1;
    init.enablePTD    = 1;
    init.tuningInfo   = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    init.encodeConfig = &enc_cfg;

    NV_ENC_RECONFIGURE_PARAMS reconf = {};
    reconf.version = ver(NV_ENC_RECONFIGURE_PARAMS_VER);
    reconf.reInitEncodeParams = init;
    if (api->nvEncReconfigureEncoder(encoder_, &reconf) != NV_ENC_SUCCESS) {
        log::warn(TAG, "nvEncReconfigureEncoder failed (bitrate change ignored)");
    }
}

} // namespace vivora::host

#endif // VIVORA_LINUX
