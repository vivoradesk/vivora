#include "common/audio/resampler.h"
#include "common/utils/log.h"

#include <speex/speex_resampler.h>

#include <algorithm>
#include <cstring>

namespace vivora::audio {

Resampler::Resampler() = default;
Resampler::~Resampler() { destroy(); }

void Resampler::destroy() {
    if (st_) {
        speex_resampler_destroy(st_);
        st_ = nullptr;
    }
}

bool Resampler::init(int channels, int in_rate, int out_rate, int quality) {
    destroy();
    channels_ = channels;
    in_rate_  = in_rate;
    out_rate_ = out_rate;
    passthrough_ = (in_rate == out_rate);
    if (passthrough_) {
        return true;
    }
    int err = 0;
    st_ = speex_resampler_init(static_cast<spx_uint32_t>(channels),
                               static_cast<spx_uint32_t>(in_rate),
                               static_cast<spx_uint32_t>(out_rate),
                               quality, &err);
    if (err != RESAMPLER_ERR_SUCCESS || !st_) {
        log::error("Resampler", "init failed: %s",
                   speex_resampler_strerror(err));
        st_ = nullptr;
        return false;
    }
    log::info("Resampler", "%d ch, %d -> %d Hz, quality=%d",
              channels, in_rate, out_rate, quality);
    return true;
}

void Resampler::reset() {
    if (st_) speex_resampler_reset_mem(st_);
}

bool Resampler::process(const float* in,  unsigned int& in_frames,
                        float*       out, unsigned int& out_frames) {
    if (passthrough_) {
        unsigned int n = std::min(in_frames, out_frames);
        std::memcpy(out, in, sizeof(float) * n * channels_);
        in_frames = n;
        out_frames = n;
        return true;
    }
    if (!st_) return false;
    spx_uint32_t in_len  = in_frames;
    spx_uint32_t out_len = out_frames;
    int err = speex_resampler_process_interleaved_float(
        st_, in, &in_len, out, &out_len);
    in_frames  = in_len;
    out_frames = out_len;
    return err == RESAMPLER_ERR_SUCCESS;
}

} // namespace vivora::audio
