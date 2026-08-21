// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#ifdef VIVORA_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app/video_pipeline.h"
#include "client/decode/video_decoder.h"
#include "common/utils/spsc_ring.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

namespace vivora {

class StreamWindow;

// Windows IVideoPipeline (VIV-84): Media Foundation decode on the decode
// thread, present through the existing StreamWindow/D3dRenderer on the main
// thread.
//
// The pipeline owns the D3D11 device (VIDEO_SUPPORT + multithread protection)
// and shares it with both the MF decoder and the renderer, so reinit_decoder()
// can recreate the decoder without invalidating the renderer or the pool.
//
// Frame hand-off: poll_frame() GPU-copies the decoder's output (a slice of
// MF's internal texture-array pool) into a pipeline-owned pool texture.  A
// true zero-ref hand-off (holding the MF texture across Q2) is unsafe here:
// releasing the IMFSample returns the array slice to the MFT's allocator, so
// the decoder may scribble into it while the handle waits in Q2 (the serial
// path never saw this because it presents in the same loop iteration).  The
// copy is GPU→GPU (no CPU touch, ~tens of µs) and keeps MF's pool drained.
class WindowsVideoPipeline : public IVideoPipeline {
public:
    // `window` is owned by the platform and lives on the main thread;
    // present() is only ever called there.
    explicit WindowsVideoPipeline(StreamWindow* window);
    ~WindowsVideoPipeline() override;

    bool init_decoder(VideoCodec codec) override;           // main, pre-threads
    SubmitStatus submit(const uint8_t* data, size_t len,
                        uint32_t timestamp, bool keyframe,
                        uint16_t seq_no) override;           // decode thread
    PollStatus poll_frame(FrameHandle& out) override;        // decode thread
    void flush_decoder() override;                           // decode thread
    bool reinit_decoder() override;                          // decode thread
    void present(FrameHandle h) override;                    // main thread
    void recycle(FrameHandle h) override;                    // main thread
    void unreserve(FrameHandle h) override;                  // decode thread (q2-full)
    void wake_render() override {}                           // poll-in-iter model
    void on_decode_thread_start() override;                  // CoInitializeEx (MF)
    void on_decode_thread_stop() override;

    // Real (pre-encoder-padding) stream dims from StreamInfo — applied right
    // after the lazy renderer init in present(), mirroring the serial path
    // (StreamInfo can arrive before the first decoded frame).  Main thread.
    void set_pending_stream_size(uint32_t w, uint32_t h);

private:
    bool create_device();

    // Same sizing rationale as LinuxVideoPipeline: Q2 capacity (4) +
    // reserved (1) + presenting (1) = 6; 8 gives slack (VIV-82).
    static constexpr int kSlots = 8;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        uint32_t    w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    };

    StreamWindow*                           window_;
    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    std::unique_ptr<IVideoDecoder>          dec_;
    VideoCodec                              codec_ = VideoCodec::HEVC;
    Slot                                    slots_[kSlots];
    // Free slot indices.  Strict SPSC: the ONLY producer is the main thread
    // (recycle), the ONLY consumer is the decode thread (poll_frame).
    util::SpscRing<uint32_t, 16>            free_;  // must hold all kSlots indices
    // A slot the decode thread has popped but not yet filled — held across
    // empty polls so the decode thread never pushes back into free_ (VIV-82).
    uint32_t                                reserved_idx_ = FrameHandle::kInvalid;

    bool renderer_ready_   = false;   // main thread (present)
    uint32_t    renderer_w_   = 0;    // geometry the renderer was built for;
    uint32_t    renderer_h_   = 0;    // a mismatch (monitor switch, VIV-50)
    DXGI_FORMAT renderer_fmt_ = DXGI_FORMAT_UNKNOWN;  // forces a re-init
    bool com_initialized_  = false;   // decode thread
    uint32_t pending_stream_w_ = 0;   // main thread
    uint32_t pending_stream_h_ = 0;
};

} // namespace vivora

#endif // VIVORA_WINDOWS
