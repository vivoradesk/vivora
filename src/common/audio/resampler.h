#pragma once

#include <cstddef>
#include <cstdint>

struct SpeexResamplerState_;
typedef struct SpeexResamplerState_ SpeexResamplerState;

namespace deskbeam::audio {

// Float interleaved multichannel resampler (thin speexdsp wrapper).
// Call init() whenever rates/channels change. No-op if input==output rate.
class Resampler {
public:
    Resampler();
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // quality: 0 (fastest) .. 10 (highest); 5 is speexdsp default for voice.
    bool init(int channels, int in_rate, int out_rate, int quality = 5);
    void reset();
    bool is_passthrough() const { return passthrough_; }

    int channels() const { return channels_; }
    int in_rate()  const { return in_rate_;  }
    int out_rate() const { return out_rate_; }

    // Process interleaved float samples. `in_frames` counts samples per channel
    // on input. On return, `in_frames` is set to the count actually consumed
    // and `out_frames` (in/out) to the count actually produced per channel.
    // Returns true on success.
    bool process(const float* in,  unsigned int& in_frames,
                 float*       out, unsigned int& out_frames);

private:
    void destroy();

    SpeexResamplerState* st_ = nullptr;
    int channels_   = 0;
    int in_rate_    = 0;
    int out_rate_   = 0;
    bool passthrough_ = false;
};

} // namespace deskbeam::audio
