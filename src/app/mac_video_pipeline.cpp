#ifdef VIVORA_MACOS

#include "app/mac_video_pipeline.h"
#include "client/render/mac_video_view.h"

namespace vivora {

bool MacVideoPipeline::init_decoder(VideoCodec codec) {
    if (!view_) return false;
    view_->set_codec(codec);
    return true;  // the layer builds its decoder from the first keyframe
}

SubmitStatus MacVideoPipeline::submit(const uint8_t* data, size_t len,
                                      uint32_t timestamp, bool keyframe,
                                      uint16_t seq_no) {
    if (!view_) return SubmitStatus::Rejected;
    // false = dropped (no format description yet on a non-keyframe, or a
    // CoreMedia failure) → the generic loop runs the reinit+IDR recovery,
    // same as the serial path treats a decode() false.
    if (!view_->submit_frame(data, len, timestamp, keyframe)) {
        return SubmitStatus::Rejected;
    }
    produced_ = true;
    last_seq_ = seq_no;
    return SubmitStatus::Accepted;
}

PollStatus MacVideoPipeline::poll_frame(FrameHandle& out) {
    // One synthetic handle per accepted submit — the frame itself is already
    // queued for display inside the layer (fused pipeline, see header).
    if (!produced_) return PollStatus::Empty;
    produced_ = false;
    out.id  = 0;
    out.seq = last_seq_;
    return PollStatus::Produced;
}

void MacVideoPipeline::flush_decoder() {
    if (view_) view_->flush_decoder();
}

bool MacVideoPipeline::reinit_decoder() {
    // No separate decoder object to recreate — a flush (drop queued samples +
    // cached parameter sets) is the hard reset; the next keyframe rebuilds
    // the format description from its VPS/SPS/PPS.
    if (view_) view_->flush_decoder();
    return view_ != nullptr;
}

} // namespace vivora

#endif // VIVORA_MACOS
