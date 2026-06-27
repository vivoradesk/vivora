#pragma once

#include "common/codec/video_codec.h"
#include <cstddef>
#include <cstdint>

namespace vivora {

// Platform-specific decode→present data path for the threaded client
// pipeline (VIV-81).  The GENERIC pipeline owns the threads, the lock-free
// queues (Q1 net→decode, Q2 decode→render), and the send funnel; this
// interface is the thin, platform-specific slice it drives:
//
//   net thread    →  [Q1]  →  decode thread          →  [Q2]  →  main/GL thread
//                            submit()/poll_frame()             present()
//
// Each platform implements it over its native decoder + frame storage:
//   Linux : FFmpeg — VAAPI surface (zero-copy via DMA-BUF/EGLImage is the
//           target; today's VAAPI→CPU-YUV readback is a copy, to be removed)
//   Windows: Media Foundation — ID3D11Texture2D
//   macOS : VideoToolbox — CVPixelBuffer
// The cross-thread GPU detail (D3D11 fence/keyed-mutex, CVPixelBuffer hand-off)
// is hidden INSIDE the impl — the generic pipeline only ever moves opaque
// FrameHandles between threads, never the pixels.
//
// ZERO-COPY CONTRACT (project rule: the frame stays a GPU texture from decode
// to render):
//   * A FrameHandle's pool slot holds a *reference* to a decoder-owned GPU
//     surface (a refcount bump / ComPtr / CVPixelBufferRetain), NOT a pixel
//     copy.  present() draws straight from that surface.
//   * HW decoders output into their own fixed surface pool, so an in-flight
//     FrameHandle ties up one decoder surface.  In-flight handles are bounded
//     and small (Q2 depth 3 + the one being presented ≈ 4), so the impl MUST
//     allocate that many EXTRA output surfaces beyond the DPB, and recycle()
//     MUST return the surface ref promptly or the decoder stalls.
//   * The slot "pool" is just the bookkeeping array; the memory is the
//     decoder's surfaces.
//
// THREAD AFFINITY is part of the contract and is annotated on every method.
// Violating it is a data race — the impl may assume it.

// Opaque token for one decoded frame living in the impl's frame pool.
// Carried through Q2 by value (trivially copyable).  The impl maps id → its
// native frame; the generic pipeline treats it as a cookie.
struct FrameHandle {
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    uint32_t id = kInvalid;
    bool valid() const { return id != kInvalid; }
};

// Result of feeding one compressed frame to the decoder.
enum class SubmitStatus {
    Accepted,   // fed OK; decoded output (if any) comes via poll_frame()
    Rejected,   // decode error / decoder tainted → caller drops to the
                // no-artifact recovery: reset stream, request IDR, reinit().
};

// Result of pulling a decoded frame.
enum class PollStatus {
    Produced,   // `out` is a valid handle to a decoded frame
    Empty,      // decoder has nothing ready this call
    PoolFull,   // decoder HAS a frame, but every slot is in flight — the
                // surface stays held by the decoder; retry after render
                // recycles.  Distinct from Empty so the generic loop can
                // stop pulling and let the render side catch up instead of
                // busy-spinning.
};

class IVideoPipeline {
public:
    virtual ~IVideoPipeline() = default;

    // ---- setup (main thread, before threads start) -----------------------

    // Lazily create the decoder for the negotiated codec (known only after
    // the handshake).  Idempotent for the same codec.
    virtual bool init_decoder(VideoCodec codec) = 0;

    // ---- decode thread ---------------------------------------------------

    // Feed one assembled compressed frame.  Keyframe-gating and IDR-recovery
    // policy live in the generic decode loop; this only does the codec work
    // and reports whether the decoder is still healthy.
    virtual SubmitStatus submit(const uint8_t* data, size_t len,
                                uint32_t timestamp, bool keyframe,
                                uint16_t seq_no) = 0;

    // Pull the next decoded frame: ref the decoder's output surface into a
    // free pool slot and hand back its handle (zero-copy — no pixel copy).
    // May yield >1 frame per submit; call in a loop while it returns
    // Produced.  Empty = nothing ready; PoolFull = a frame exists but no
    // slot free (back-pressure, retry after recycle).
    virtual PollStatus poll_frame(FrameHandle& out) = 0;

    // Drop the decoder's reference pictures (after detected loss, before the
    // next IDR).  Cheap DPB reset.
    virtual void flush_decoder() = 0;

    // Hard reset: recreate the decoder from scratch.  Used at IDR boundaries
    // for codecs that leak state across flushes (libav 4.4 HEVC), and as the
    // hook for HW→SW auto-fallback after repeated HW failures (VIV-80).
    virtual bool reinit_decoder() = 0;

    // ---- main / GL thread ------------------------------------------------

    // Draw the decoded frame referenced by `h`.  Runs on the thread that
    // owns the render surface (Qt GL widget / D3D swap chain / Cocoa layer).
    virtual void present(FrameHandle h) = 0;

    // ---- any thread ------------------------------------------------------

    // Return a pool slot to the free list.  Called from the main thread for
    // both presented and policy-dropped frames; the free list is SPSC
    // (decode acquires in poll_frame, main releases here).
    virtual void recycle(FrameHandle h) = 0;

    // Drop a just-produced frame from the DECODE thread WITHOUT touching the
    // free list (which is the main thread's to produce).  Re-reserves the slot
    // so the next poll_frame reuses it.  Used when Q2 is full — calling
    // recycle() there made the decode thread a second free-list producer and
    // corrupted the SPSC ring, intermittently deadlocking the pipeline (VIV-82).
    virtual void unreserve(FrameHandle h) { (void)h; }

    // Schedule a render pass on the main thread (decode thread calls this
    // after publishing to Q2).  Impl posts to the main loop:
    // Qt QMetaObject::invokeMethod(...QueuedConnection) on Linux/Windows,
    // dispatch_async(main_queue) on macOS.  The posted handler asks the
    // generic pipeline to drain Q2 (render-penultimate policy) and present.
    virtual void wake_render() = 0;
};

} // namespace vivora
