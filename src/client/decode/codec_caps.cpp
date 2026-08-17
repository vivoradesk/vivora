#include "client/decode/codec_caps.h"

#include "common/codec/video_codec.h"
#include "common/utils/log.h"

#if defined(VIVORA_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#elif defined(VIVORA_LINUX)
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace vivora::client {

namespace {

#if defined(VIVORA_WINDOWS)
// True if Media Foundation exposes a decoder MFT for `subtype`.  Mirrors the
// enumeration MfDecoder::init() does, minus the D3D device + activation — a
// non-zero count is a reliable "this codec is decodable on this machine" signal
// (e.g. HEVC returns 0 when the HEVC Video Extension isn't installed).
bool mf_can_decode(const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO input_info = { MFMediaType_Video, subtype };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &input_info, nullptr, &activates, &count);
    if (activates) {
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
    }
    return SUCCEEDED(hr) && count > 0;
}

uint8_t probe_platform() {
    // MFStartup is refcounted and cheap; pair it with a shutdown.  We
    // deliberately avoid CoInitialize here — MFTEnumEx works without an
    // apartment, and the real decoder path does its own COM init.
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        return 0;  // caller substitutes the safe default
    uint8_t caps = 0;
    if (mf_can_decode(MFVideoFormat_H264)) caps |= CODEC_CAP_H264;
    if (mf_can_decode(MFVideoFormat_HEVC)) caps |= CODEC_CAP_HEVC;
    MFShutdown();
    return caps;
}
#elif defined(VIVORA_LINUX)
uint8_t probe_platform() {
    uint8_t caps = 0;
    if (avcodec_find_decoder(AV_CODEC_ID_H264)) caps |= CODEC_CAP_H264;
    if (avcodec_find_decoder(AV_CODEC_ID_HEVC)) caps |= CODEC_CAP_HEVC;
    return caps;
}
#else  // macOS / other: VideoToolbox decodes both on every supported Mac.
uint8_t probe_platform() {
    return CODEC_CAP_H264 | CODEC_CAP_HEVC;
}
#endif

} // namespace

uint8_t probe_decode_caps() {
    static const uint8_t cached = [] {
        uint8_t caps = probe_platform();
        if (caps == 0) {
            // Probe failed or found nothing — don't strand the client with no
            // codec.  Fall back to the pre-VIV-112 assumption (both decodable);
            // the runtime fallback still backstops a genuine decode failure.
            log::warn("DECODE",
                      "Codec-capability probe found nothing — assuming H.264+HEVC");
            caps = CODEC_CAP_ALL_KNOWN;
        }
        log::info("DECODE", "Decoder capabilities: H.264=%s HEVC=%s",
                  (caps & CODEC_CAP_H264) ? "yes" : "no",
                  (caps & CODEC_CAP_HEVC) ? "yes" : "no");
        return caps;
    }();
    return cached;
}

} // namespace vivora::client
