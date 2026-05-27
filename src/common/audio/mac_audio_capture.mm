#ifdef __APPLE__

#include "common/audio/audio_capture.h"
#include "common/utils/log.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace vivora::audio {

namespace {
constexpr const char* TAG = "MAC_AUDIO_CAP";

static SCShareableContent* sck_fetch_sync() {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block SCShareableContent* result = nil;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent* c, NSError* /*e*/) {
        result = [c retain];
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return result;
}

} // namespace

} // namespace vivora::audio

@class DBAudioStreamOutput;

namespace vivora::audio {
class MacAudioCapture;  // forward — restart hook used by delegate
void mac_audio_capture_request_restart(MacAudioCapture* p);  // forward
}

// Obj-C delegate receiving audio sample buffers from SCStream.
@interface DBAudioStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) std::atomic<bool>* running;
@property (nonatomic, copy) void (^onSamples)(const float* interleaved,
                                              uint32_t frames,
                                              uint32_t sample_rate,
                                              uint16_t channels);
@property (nonatomic, assign) vivora::audio::MacAudioCapture* parent;
@end

@implementation DBAudioStreamOutput

- (void)stream:(SCStream*)stream
       didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                      ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeAudio) return;
    if (!_running || !_running->load()) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    CMAudioFormatDescriptionRef fmt =
        (CMAudioFormatDescriptionRef)CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!fmt) return;
    const AudioStreamBasicDescription* asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
    if (!asbd) return;

    const uint32_t channels = asbd->mChannelsPerFrame;
    const uint32_t rate     = (uint32_t)asbd->mSampleRate;
    const bool     is_float = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool     is_planar = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    // We expect 32-bit float; bail if SCK ever changes default.
    if (!is_float || asbd->mBitsPerChannel != 32) {
        vivora::log::warn(vivora::audio::TAG,
            "Unexpected audio format: flags=0x%x bits=%u channels=%u",
            (unsigned)asbd->mFormatFlags, (unsigned)asbd->mBitsPerChannel, (unsigned)channels);
        return;
    }
    if (channels == 0 || channels > 8) return;

    // Obtain an AudioBufferList. For non-interleaved there is one mBuffer per
    // channel; we allocate enough room.
    const size_t abl_size = sizeof(AudioBufferList) + sizeof(AudioBuffer) * (channels > 0 ? channels - 1 : 0);
    std::vector<uint8_t> abl_storage(abl_size);
    AudioBufferList* abl = reinterpret_cast<AudioBufferList*>(abl_storage.data());

    CMBlockBufferRef block_buf = nullptr;
    OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer,
        nullptr,
        abl,
        (size_t)abl_size,
        kCFAllocatorDefault,
        kCFAllocatorDefault,
        0,
        &block_buf);
    if (st != noErr || !block_buf) {
        if (block_buf) CFRelease(block_buf);
        return;
    }

    // Compute frame count from first buffer.
    if (abl->mNumberBuffers == 0) {
        CFRelease(block_buf);
        return;
    }
    const uint32_t bytes_per_frame_first = is_planar
        ? sizeof(float)
        : sizeof(float) * channels;
    const uint32_t frames = abl->mBuffers[0].mDataByteSize / bytes_per_frame_first;
    if (frames == 0) {
        CFRelease(block_buf);
        return;
    }

    // Interleave into a contiguous buffer if needed.
    std::vector<float> interleaved;
    const float* out_ptr = nullptr;
    if (is_planar) {
        interleaved.resize(frames * channels);
        for (uint32_t ch = 0; ch < channels && ch < abl->mNumberBuffers; ++ch) {
            const float* src = reinterpret_cast<const float*>(abl->mBuffers[ch].mData);
            if (!src) continue;
            for (uint32_t f = 0; f < frames; ++f) {
                interleaved[f * channels + ch] = src[f];
            }
        }
        out_ptr = interleaved.data();
    } else {
        out_ptr = reinterpret_cast<const float*>(abl->mBuffers[0].mData);
    }

    if (out_ptr && _onSamples) {
        _onSamples(out_ptr, frames, rate, (uint16_t)channels);
    }

    CFRelease(block_buf);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    vivora::log::warn(vivora::audio::TAG, "SCStream(audio) stopped: %s — attempting auto-restart",
        error ? [[error localizedDescription] UTF8String] : "no error");
    // Forward to free function declared earlier in the namespace —
    // Obj-C @implementation here can't see MacAudioCapture's methods
    // directly because the class is defined further down in the file.
    if (_parent) vivora::audio::mac_audio_capture_request_restart(_parent);
}

@end

namespace vivora::audio {

class MacAudioCapture : public AudioCapture {
public:
    MacAudioCapture() = default;
    ~MacAudioCapture() override {
        if (wake_observer_) {
            [[[NSWorkspace sharedWorkspace] notificationCenter]
                removeObserver:wake_observer_];
            wake_observer_ = nil;
        }
        stop();
    }

    bool start(AudioCaptureCallback cb) override {
        if (running_.load()) return true;
        saved_cb_ = cb;  // preserved for restart_stream()
        if (!build_and_start_locked()) return false;

        if (!control_q_) {
            control_q_ = dispatch_queue_create("dev.vivora.audio_capture.ctrl", DISPATCH_QUEUE_SERIAL);
        }
        if (!wake_observer_) {
            MacAudioCapture* self_ptr = this;
            wake_observer_ = [[[NSWorkspace sharedWorkspace] notificationCenter]
                addObserverForName:NSWorkspaceDidWakeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* /*note*/) {
                log::info(TAG, "System wake — restarting audio SCStream");
                self_ptr->restart_stream();
            }];
        }
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) {
            teardown();
            return;
        }
        if (stream_) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError* /*e*/) {
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        }
        teardown();
    }

    uint32_t sample_rate() const override { return sample_rate_; }
    uint16_t channels()    const override { return channels_; }

    // Public so the delegate's stop callback + wake observer can poke us.
    void restart_stream() {
        if (!control_q_) return;
        MacAudioCapture* self_ptr = this;
        dispatch_async(control_q_, ^{
            if (self_ptr->stream_) {
                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [self_ptr->stream_ stopCaptureWithCompletionHandler:^(NSError* /*e*/) {
                    dispatch_semaphore_signal(sem);
                }];
                dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
            }
            // teardown_stream_only — keep saved_cb_, control_q_, wake_observer_.
            if (self_ptr->stream_)  { [self_ptr->stream_ release];  self_ptr->stream_  = nil; }
            if (self_ptr->output_)  { [self_ptr->output_ release];  self_ptr->output_  = nil; }
            if (self_ptr->display_) { [self_ptr->display_ release]; self_ptr->display_ = nil; }
            // queue_ is kept (no explicit release in original code path).

            if (!self_ptr->build_and_start_locked()) {
                int attempt = ++self_ptr->restart_attempt_;
                int64_t delay_ms = 250LL << std::min(attempt - 1, 6);
                if (delay_ms > 10'000) delay_ms = 10'000;
                log::warn(TAG, "Audio SCStream restart failed, retry #%d in %lldms",
                          attempt, (long long)delay_ms);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * NSEC_PER_MSEC),
                    self_ptr->control_q_, ^{ self_ptr->restart_stream(); });
                return;
            }
            self_ptr->restart_attempt_ = 0;
            log::info(TAG, "Audio SCStream restart succeeded");
        });
    }

private:
    bool build_and_start_locked() {
        SCShareableContent* content = sck_fetch_sync();
        if (!content || content.displays.count == 0) {
            log::warn(TAG, "No SCShareableContent / no displays for audio capture");
            [content release];
            return false;
        }
        display_ = [content.displays.firstObject retain];

        SCContentFilter* filter = [[SCContentFilter alloc]
            initWithDisplay:display_ excludingWindows:@[]];

        SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
        if (@available(macOS 13.0, *)) {
            cfg.capturesAudio = YES;
            cfg.sampleRate = 48000;
            cfg.channelCount = 2;
            cfg.excludesCurrentProcessAudio = YES;
        } else {
            log::error(TAG, "ScreenCaptureKit audio capture requires macOS 13+");
            [filter release];
            [cfg release];
            [content release];
            return false;
        }
        cfg.width = 16;
        cfg.height = 16;
        cfg.minimumFrameInterval = CMTimeMake(1, 1);
        cfg.showsCursor = NO;
        cfg.queueDepth = 3;

        output_ = [[DBAudioStreamOutput alloc] init];
        output_.running = &running_;
        output_.parent  = this;
        auto cb_copy = saved_cb_;
        sample_rate_ = 48000;
        channels_ = 2;
        output_.onSamples = ^(const float* samples, uint32_t frames,
                              uint32_t rate, uint16_t channels) {
            if (cb_copy) cb_copy(samples, frames, rate, channels);
        };

        if (!queue_) {
            queue_ = dispatch_queue_create("dev.vivora.audio_capture", DISPATCH_QUEUE_SERIAL);
        }
        stream_ = [[SCStream alloc] initWithFilter:filter
                                     configuration:cfg
                                          delegate:output_];

        NSError* err = nil;
        BOOL ok = [stream_ addStreamOutput:output_
                                      type:SCStreamOutputTypeAudio
                        sampleHandlerQueue:queue_
                                     error:&err];
        [filter release];
        [cfg release];
        [content release];
        if (!ok) {
            log::error(TAG, "addStreamOutput(audio) failed: %s",
                       err ? [[err localizedDescription] UTF8String] : "unknown");
            teardown();
            return false;
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block bool start_ok = false;
        __block NSError* start_err = nil;
        [stream_ startCaptureWithCompletionHandler:^(NSError* e) {
            start_err = e;
            start_ok = (e == nil);
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        if (!start_ok) {
            log::error(TAG, "startCapture(audio) failed: %s",
                start_err ? [[start_err localizedDescription] UTF8String] : "unknown");
            teardown();
            return false;
        }

        running_.store(true);
        log::info(TAG, "Audio capture started (48kHz stereo float32)");
        return true;
    }

    void teardown() {
        if (stream_)  { [stream_ release];  stream_  = nil; }
        if (output_)  { [output_ release];  output_  = nil; }
        if (display_) { [display_ release]; display_ = nil; }
        queue_ = nullptr;
    }

    SCStream* stream_ = nil;
    DBAudioStreamOutput* output_ = nil;
    SCDisplay* display_ = nil;
    dispatch_queue_t queue_ = nullptr;
    dispatch_queue_t control_q_ = nullptr;
    id wake_observer_ = nil;
    AudioCaptureCallback saved_cb_;
    int restart_attempt_ = 0;
    std::atomic<bool> running_{false};
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
};

// Adapter for the @implementation delegate (which can't call class
// methods directly because @implementation appears before the class
// definition in this file).
void mac_audio_capture_request_restart(MacAudioCapture* p) {
    if (p) p->restart_stream();
}

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    if (@available(macOS 13.0, *)) {
        return std::make_unique<MacAudioCapture>();
    }
    return nullptr;
}

} // namespace vivora::audio

#endif // __APPLE__
