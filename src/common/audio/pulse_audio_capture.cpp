// PulseAudio loopback capture for Linux hosts.  Records from the default
// sink's monitor source ("@DEFAULT_MONITOR@" — a server-side alias both
// real PulseAudio and PipeWire's pulse-shim resolve to whatever the
// active output device's monitor is).  Synchronous pa_simple API + a
// dedicated thread that pumps callback-shaped float32 blocks.
//
// Matches the WASAPI / CoreAudio loopback contract: deliver interleaved
// float PCM at the device-native rate as 10ms-ish chunks.

#if defined(VIVORA_LINUX)

#include "common/audio/audio_capture.h"
#include "common/utils/log.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <atomic>
#include <thread>
#include <vector>

namespace vivora::audio {

namespace {

constexpr const char* TAG          = "PulseCap";
constexpr uint32_t    SAMPLE_RATE  = 48'000;   // matches Opus encoder + Win/Mac hosts
constexpr uint16_t    CHANNELS     = 2;
constexpr uint32_t    BLOCK_FRAMES = 480;       // 10ms @ 48kHz

class PulseAudioCapture : public AudioCapture {
public:
    ~PulseAudioCapture() override { stop(); }

    bool start(AudioCaptureCallback cb) override {
        stop();
        cb_ = std::move(cb);

        pa_sample_spec spec{};
        spec.format   = PA_SAMPLE_FLOAT32LE;
        spec.rate     = SAMPLE_RATE;
        spec.channels = static_cast<uint8_t>(CHANNELS);

        // Tight fragsize so the daemon delivers 10ms chunks instead of
        // hundreds-of-ms batches (which would inflate end-to-end latency).
        pa_buffer_attr attr{};
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.fragsize  = BLOCK_FRAMES * CHANNELS * sizeof(float);
        attr.tlength   = static_cast<uint32_t>(-1);
        attr.prebuf    = static_cast<uint32_t>(-1);
        attr.minreq    = static_cast<uint32_t>(-1);

        int err = 0;
        s_ = pa_simple_new(nullptr,                 // default server
                           "Vivora",
                           PA_STREAM_RECORD,
                           "@DEFAULT_MONITOR@",     // default sink's monitor
                           "loopback",
                           &spec,
                           nullptr,                 // default channel map
                           &attr,
                           &err);
        if (!s_) {
            log::error(TAG, "pa_simple_new failed: %s", pa_strerror(err));
            return false;
        }
        sample_rate_ = SAMPLE_RATE;
        channels_    = CHANNELS;
        log::info(TAG, "Opened @DEFAULT_MONITOR@: %u Hz, %u ch, float32",
                  sample_rate_, channels_);

        running_.store(true);
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        if (s_) {
            pa_simple_free(s_);
            s_ = nullptr;
        }
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_; }

private:
    void run() {
        std::vector<float> buf(BLOCK_FRAMES * CHANNELS);
        const size_t bytes = buf.size() * sizeof(float);
        while (running_.load()) {
            int err = 0;
            if (pa_simple_read(s_, buf.data(), bytes, &err) < 0) {
                log::warn(TAG, "pa_simple_read: %s", pa_strerror(err));
                // Tiny back-off so a persistent error doesn't spin a hot loop.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (cb_) cb_(buf.data(), BLOCK_FRAMES, sample_rate_, channels_);
        }
    }

    pa_simple*           s_ = nullptr;
    AudioCaptureCallback cb_;
    std::thread          worker_;
    std::atomic<bool>    running_{false};
    uint32_t             sample_rate_ = 0;
    uint16_t             channels_    = 0;
};

} // namespace

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    return std::make_unique<PulseAudioCapture>();
}

} // namespace vivora::audio

#endif // VIVORA_LINUX
