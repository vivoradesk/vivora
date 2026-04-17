#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include "host/encode/amf_encoder.h"
#include "host/encode/nvenc_encoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace deskbeam {

std::unique_ptr<IVideoEncoder> IVideoEncoder::create() {
    // Probe AMF (AMD) first.
    HMODULE amf = LoadLibraryA("amfrt64.dll");
    if (amf) {
        FreeLibrary(amf);
        log::info("ENCODE", "AMF runtime found — using AMD encoder");
        return std::make_unique<AmfEncoder>();
    }

    // Probe NVENC (NVIDIA).
    HMODULE nvenc = LoadLibraryA("nvEncodeAPI64.dll");
    if (nvenc) {
        FreeLibrary(nvenc);
        log::info("ENCODE", "NVENC runtime found — using NVIDIA encoder");
        return std::make_unique<NvencEncoder>();
    }

    log::error("ENCODE", "No hardware encoder available (neither AMF nor NVENC)");
    return nullptr;
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
