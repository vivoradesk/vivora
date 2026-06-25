#pragma once

#include "common/utils/types.h"
#include "common/codec/video_codec.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>

#ifdef VIVORA_WINDOWS
#include <dxgiformat.h>
#endif
// Forward declarations (incomplete types are fine on all platforms; only
// Windows implementations actually deref these).
struct ID3D11Device;
struct ID3D11Texture2D;

namespace vivora {

// Which hardware encoder backend to use. `Auto` probes available runtimes.
enum class EncoderKind : uint8_t {
    Auto = 0,
    Amf,     // AMD
    Nvenc,   // NVIDIA
    Qsv,     // Intel Quick Sync (oneVPL)
};

struct EncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate_bps = 15'000'000;  // 15 Mbps default
    uint32_t idr_period = 120;          // IDR every N frames (0 = auto)
    uint32_t num_slices = 1;            // slices per frame (>1: burst-loss
                                        // localization + parallel decode, VIV-82)
    bool low_latency = true;
    VideoCodec codec = VideoCodec::HEVC;
#ifdef VIVORA_WINDOWS
    DXGI_FORMAT input_format = DXGI_FORMAT_B8G8R8A8_UNORM;  // capture texture format
#endif
};

struct EncodedPacket {
    std::vector<uint8_t> data;          // H.264 NAL units
    uint64_t pts = 0;                   // presentation timestamp (microseconds)
    bool keyframe = false;
    bool heartbeat = false;             // emitted by encode_skip (no FEC needed)
    double encode_time_ms = 0.0;        // how long encoding took
};

// Platform-independent video encoder interface
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Initialize encoder. Device is the D3D11 device used for capture (zero-copy).
    virtual bool init(const EncoderConfig& config, ID3D11Device* device) = 0;

    // Encode a GPU texture. Returns false if encoder couldn't accept input.
    // The texture must remain valid until the next encode() call.
    virtual bool encode(ID3D11Texture2D* texture, uint64_t pts_us) = 0;

    // Encode a GPU texture with a hint that this is a static-screen
    // heartbeat: backends that support it (AMF) emit a tiny skip-type
    // frame instead of a full intra-refresh slice. Default falls back
    // to encode().
    virtual bool encode_skip(ID3D11Texture2D* texture, uint64_t pts_us) {
        return encode(texture, pts_us);
    }

    // Retrieve encoded packets. May return 0 or more packets per encode() call.
    // Returns false when no more packets are available.
    virtual bool get_packet(EncodedPacket& packet) = 0;

    // Request an IDR (keyframe) on the next encode
    virtual void request_idr() = 0;

    // Dynamically change bitrate without reinit
    virtual void set_bitrate(uint32_t bitrate_bps) = 0;

    // Get current config
    virtual const EncoderConfig& get_config() const = 0;

    // Factory: create best available encoder for this system.
    // `kind=Auto` probes runtimes in order (AMF -> NVENC -> QSV). Any
    // other value forces the specific backend; returns nullptr if the
    // requested backend is not available on this machine.
    static std::unique_ptr<IVideoEncoder> create(EncoderKind kind = EncoderKind::Auto);
};

} // namespace vivora
