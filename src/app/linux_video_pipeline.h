#pragma once
#ifdef VIVORA_LINUX

#include "app/video_pipeline.h"
#include "client/decode/ffmpeg_decoder.h"
#include "common/utils/spsc_ring.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vivora::client { class QtGlVideoView; }

namespace vivora {

// Linux IVideoPipeline (VIV-81): FFmpeg decode on the decode thread, present
// through the existing QtGlVideoView on the main/GL thread.
//
// Today's Linux decode path is CPU-YUV (VAAPI readback / SW), so a decoded
// frame is copied into a pooled YUV slot that crosses to the main thread via
// Q2.  (Zero-copy DMA-BUF/EGLImage is the eventual replacement; the interface
// already allows it.)  The slot free-list is an SPSC ring: the decode thread
// acquires in poll_frame(), the main thread releases in recycle().
class LinuxVideoPipeline : public IVideoPipeline {
public:
    // `view` is owned by the platform and lives on the main thread; present()
    // and recycle() are only ever called there.
    explicit LinuxVideoPipeline(client::QtGlVideoView* view);
    ~LinuxVideoPipeline() override;

    bool init_decoder(VideoCodec codec) override;          // main, pre-threads
    SubmitStatus submit(const uint8_t* data, size_t len,
                        uint32_t timestamp, bool keyframe,
                        uint16_t seq_no) override;          // decode thread
    PollStatus poll_frame(FrameHandle& out) override;       // decode thread
    void flush_decoder() override;                          // decode thread
    bool reinit_decoder() override;                         // decode thread
    void present(FrameHandle h) override;                   // main thread
    void recycle(FrameHandle h) override;                   // main thread
    void unreserve(FrameHandle h) override;                 // decode thread (q2-full)
    void wake_render() override {}                          // poll-in-iter model

private:
    static constexpr int kSlots = 5;   // Q2 depth (3) + present (1) + margin
    struct Slot {
        std::vector<uint8_t> y, u, v;
        int      ys = 0, us = 0, vs = 0;
        uint32_t w = 0, h = 0;
        bool     hdr = false;  // stashed in poll_frame so present() needn't
                               // touch the decoder from the main thread
    };

    client::QtGlVideoView*                  view_;
    std::unique_ptr<client::FfmpegDecoder>  dec_;
    Slot                                    slots_[kSlots];
    // Free slot indices.  Strict SPSC: the ONLY producer is the main thread
    // (recycle), the ONLY consumer is the decode thread (poll_frame).
    util::SpscRing<uint32_t, 8>             free_;
    bool                                    free_primed_ = false;
    // A slot the decode thread has popped but not yet filled.  Held across
    // empty polls so we never push back into free_ from the decode thread
    // (that would make it a second producer and corrupt the SPSC ring).
    uint32_t                                reserved_idx_ = FrameHandle::kInvalid;
};

} // namespace vivora

#endif // VIVORA_LINUX
