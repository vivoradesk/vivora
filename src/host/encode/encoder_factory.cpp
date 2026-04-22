#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include "host/encode/amf_encoder.h"
#include "host/encode/nvenc_encoder.h"
#include "host/encode/qsv_encoder.h"
#include "host/encode/fallback_encoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace deskbeam {

namespace {

bool probe_amf()   { HMODULE h = LoadLibraryA("amfrt64.dll");       if (h) { FreeLibrary(h); return true; } return false; }
bool probe_nvenc() { HMODULE h = LoadLibraryA("nvEncodeAPI64.dll"); if (h) { FreeLibrary(h); return true; } return false; }
bool probe_qsv()   { HMODULE h = LoadLibraryA("libvpl.dll");        if (h) { FreeLibrary(h); return true; } return false; }

// Wrap NVENC in a FallbackEncoder targeting QSV when both are present.
// On hybrid Intel+NVidia laptops NVENC can get stuck in Map-failure state
// for 20-75 seconds when a fullscreen game starts (see
// project_nvenc_hybrid_limitation memory); the wrapper hot-swaps to QSV
// if NVENC returns encode() failures for ~2 seconds.  When QSV is not
// available (pure NVIDIA desktop) we just return plain NVENC.
std::unique_ptr<IVideoEncoder> wrap_nvenc_with_fallback() {
    auto nvenc = std::make_unique<NvencEncoder>();
    if (!probe_qsv()) return nvenc;
    log::info("ENCODE", "Wrapping NVENC with QSV fallback (hybrid-GPU safety)");
    return std::make_unique<FallbackEncoder>(std::move(nvenc), EncoderKind::Qsv);
}

} // anonymous

std::unique_ptr<IVideoEncoder> IVideoEncoder::create(EncoderKind kind) {
    // Explicit backend: create or fail — no silent fallback, caller asked for
    // this specific encoder.
    switch (kind) {
        case EncoderKind::Amf:
            if (!probe_amf()) { log::error("ENCODE", "AMF runtime not found"); return nullptr; }
            log::info("ENCODE", "Using AMF encoder (forced)");
            return std::make_unique<AmfEncoder>();
        case EncoderKind::Nvenc:
            if (!probe_nvenc()) { log::error("ENCODE", "NVENC runtime not found"); return nullptr; }
            log::info("ENCODE", "Using NVENC encoder (forced)");
            return wrap_nvenc_with_fallback();
        case EncoderKind::Qsv:
            if (!probe_qsv()) { log::error("ENCODE", "oneVPL (libvpl.dll) not found"); return nullptr; }
            log::info("ENCODE", "Using QSV encoder (forced)");
            return std::make_unique<QsvEncoder>();
        case EncoderKind::Auto:
        default:
            break;
    }

    // Auto mode: probe in order AMF → NVENC → QSV. AMF/NVENC are discrete
    // GPUs (almost always faster than iGPU). QSV is the iGPU fallback.
    if (probe_amf())   { log::info("ENCODE", "AMF runtime found — using AMD encoder");    return std::make_unique<AmfEncoder>();   }
    if (probe_nvenc()) { log::info("ENCODE", "NVENC runtime found — using NVIDIA encoder"); return wrap_nvenc_with_fallback(); }
    if (probe_qsv())   { log::info("ENCODE", "oneVPL runtime found — using Intel QSV");   return std::make_unique<QsvEncoder>();   }

    log::error("ENCODE", "No hardware encoder available (AMF / NVENC / QSV)");
    return nullptr;
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
