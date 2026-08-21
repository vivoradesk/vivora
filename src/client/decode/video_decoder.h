// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "common/codec/video_codec.h"
#include "common/utils/types.h"
#include <cstdint>
#include <memory>
#include <vector>

#ifdef VIVORA_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace vivora {

struct DecodedFrame {
#ifdef VIVORA_WINDOWS
    // GPU texture (NV12 or P010).  ComPtr holds the reference so the
    // frame self-releases on destruction — fixes prior MfDecoder leak
    // where frames left in the output queue at shutdown left dangling
    // ID3D11Texture2D refs.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    uint32_t subresource = 0;  // texture array index
#endif
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts = 0;
    bool keyframe = false;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Initialize decoder for the given codec. If device is null, decoder
    // creates its own.
    virtual bool init(VideoCodec codec, ID3D11Device* device = nullptr) = 0;

    // Feed encoded data (HEVC or H.264 NAL units). May buffer internally.
    virtual bool decode(const uint8_t* data, size_t len, uint64_t pts) = 0;

    // Retrieve decoded frames. Returns false when no more available.
    virtual bool get_frame(DecodedFrame& frame) = 0;

    // Flush decoder state — discard buffered frames and reset internal
    // reference pictures. Call before feeding a new IDR after frame loss.
    virtual void flush() = 0;

    // Get the D3D11 device used by the decoder (for renderer sharing).
    virtual ID3D11Device* get_device() = 0;

    // Factory
    static std::unique_ptr<IVideoDecoder> create();
};

} // namespace vivora
