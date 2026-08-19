#ifdef VIVORA_LINUX

#include "host/encode/linux_encoder.h"
#include "host/encode/vaapi_encoder.h"
#include "host/encode/nvenc_linux_encoder.h"
#include "common/utils/log.h"

namespace vivora::host {

namespace {

std::unique_ptr<ILinuxEncoder> try_nvenc(const ILinuxEncoder::Config& cfg) {
    auto e = std::make_unique<NvencLinuxEncoder>();
    if (e->init(cfg)) return e;
    return nullptr;
}

std::unique_ptr<ILinuxEncoder> try_vaapi(const ILinuxEncoder::Config& cfg) {
    auto e = std::make_unique<VaapiEncoder>();
    if (e->init(cfg)) return e;
    return nullptr;
}

} // namespace

std::unique_ptr<ILinuxEncoder> create_linux_encoder(EncoderKind kind,
                                                     const ILinuxEncoder::Config& cfg) {
    // NVENC forced: try it, fall back to VAAPI if the device/driver isn't
    // there so the host still works on non-NVIDIA machines.
    if (kind == EncoderKind::Nvenc) {
        if (auto e = try_nvenc(cfg)) {
            log::info("ENC", "Using NVENC (forced)");
            return e;
        }
        log::warn("ENC", "NVENC requested but unavailable — falling back to VAAPI");
        return try_vaapi(cfg);
    }

    // Auto: prefer NVENC on NVIDIA (discrete GPU, lowest latency); the cheap
    // probe avoids a full init attempt on machines without the driver.
    if (kind == EncoderKind::Auto) {
        if (NvencLinuxEncoder::is_available()) {
            if (auto e = try_nvenc(cfg)) {
                log::info("ENC", "Using NVENC (auto-selected over VAAPI)");
                return e;
            }
            log::warn("ENC", "NVENC probe passed but init failed — falling back to VAAPI");
        }
        return try_vaapi(cfg);
    }

    // Vaapi forced, or a Windows-only kind (Amf / Qsv) that reached a Linux
    // host through a shared config — VAAPI is the only other option here.
    if (kind != EncoderKind::Vaapi)
        log::info("ENC", "Requested encoder is not a Linux backend — using VAAPI");
    return try_vaapi(cfg);
}

} // namespace vivora::host

#endif // VIVORA_LINUX
