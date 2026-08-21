// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// PulseAudio sink for Linux clients.  Uses the synchronous `pa_simple` API
// because AudioReceiver already runs on its own thread and pushes PCM in
// 10ms chunks — a blocking write that drains into the daemon's buffer is
// the simplest reliable shape, and matches the WASAPI sync write model on
// Windows.  PipeWire ships a pulse-shim out of the box on modern distros,
// so this same code talks to either daemon transparently.

#if defined(VIVORA_LINUX)

#include "common/audio/audio_output.h"
#include "common/utils/log.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <cstring>

namespace vivora::audio {

namespace {

class PulseAudioOutput : public AudioOutput {
public:
    ~PulseAudioOutput() override { stop(); }

    bool start(uint32_t sample_rate, uint16_t channels) override {
        stop();
        pa_sample_spec spec{};
        spec.format   = PA_SAMPLE_FLOAT32LE;
        spec.rate     = sample_rate;
        spec.channels = static_cast<uint8_t>(channels);

        // Latency hint: 20ms.  PulseAudio still buffers more than this
        // internally; the value just biases its own scheduler.  Lower
        // values risk underruns, higher add to end-to-end audio latency.
        pa_buffer_attr attr{};
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.tlength   = static_cast<uint32_t>(sample_rate * channels * sizeof(float) * 2 / 100);
        attr.prebuf    = static_cast<uint32_t>(-1);
        attr.minreq    = static_cast<uint32_t>(-1);
        attr.fragsize  = static_cast<uint32_t>(-1);

        int err = 0;
        s_ = pa_simple_new(nullptr,           // server (default)
                           "Vivora",        // app name
                           PA_STREAM_PLAYBACK,
                           nullptr,           // device (default sink)
                           "stream",          // stream description
                           &spec,
                           nullptr,           // channel map (default)
                           &attr,
                           &err);
        if (!s_) {
            log::error("PulseOut", "pa_simple_new failed: %s", pa_strerror(err));
            return false;
        }
        sample_rate_ = sample_rate;
        channels_    = channels;
        log::info("PulseOut", "Opened: %u Hz, %u ch, float32",
                  sample_rate, channels);
        return true;
    }

    void stop() override {
        if (s_) {
            pa_simple_free(s_);
            s_ = nullptr;
        }
    }

    uint32_t write(const float* samples, uint32_t frames) override {
        if (!s_ || !samples || frames == 0) return 0;
        const size_t bytes = static_cast<size_t>(frames) * channels_ * sizeof(float);
        int err = 0;
        if (pa_simple_write(s_, samples, bytes, &err) < 0) {
            // Underflow / device disappear — log once per occurrence; the
            // session machinery will reconnect.
            log::warn("PulseOut", "pa_simple_write: %s", pa_strerror(err));
            return 0;
        }
        return frames;
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_; }

private:
    pa_simple* s_ = nullptr;
    uint32_t   sample_rate_ = 0;
    uint16_t   channels_    = 0;
};

} // namespace

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return std::make_unique<PulseAudioOutput>();
}

} // namespace vivora::audio

#endif // VIVORA_LINUX
