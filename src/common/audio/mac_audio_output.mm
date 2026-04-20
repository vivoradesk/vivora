#ifdef __APPLE__

#include "common/audio/audio_output.h"
#include "common/utils/log.h"

#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace deskbeam::audio {

namespace {
constexpr const char* TAG = "MAC_AUDIO_OUT";

// Small lock-free-ish ring buffer for float samples. Producer (write) and
// consumer (AU render callback) touch separate cursors guarded by a mutex;
// the render callback is short so contention is minimal.
class FloatRing {
public:
    void reset(size_t capacity_samples) {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.assign(capacity_samples, 0.0f);
        read_ = write_ = 0;
        filled_ = 0;
    }

    // Returns number of samples actually written (drops tail on overflow).
    size_t write(const float* src, size_t count) {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t cap = buf_.size();
        if (cap == 0) return 0;
        size_t free_space = cap - filled_;
        size_t to_write = count < free_space ? count : free_space;
        for (size_t i = 0; i < to_write; ++i) {
            buf_[write_] = src[i];
            write_ = (write_ + 1) % cap;
        }
        filled_ += to_write;
        return to_write;
    }

    // Read exactly `count` samples into dst; pad with zeros on underrun.
    void read_padded(float* dst, size_t count) {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t cap = buf_.size();
        size_t avail = filled_;
        size_t to_read = count < avail ? count : avail;
        for (size_t i = 0; i < to_read; ++i) {
            dst[i] = buf_[read_];
            read_ = (read_ + 1) % cap;
        }
        if (to_read < count) {
            std::memset(dst + to_read, 0, sizeof(float) * (count - to_read));
        }
        filled_ -= to_read;
    }

private:
    std::mutex mu_;
    std::vector<float> buf_;
    size_t read_ = 0;
    size_t write_ = 0;
    size_t filled_ = 0;
};

} // namespace

class MacAudioOutput : public AudioOutput {
public:
    MacAudioOutput() = default;
    ~MacAudioOutput() override { stop(); }

    bool start(uint32_t sample_rate, uint16_t channels) override {
        if (started_) return true;
        sample_rate_ = sample_rate;
        channels_ = channels;

        // ~40 ms buffer: rate * ch * 0.04.
        ring_.reset((size_t)sample_rate * channels * 40 / 1000);

        AudioComponentDescription desc = {};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            log::error(TAG, "Default output AudioComponent not found");
            return false;
        }
        OSStatus st = AudioComponentInstanceNew(comp, &unit_);
        if (st != noErr) {
            log::error(TAG, "AudioComponentInstanceNew failed: %d", (int)st);
            return false;
        }

        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate       = sample_rate;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mFramesPerPacket  = 1;
        asbd.mChannelsPerFrame = channels;
        asbd.mBitsPerChannel   = 32;
        asbd.mBytesPerFrame    = sizeof(float) * channels;
        asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
        if (st != noErr) {
            log::error(TAG, "SetProperty StreamFormat failed: %d", (int)st);
            teardown();
            return false;
        }

        AURenderCallbackStruct cb = {};
        cb.inputProc = &MacAudioOutput::render_cb;
        cb.inputProcRefCon = this;
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &cb, sizeof(cb));
        if (st != noErr) {
            log::error(TAG, "SetProperty RenderCallback failed: %d", (int)st);
            teardown();
            return false;
        }

        st = AudioUnitInitialize(unit_);
        if (st != noErr) {
            log::error(TAG, "AudioUnitInitialize failed: %d", (int)st);
            teardown();
            return false;
        }
        st = AudioOutputUnitStart(unit_);
        if (st != noErr) {
            log::error(TAG, "AudioOutputUnitStart failed: %d", (int)st);
            teardown();
            return false;
        }

        started_ = true;
        log::info(TAG, "Audio output started: %u Hz, %u ch", sample_rate, channels);
        return true;
    }

    void stop() override {
        if (!started_) { teardown(); return; }
        if (unit_) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
        }
        teardown();
        started_ = false;
    }

    uint32_t write(const float* samples, uint32_t frames) override {
        if (!started_ || channels_ == 0) return 0;
        size_t written = ring_.write(samples, (size_t)frames * channels_);
        return (uint32_t)(written / channels_);
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_; }

private:
    static OSStatus render_cb(void* refcon,
                              AudioUnitRenderActionFlags* /*flags*/,
                              const AudioTimeStamp* /*ts*/,
                              UInt32 /*bus*/,
                              UInt32 frames,
                              AudioBufferList* data) {
        auto* self = static_cast<MacAudioOutput*>(refcon);
        if (!data || data->mNumberBuffers == 0) return noErr;
        // Packed interleaved float: single buffer expected.
        float* dst = reinterpret_cast<float*>(data->mBuffers[0].mData);
        size_t samples = (size_t)frames * self->channels_;
        self->ring_.read_padded(dst, samples);
        return noErr;
    }

    void teardown() {
        if (unit_) {
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }

    AudioUnit unit_ = nullptr;
    FloatRing ring_;
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    bool started_ = false;
};

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return std::make_unique<MacAudioOutput>();
}

} // namespace deskbeam::audio

#endif // __APPLE__
