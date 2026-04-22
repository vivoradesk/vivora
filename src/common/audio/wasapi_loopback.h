#pragma once

#include "common/audio/audio_capture.h"

#include <atomic>
#include <thread>

struct IAudioCaptureClient;
struct IAudioClient;
struct IMMDevice;

namespace deskbeam::audio {

// Windows WASAPI loopback capture of the default render endpoint.
// Produces float32 interleaved PCM at the endpoint's native mix format.
class WasapiLoopbackCapture : public AudioCapture {
public:
    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture() override;

    bool start(AudioCaptureCallback cb) override;
    void stop() override;

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_;    }

private:
    void thread_proc();
    bool open_endpoint();   // opens device/client/capture, binds event_
    void close_endpoint();  // releases client/capture/device; keeps event_

    AudioCaptureCallback cb_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    IMMDevice*           device_ = nullptr;
    IAudioClient*        client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    void*                event_  = nullptr; // HANDLE

    uint32_t sample_rate_ = 0;
    uint16_t channels_    = 0;
    uint16_t bits_per_sample_ = 0;
    bool     is_float_ = false;
};

} // namespace deskbeam::audio
