#pragma once

#include "common/utils/types.h"
#include <cstdint>
#include <memory>
#include <vector>

#ifdef DESKBEAM_WINDOWS
struct ID3D11Device;
struct ID3D11Texture2D;
#endif

namespace deskbeam {

struct DecodedFrame {
#ifdef DESKBEAM_WINDOWS
    ID3D11Texture2D* texture = nullptr;  // GPU texture (NV12 or P010)
    uint32_t subresource = 0;            // texture array index
#endif
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t pts = 0;
    bool keyframe = false;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Initialize decoder. If device is null, decoder creates its own.
    virtual bool init(ID3D11Device* device = nullptr) = 0;

    // Feed encoded data (HEVC NAL units). May buffer internally.
    virtual bool decode(const uint8_t* data, size_t len, uint64_t pts) = 0;

    // Retrieve decoded frames. Returns false when no more available.
    virtual bool get_frame(DecodedFrame& frame) = 0;

    // Get the D3D11 device used by the decoder (for renderer sharing).
    virtual ID3D11Device* get_device() = 0;

    // Factory
    static std::unique_ptr<IVideoDecoder> create();
};

} // namespace deskbeam
