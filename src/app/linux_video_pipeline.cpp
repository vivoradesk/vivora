#ifdef VIVORA_LINUX

#include "app/linux_video_pipeline.h"
#include "client/render/qt_gl_video_view.h"
#include "common/utils/log.h"

namespace vivora {

LinuxVideoPipeline::LinuxVideoPipeline(client::QtGlVideoView* view) : view_(view) {
    // Prime the free list with every slot index.  Runs on the main thread
    // before the decode thread starts, so this is the producer side.
    for (uint32_t i = 0; i < static_cast<uint32_t>(kSlots); ++i) free_.try_push(i);
    free_primed_ = true;
}

LinuxVideoPipeline::~LinuxVideoPipeline() = default;

bool LinuxVideoPipeline::init_decoder(VideoCodec codec) {
    if (dec_) return true;
    dec_ = std::make_unique<client::FfmpegDecoder>();
    if (!dec_->init(codec)) {
        vivora::log::error("VIEW", "LinuxVideoPipeline: decoder init failed");
        dec_.reset();
        return false;
    }
    return true;
}

SubmitStatus LinuxVideoPipeline::submit(const uint8_t* data, size_t len,
                                        uint32_t /*timestamp*/, bool keyframe,
                                        uint16_t seq_no) {
    if (!dec_) return SubmitStatus::Rejected;
    return dec_->decode(data, len, seq_no, keyframe) ? SubmitStatus::Accepted
                                                     : SubmitStatus::Rejected;
}

PollStatus LinuxVideoPipeline::poll_frame(FrameHandle& out) {
    if (!dec_) return PollStatus::Empty;
    // Reserve a slot for this attempt and HOLD it across empty polls — pushing
    // it back into free_ here would make the decode thread a second producer
    // and corrupt the SPSC free-list (the cause of the post-N-frame freeze).
    if (reserved_idx_ == FrameHandle::kInvalid) {
        uint32_t idx;
        if (!free_.try_pop(idx)) return PollStatus::PoolFull;  // back-pressure
        reserved_idx_ = idx;
    }

    client::FfmpegDecoder::YuvFrame f;
    if (!dec_->get_frame(f)) {
        return PollStatus::Empty;  // nothing decoded — keep reserved_idx_
    }

    Slot& s = slots_[reserved_idx_];
    s.w  = f.width;  s.h  = f.height;
    s.ys = f.stride[0]; s.us = f.stride[1]; s.vs = f.stride[2];
    s.hdr = dec_->is_hdr();
    s.y.assign(f.plane[0], f.plane[0] + static_cast<size_t>(f.stride[0]) * f.height);
    s.u.assign(f.plane[1], f.plane[1] + static_cast<size_t>(f.stride[1]) * (f.height / 2));
    s.v.assign(f.plane[2], f.plane[2] + static_cast<size_t>(f.stride[2]) * (f.height / 2));
    out.id = reserved_idx_;
    reserved_idx_ = FrameHandle::kInvalid;  // consumed into Q2
    return PollStatus::Produced;
}

void LinuxVideoPipeline::flush_decoder() {
    // Hard re-create, NOT flush().  libav 4.4 (Ubuntu 22.04) HEVC's
    // flush_buffers leaks POC-MSB tracking across IDRs → "Could not find ref"
    // cascades → the decoder conceals the broken area in grey.  This is exactly
    // why the serial LinuxViewPlatform::flush_decoder recreates the context;
    // the threaded path used the leaky flush() and so greyed under loss (VIV-82).
    if (dec_) dec_->reinit();
}
bool LinuxVideoPipeline::reinit_decoder() { return dec_ && dec_->reinit(); }

void LinuxVideoPipeline::present(FrameHandle h) {
    if (h.id >= static_cast<uint32_t>(kSlots) || !view_) return;
    Slot& s = slots_[h.id];
    view_->set_hdr(s.hdr);
    view_->update_yuv(s.y.data(), s.ys, s.u.data(), s.us, s.v.data(), s.vs,
                      s.w, s.h);
}

void LinuxVideoPipeline::recycle(FrameHandle h) {
    if (h.id < static_cast<uint32_t>(kSlots)) free_.try_push(h.id);
}

void LinuxVideoPipeline::unreserve(FrameHandle h) {
    // Re-reserve the slot for the next poll_frame instead of pushing it to the
    // free list — keeps the decode thread off the SPSC producer side (VIV-82).
    if (h.id < static_cast<uint32_t>(kSlots)) reserved_idx_ = h.id;
}

} // namespace vivora

#endif // VIVORA_LINUX
