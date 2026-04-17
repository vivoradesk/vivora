#pragma once

#include "common/audio/audio_output.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;

namespace deskbeam::audio {

// Windows WASAPI shared-mode render output. Opens the default audio endpoint,
// forces its native mix format, and plays float32 samples pushed via write().
//
// Internally holds a ring buffer (~80ms) filled by write() and drained by a
// dedicated thread that feeds the audio endpoint on event callbacks.
class WasapiOutput : public AudioOutput {
public:
    WasapiOutput();
    ~WasapiOutput() override;

    bool     start(uint32_t sample_rate, uint16_t channels) override;
    void     stop() override;
    uint32_t write(const float* samples, uint32_t frames) override;

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_;    }

private:
    void thread_proc();

    std::atomic<bool> running_{false};
    std::thread worker_;

    IMMDevice*          device_ = nullptr;
    IAudioClient*       client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    void*               event_  = nullptr;

    uint32_t sample_rate_ = 0;
    uint16_t channels_    = 0;
    uint32_t endpoint_frames_ = 0;

    std::mutex ring_mu_;
    std::vector<float> ring_; // interleaved
    size_t ring_read_  = 0;
    size_t ring_write_ = 0;
    size_t ring_size_frames_ = 0;
};

} // namespace deskbeam::audio
