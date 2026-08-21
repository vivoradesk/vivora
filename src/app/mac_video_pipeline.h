// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#ifdef VIVORA_MACOS

#include "app/video_pipeline.h"

#include <cstdint>

namespace vivora {

class MacVideoView;

// macOS IVideoPipeline (VIV-84): a "fused" decode+present pipeline over the
// existing MacVideoView / AVSampleBufferVideoRenderer.
//
// On macOS the renderer layer already IS the decoder: submit_frame() wraps the
// compressed frame in a CMSampleBuffer and enqueues it; VideoToolbox decodes
// asynchronously and the layer displays the CVPixelBuffer zero-copy.  There is
// no decoded-frame object to carry across Q2 — decode and present cannot be
// split.  What the threaded pipeline buys here is moving the per-frame CPU
// work (Annex-B parse, AVCC repack, CMSampleBuffer creation, enqueue) off the
// main thread, so the event pump / render tick never stalls on it.
//
// Mapping onto the generic contract:
//   * submit()      — decode thread: full parse+enqueue.  The frame is now on
//                     its way to the screen regardless of what Q2 does.
//   * poll_frame()  — returns exactly one synthetic handle per accepted
//                     submit, so the generic loop's fps accounting, Q2 flow
//                     and FTRACE stay meaningful.  No pool → never PoolFull.
//   * present()     — no-op (the layer already displayed the frame); the Q2
//                     drain just drives the counters.
//   * flush/reinit  — [renderer flush] + drop cached parameter sets; the next
//                     keyframe rebuilds the format description.  Thread-safe
//                     (AVSampleBufferVideoRenderer is; no AppKit calls).
class MacVideoPipeline : public IVideoPipeline {
public:
    // `view` is owned by the platform; enqueue/flush are thread-safe, so the
    // decode thread may call into it directly.
    explicit MacVideoPipeline(MacVideoView* view) : view_(view) {}

    bool init_decoder(VideoCodec codec) override;            // main, pre-threads
    SubmitStatus submit(const uint8_t* data, size_t len,
                        uint32_t timestamp, bool keyframe,
                        uint16_t seq_no) override;            // decode thread
    PollStatus poll_frame(FrameHandle& out) override;         // decode thread
    void flush_decoder() override;                            // decode thread
    bool reinit_decoder() override;                           // decode thread
    void present(FrameHandle) override {}                     // main (no-op, fused)
    void recycle(FrameHandle) override {}                     // main (no pool)
    void wake_render() override {}                            // poll-in-iter model

private:
    MacVideoView* view_;
    // Set by submit(), consumed by the poll_frame() the generic loop runs
    // right after — both on the decode thread.
    bool     produced_ = false;
    uint16_t last_seq_ = 0;
};

} // namespace vivora

#endif // VIVORA_MACOS
