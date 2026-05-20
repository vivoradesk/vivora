#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace vivora::audio {

// Interleaved float PCM delivered at the device-native sample rate and
// channel count. Implementations decide the block size (typically equal to
// the endpoint period, ~10ms).
//
// samples      — interleaved float32 samples, length = frames * channels
// frames       — per-channel frame count
// sample_rate  — device sample rate in Hz (may be 44100, 48000, ...)
// channels     — device channel count (1 or 2 typical)
using AudioCaptureCallback =
    std::function<void(const float* samples, uint32_t frames,
                       uint32_t sample_rate, uint16_t channels)>;

// System audio capture (host side: loopback from default render device).
// Same interface is reusable later for microphone capture on the client.
class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    virtual bool start(AudioCaptureCallback cb) = 0;
    virtual void stop() = 0;

    // Native device parameters, valid after start().
    virtual uint32_t sample_rate() const = 0;
    virtual uint16_t channels()    const = 0;
};

// Create platform default capture (loopback). Returns nullptr if unsupported.
std::unique_ptr<AudioCapture> create_default_loopback_capture();

} // namespace vivora::audio
