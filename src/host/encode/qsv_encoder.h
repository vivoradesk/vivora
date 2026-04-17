#pragma once

#ifdef DESKBEAM_WINDOWS

#include "host/encode/video_encoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <queue>
#include <vector>

// Forward declarations — oneVPL loaded via dispatcher (libvpl.dll).
struct mfxSession_;
typedef struct mfxSession_* mfxSession;
struct mfxLoader_;
typedef struct mfxLoader_* mfxLoader;
struct mfxFrameSurface1;
struct mfxBitstream;
struct mfxSyncPoint_;
typedef struct mfxSyncPoint_* mfxSyncPoint;

namespace deskbeam {

using Microsoft::WRL::ComPtr;

// Intel Quick Sync encoder via oneVPL. Uses D3D11 surface allocator so
// capture textures stay on the iGPU end-to-end (zero copy).
class QsvEncoder : public IVideoEncoder {
public:
    QsvEncoder();
    ~QsvEncoder() override;

    bool init(const EncoderConfig& config, ID3D11Device* device) override;
    bool encode(ID3D11Texture2D* texture, uint64_t pts_us) override;
    bool get_packet(EncodedPacket& packet) override;
    void request_idr() override;
    void set_bitrate(uint32_t bitrate_bps) override;
    const EncoderConfig& get_config() const override { return config_; }

private:
    bool load_dispatcher();
    bool create_session();
    bool configure_encoder();

    EncoderConfig config_;
    bool idr_requested_ = false;

    // oneVPL dispatcher / session
    void*     vpl_dll_ = nullptr;   // HMODULE as void*, to keep <windows.h> out of header
    mfxLoader loader_  = nullptr;
    mfxSession session_ = nullptr;

    // D3D11 (not owned)
    ID3D11Device* device_ = nullptr;
    ComPtr<ID3D11DeviceContext> d3d_context_;

    // Output
    std::vector<uint8_t> bitstream_buf_;
    std::queue<EncodedPacket> output_packets_;
};

} // namespace deskbeam

#endif // DESKBEAM_WINDOWS
