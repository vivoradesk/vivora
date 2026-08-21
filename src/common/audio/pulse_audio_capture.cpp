// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// PulseAudio loopback capture for Linux hosts.  Records from the default
// sink's monitor source ("@DEFAULT_MONITOR@" — a server-side alias both
// real PulseAudio and PipeWire's pulse-shim resolve to whatever the
// active output device's monitor is).
//
// Uses the ASYNC pa_threaded_mainloop + pa_stream API (not the synchronous
// pa_simple wrapper).  Rationale: pa_simple_read() has no timeout and cannot
// be interrupted from another thread — when module-suspend-on-idle suspends
// the monitor source (sink goes idle), the read blocks indefinitely, and a
// stop() that join()s the read thread deadlocks the whole host on shutdown.
// With the threaded mainloop, stop() calls pa_threaded_mainloop_stop(), which
// signals the loop to exit and joins it cleanly — there is no blocking read to
// hang on.  Captured PCM is delivered from the stream read callback (runs on
// the mainloop thread), matching the WASAPI / CoreAudio loopback contract:
// interleaved float PCM at the device-native rate in ~10ms chunks.

#if defined(VIVORA_LINUX)

#include "common/audio/audio_capture.h"
#include "common/utils/log.h"

#include <pulse/pulseaudio.h>

#include <vector>

namespace vivora::audio {

namespace {

constexpr const char* TAG          = "PulseCap";
constexpr uint32_t    SAMPLE_RATE  = 48'000;   // matches Opus encoder + Win/Mac hosts
constexpr uint16_t    CHANNELS     = 2;
constexpr uint32_t    BLOCK_FRAMES = 480;       // 10ms @ 48kHz — fragsize hint

class PulseAudioCapture : public AudioCapture {
public:
    ~PulseAudioCapture() override { stop(); }

    bool start(AudioCaptureCallback cb) override {
        stop();
        cb_ = std::move(cb);

        ml_ = pa_threaded_mainloop_new();
        if (!ml_) {
            log::error(TAG, "pa_threaded_mainloop_new failed");
            return false;
        }

        ctx_ = pa_context_new(pa_threaded_mainloop_get_api(ml_), "Vivora");
        if (!ctx_) {
            log::error(TAG, "pa_context_new failed");
            cleanup();
            return false;
        }
        pa_context_set_state_callback(ctx_, &PulseAudioCapture::context_state_cb, this);

        if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            log::error(TAG, "pa_context_connect: %s",
                       pa_strerror(pa_context_errno(ctx_)));
            cleanup();
            return false;
        }

        pa_threaded_mainloop_lock(ml_);
        if (pa_threaded_mainloop_start(ml_) < 0) {
            pa_threaded_mainloop_unlock(ml_);
            log::error(TAG, "pa_threaded_mainloop_start failed");
            cleanup();
            return false;
        }
        ml_started_ = true;

        // Block (interruptibly, via the state callback's signal) until the
        // context is READY — or bail if it enters a terminal bad state.
        for (;;) {
            const pa_context_state_t st = pa_context_get_state(ctx_);
            if (st == PA_CONTEXT_READY) break;
            if (!PA_CONTEXT_IS_GOOD(st)) {
                log::error(TAG, "context connect failed: %s",
                           pa_strerror(pa_context_errno(ctx_)));
                pa_threaded_mainloop_unlock(ml_);
                cleanup();
                return false;
            }
            pa_threaded_mainloop_wait(ml_);
        }

        pa_sample_spec spec{};
        spec.format   = PA_SAMPLE_FLOAT32LE;
        spec.rate     = SAMPLE_RATE;
        spec.channels = static_cast<uint8_t>(CHANNELS);

        stream_ = pa_stream_new(ctx_, "loopback", &spec, nullptr);
        if (!stream_) {
            log::error(TAG, "pa_stream_new: %s",
                       pa_strerror(pa_context_errno(ctx_)));
            pa_threaded_mainloop_unlock(ml_);
            cleanup();
            return false;
        }
        pa_stream_set_state_callback(stream_, &PulseAudioCapture::stream_state_cb, this);
        pa_stream_set_read_callback(stream_, &PulseAudioCapture::stream_read_cb, this);

        // Tight fragsize so the daemon delivers ~10ms chunks instead of
        // hundreds-of-ms batches (which would inflate end-to-end latency).
        // ADJUST_LATENCY asks the server to honour it.
        pa_buffer_attr attr{};
        attr.maxlength = static_cast<uint32_t>(-1);
        attr.fragsize  = BLOCK_FRAMES * CHANNELS * sizeof(float);
        attr.tlength   = static_cast<uint32_t>(-1);
        attr.prebuf    = static_cast<uint32_t>(-1);
        attr.minreq    = static_cast<uint32_t>(-1);

        if (pa_stream_connect_record(stream_, "@DEFAULT_MONITOR@", &attr,
                                     PA_STREAM_ADJUST_LATENCY) < 0) {
            log::error(TAG, "pa_stream_connect_record: %s",
                       pa_strerror(pa_context_errno(ctx_)));
            pa_threaded_mainloop_unlock(ml_);
            cleanup();
            return false;
        }

        for (;;) {
            const pa_stream_state_t st = pa_stream_get_state(stream_);
            if (st == PA_STREAM_READY) break;
            if (!PA_STREAM_IS_GOOD(st)) {
                log::error(TAG, "stream connect failed: %s",
                           pa_strerror(pa_context_errno(ctx_)));
                pa_threaded_mainloop_unlock(ml_);
                cleanup();
                return false;
            }
            pa_threaded_mainloop_wait(ml_);
        }

        sample_rate_ = SAMPLE_RATE;
        channels_    = CHANNELS;
        pa_threaded_mainloop_unlock(ml_);
        log::info(TAG, "Opened @DEFAULT_MONITOR@: %u Hz, %u ch, float32",
                  sample_rate_, channels_);
        return true;
    }

    void stop() override { cleanup(); }

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_; }

private:
    // Tear everything down.  Stops (and joins) the mainloop thread FIRST so no
    // callback can run while we unref, then disconnects/frees from this thread.
    // Safe to call on a partially-constructed instance and idempotent.
    void cleanup() {
        if (ml_ && ml_started_) pa_threaded_mainloop_stop(ml_);
        ml_started_ = false;
        if (stream_) {
            pa_stream_disconnect(stream_);
            pa_stream_unref(stream_);
            stream_ = nullptr;
        }
        if (ctx_) {
            pa_context_disconnect(ctx_);
            pa_context_unref(ctx_);
            ctx_ = nullptr;
        }
        if (ml_) {
            pa_threaded_mainloop_free(ml_);
            ml_ = nullptr;
        }
        cb_ = nullptr;
    }

    // --- PulseAudio callbacks (invoked on the mainloop thread) ---

    static void context_state_cb(pa_context* /*c*/, void* userdata) {
        auto* self = static_cast<PulseAudioCapture*>(userdata);
        pa_threaded_mainloop_signal(self->ml_, 0);
    }

    static void stream_state_cb(pa_stream* /*s*/, void* userdata) {
        auto* self = static_cast<PulseAudioCapture*>(userdata);
        pa_threaded_mainloop_signal(self->ml_, 0);
    }

    static void stream_read_cb(pa_stream* s, size_t /*nbytes*/, void* userdata) {
        auto* self = static_cast<PulseAudioCapture*>(userdata);
        while (pa_stream_readable_size(s) > 0) {
            const void* data = nullptr;
            size_t      nbytes = 0;
            if (pa_stream_peek(s, &data, &nbytes) < 0) {
                log::warn(TAG, "pa_stream_peek failed");
                return;
            }
            if (nbytes == 0) break;   // buffer momentarily empty
            // data == nullptr with nbytes > 0 is a hole (dropped chunk): skip
            // it by dropping without delivering — audio path tolerates gaps.
            if (data) {
                const uint32_t frames =
                    static_cast<uint32_t>(nbytes / (CHANNELS * sizeof(float)));
                if (frames && self->cb_) {
                    self->cb_(static_cast<const float*>(data), frames,
                              self->sample_rate_, CHANNELS);
                }
            }
            pa_stream_drop(s);
        }
    }

    pa_threaded_mainloop* ml_     = nullptr;
    pa_context*           ctx_    = nullptr;
    pa_stream*            stream_ = nullptr;
    bool                  ml_started_ = false;

    AudioCaptureCallback  cb_;
    uint32_t              sample_rate_ = 0;
    uint16_t              channels_    = 0;
};

} // namespace

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    return std::make_unique<PulseAudioCapture>();
}

} // namespace vivora::audio

#endif // VIVORA_LINUX
