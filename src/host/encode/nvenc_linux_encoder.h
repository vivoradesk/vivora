// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef VIVORA_LINUX

#include "host/encode/linux_encoder.h"

#include <cstdint>
#include <queue>
#include <vector>

namespace vivora::host {

// Native NVENC encoder for Linux + NVIDIA hosts (VIV-8).  Unlike the Windows
// NVENC path (which registers D3D11 textures) this opens a CUDA-device NVENC
// session and feeds CPU BGRx frames through an NVENC-allocated host-visible
// input buffer — matching the PipeWire SHM capture path.  The NVIDIA driver
// libraries (libcuda, libnvidia-encode) are loaded at runtime via dlopen, so
// the binary still links and runs on machines without an NVIDIA GPU (the
// factory falls back to VAAPI there).
//
// Supports HEVC Main and H.264 High, SDR 8-bit only (PipeWire delivers BGRx).
class NvencLinuxEncoder : public ILinuxEncoder {
public:
    NvencLinuxEncoder();
    ~NvencLinuxEncoder() override;

    NvencLinuxEncoder(const NvencLinuxEncoder&) = delete;
    NvencLinuxEncoder& operator=(const NvencLinuxEncoder&) = delete;

    // Cheap runtime probe: can we load the driver libs, create a CUDA
    // context and open an NVENC session?  Used by the factory before
    // committing to NVENC over VAAPI.
    static bool is_available();

    bool init(const Config& cfg) override;
    void shutdown() override;
    bool encode_bgrx(const uint8_t* bgrx_data, int stride, uint64_t pts_us) override;
    bool reencode_last(uint64_t pts_us) override;
    bool get_packet(Packet& out) override;
    void request_idr() override { idr_requested_ = true; }
    void set_bitrate(int bps) override;
    int  width()  const override { return cfg_.width; }
    int  height() const override { return cfg_.height; }
    const char* backend_name() const override { return "NVENC"; }

private:
    bool load_libs();
    bool create_cuda_context();
    bool open_session();
    bool configure_encoder();
    bool create_io_buffers();
    void destroy_io_buffers();
    bool submit_picture(uint64_t pts_us);   // shared by encode_bgrx + reencode_last
    uint32_t ver(uint32_t compiled_ver) const;

    Config cfg_{};
    bool idr_requested_   = false;
    bool have_last_frame_ = false;

    // dlopen handles + resolved entry points are stashed in the .cpp via
    // an opaque pimpl-ish set of void*; keep raw here to avoid leaking the
    // NVENC / CUDA headers into this public header.
    void* cuda_lib_  = nullptr;
    void* nvenc_lib_ = nullptr;
    void* cu_ctx_    = nullptr;   // CUcontext

    uint32_t api_version_ = 0;
    void*    encoder_ = nullptr;
    // Backing store for NV_ENCODE_API_FUNCTION_LIST (filled by the driver).
    void*    fn_list_storage_[512] = {};

    void* input_buffer_     = nullptr;   // NV_ENC_INPUT_PTR
    void* output_bitstream_ = nullptr;   // NV_ENC_OUTPUT_PTR
    int   input_pitch_      = 0;         // row pitch of input_buffer_ (from lock)

    std::queue<Packet> output_packets_;
};

} // namespace vivora::host

#endif // VIVORA_LINUX
