#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include "host/encode/amf_encoder.h"
#include "host/encode/nvenc_encoder.h"
#include "host/encode/qsv_encoder.h"
#include "common/utils/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace deskbeam {

namespace {

bool probe_amf()   { HMODULE h = LoadLibraryA("amfrt64.dll");       if (h) { FreeLibrary(h); return true; } return false; }
bool probe_nvenc() { HMODULE h = LoadLibraryA("nvEncodeAPI64.dll"); if (h) { FreeLibrary(h); return true; } return false; }
bool probe_qsv()   { HMODULE h = LoadLibraryA("libvpl.dll");        if (h) { FreeLibrary(h); return true; } return false; }

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
            return std::make_unique<NvencEncoder>();
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
    if (probe_nvenc()) { log::info("ENCODE", "NVENC runtime found — using NVIDIA encoder"); return std::make_unique<NvencEncoder>(); }
    if (probe_qsv())   { log::info("ENCODE", "oneVPL runtime found — using Intel QSV");   return std::make_unique<QsvEncoder>();   }

    log::error("ENCODE", "No hardware encoder available (AMF / NVENC / QSV)");
    return nullptr;
}

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
