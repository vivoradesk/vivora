#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace deskbeam::audio {

// Audio playback sink. Caller pushes interleaved float PCM at the rate and
// channel count negotiated with the device. Implementations internally
// buffer a small amount (~20-40ms) and drop excess if overrun.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Open device. Caller will push PCM at this rate/channels after resampling
    // and channel conversion. Returns true on success.
    virtual bool start(uint32_t sample_rate, uint16_t channels) = 0;
    virtual void stop() = 0;

    // Write interleaved float32 PCM. `frames` = per-channel frames.
    // Returns frames actually accepted (usually == frames; short writes mean
    // the device buffer is full and caller should drop).
    virtual uint32_t write(const float* samples, uint32_t frames) = 0;

    virtual uint32_t sample_rate() const = 0;
    virtual uint16_t channels()    const = 0;
};

std::unique_ptr<AudioOutput> create_default_audio_output();

} // namespace deskbeam::audio
