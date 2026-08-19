#ifdef VIVORA_MACOS

#include "host/capture/mac_screen_capture.h"
#include "common/utils/log.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>

namespace vivora::host {

// VIV-95: identity tag for the control queue (see Impl::alive / stop()).
static const void* const kControlQueueKey = &kControlQueueKey;


namespace {
constexpr const char* TAG = "MAC_CAPTURE";

// Check whether a display reports an HDR-capable color space.
static bool display_is_hdr(CGDirectDisplayID did) {
    CGColorSpaceRef cs = CGDisplayCopyColorSpace(did);
    if (!cs) return false;
    CFStringRef name = CGColorSpaceGetName(cs);
    bool hdr = false;
    if (name) {
        // HDR color spaces contain "2020" or "PQ" or "HLG" or "Extended" in the name.
        CFRange r;
        if (CFStringFindWithOptions(name, CFSTR("2020"),  CFRangeMake(0, CFStringGetLength(name)), 0, &r) ||
            CFStringFindWithOptions(name, CFSTR("PQ"),    CFRangeMake(0, CFStringGetLength(name)), 0, &r) ||
            CFStringFindWithOptions(name, CFSTR("HLG"),   CFRangeMake(0, CFStringGetLength(name)), 0, &r)) {
            hdr = true;
        }
    }
    CFRelease(cs);
    return hdr;
}

// Synchronously fetch SCShareableContent via a semaphore.
// Returns a retained object — caller must [release] it when done.
// (This TU is compiled without ARC, so ScreenCaptureKit's autoreleased
// content would otherwise be gone by the time the caller uses it.)
static SCShareableContent* fetch_shareable_content_sync() {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block SCShareableContent* result = nil;
    __block NSError* err = nil;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent* c, NSError* e) {
        result = [c retain];
        err   = [e retain];
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (err) {
        log::error(TAG, "SCShareableContent error: %s",
                   [[err localizedDescription] UTF8String]);
        [err release];
    }
    return result; // +1 retain, caller releases
}
} // namespace
} // namespace vivora::host

// Obj-C delegate that receives sample buffers.
@interface DBSCStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) std::mutex* mutex;
@property (nonatomic, assign) CVPixelBufferRef* latestFrame;
@property (nonatomic, assign) uint64_t* latestPtsUs;
@property (nonatomic, assign) uint64_t* framesDelivered;
@property (nonatomic, assign) CVPixelBufferRef* cachedLastFrame;
@property (nonatomic, assign) uint64_t* cachedLastPtsUs;
@property (nonatomic, assign) vivora::host::MacScreenCapture* parent;
@end

@implementation DBSCStreamOutput

- (void)stream:(SCStream*)stream
       didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                      ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    // Filter out frames that are not "complete" (dirty-only updates etc.).
    CFArrayRef attach = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attach && CFArrayGetCount(attach) > 0) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(attach, 0);
        CFNumberRef status = (CFNumberRef)CFDictionaryGetValue(
            d, (const void*)CFSTR("SCStreamUpdateFrameStatus"));
        if (status) {
            int v = 0;
            CFNumberGetValue(status, kCFNumberIntType, &v);
            // 0 = complete, 1 = idle, 2 = blank, 3 = suspended, 4 = started, 5 = stopped
            if (v != 0) return;
        }
    }

    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pb) return;

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    uint64_t pts_us = CMTIME_IS_VALID(pts)
        ? (uint64_t)(CMTimeGetSeconds(pts) * 1'000'000.0)
        : 0;

    CFRetain(pb);  // for latest_frame slot
    CFRetain(pb);  // for cached_last_frame slot — IDR-on-loss fallback
    {
        std::lock_guard<std::mutex> lock(*_mutex);
        if (*_latestFrame) {
            CFRelease(*_latestFrame);
        }
        *_latestFrame = pb;
        *_latestPtsUs = pts_us;
        (*_framesDelivered)++;
        if (*_cachedLastFrame) {
            CFRelease(*_cachedLastFrame);
        }
        *_cachedLastFrame = pb;
        *_cachedLastPtsUs = pts_us;
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    vivora::log::warn(vivora::host::TAG, "SCStream stopped: %s — attempting auto-restart",
        error ? [[error localizedDescription] UTF8String] : "no error");
    if (_parent) _parent->restart_stream();
}

@end

namespace vivora::host {

struct MacScreenCapture::Impl {
    SCStream* stream = nil;
    DBSCStreamOutput* output = nil;
    dispatch_queue_t queue = nullptr;
    SCDisplay* display = nil;

    std::mutex mutex;
    CVPixelBufferRef latest_frame = nullptr;
    // Separately-retained copy of the most-recent delivered frame —
    // survives a normal try_get_frame() pull so the IDR-on-loss path
    // has SOMETHING to re-encode when SCK has gone dormant on a
    // static screen.  Updated on every callback delivery; freed at
    // shutdown.
    CVPixelBufferRef cached_last_frame = nullptr;
    uint64_t cached_last_pts_us = 0;
    uint64_t latest_pts_us = 0;
    uint64_t frames_delivered = 0;
    uint64_t last_pulled_count = 0;

    // Saved init args so we can rebuild the SCStream on restart
    // without the caller having to re-supply them.
    MacCaptureConfig saved_config{};

    // Serial queue for restart attempts — coalesces concurrent
    // triggers (didStopWithError + NSWorkspaceDidWake firing within
    // ms of each other) and serialises the tear-down + rebuild so
    // we never race a stop and a start.
    dispatch_queue_t control_q = nullptr;
    int restart_attempt = 0;

    // VIV-95: liveness token for work already queued on control_q.
    // schedule_restart_retry() arms a dispatch_after that cannot be
    // cancelled, and the wake observer fires on an arbitrary thread — both
    // captured a raw `this`, so either could run after the capture object
    // was destroyed and use it (crash on disconnect right after an SCK error
    // or a system wake).  Blocks now capture a copy of this shared flag and
    // check it before touching the object; stop() clears it.
    std::shared_ptr<std::atomic<bool>> alive =
        std::make_shared<std::atomic<bool>>(true);

    // NSWorkspace observer token for `removeObserver:` at shutdown.
    id wake_observer = nil;
};

MacScreenCapture::MacScreenCapture() : impl_(new Impl()) {}

MacScreenCapture::~MacScreenCapture() {
    if (impl_ && impl_->wake_observer) {
        [[[NSWorkspace sharedWorkspace] notificationCenter]
            removeObserver:impl_->wake_observer];
        impl_->wake_observer = nil;
    }
    stop();
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->latest_frame) {
            CFRelease(impl_->latest_frame);
            impl_->latest_frame = nullptr;
        }
        if (impl_->cached_last_frame) {
            CFRelease(impl_->cached_last_frame);
            impl_->cached_last_frame = nullptr;
        }
    }
    delete impl_;
}

std::vector<MacDisplayInfo> MacScreenCapture::enumerate_displays() {
    std::vector<MacDisplayInfo> out;
    SCShareableContent* content = fetch_shareable_content_sync();
    if (!content) return out;

    NSArray<SCDisplay*>* displays = content.displays;
    for (NSUInteger i = 0; i < displays.count; ++i) {
        SCDisplay* d = displays[i];
        MacDisplayInfo info;
        info.index = (uint32_t)i;
        info.display_id = (uint32_t)d.displayID;
        info.width_px = (uint32_t)CGDisplayPixelsWide(d.displayID);
        info.height_px = (uint32_t)CGDisplayPixelsHigh(d.displayID);
        info.hdr_capable = display_is_hdr(d.displayID);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Display #%u (%ux%u)",
                      info.index, info.width_px, info.height_px);
        info.name = buf;
        out.push_back(info);
    }
    [content release];
    return out;
}

bool MacScreenCapture::init(const MacCaptureConfig& config) {
    // Save for restart_stream() and the wake-from-sleep observer below.
    impl_->saved_config = config;

    SCShareableContent* content = fetch_shareable_content_sync();
    if (!content || content.displays.count == 0) {
        log::error(TAG, "No displays available");
        [content release];
        return false;
    }
    if (config.display_index >= content.displays.count) {
        log::error(TAG, "Display index %u out of range (have %lu)",
                   config.display_index, (unsigned long)content.displays.count);
        [content release];
        return false;
    }

    // Retain the chosen SCDisplay — it's owned by `content` which we release below.
    impl_->display = [content.displays[config.display_index] retain];
    CGDirectDisplayID did = impl_->display.displayID;

    // Logical "points" space — used for input injection and as SC's coord system.
    points_w_ = (uint32_t)CGDisplayPixelsWide(did);
    points_h_ = (uint32_t)CGDisplayPixelsHigh(did);

    // True backing pixel dimensions. On Retina / HiDPI Macs this is much larger
    // than CGDisplayPixelsWide (which returns points on scaled modes). Capturing
    // at native pixels avoids the 2x/4x downsample that makes text blurry.
    uint32_t pixel_w = points_w_;
    uint32_t pixel_h = points_h_;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
    if (mode) {
        size_t pw = CGDisplayModeGetPixelWidth(mode);
        size_t ph = CGDisplayModeGetPixelHeight(mode);
        if (pw > 0 && ph > 0) {
            pixel_w = (uint32_t)pw;
            pixel_h = (uint32_t)ph;
        }
        CGDisplayModeRelease(mode);
    }

    // Round down to multiples of 16 so the HEVC encoder never needs to pad
    // (non-16-aligned inputs produce a cropped SPS conformance window that
    // some decoders — notably Windows MF — don't honor, causing visible
    // garbage rows and wrong aspect ratio on the client).
    width_  = pixel_w & ~0xFu;
    height_ = pixel_h & ~0xFu;
    if (width_ != pixel_w || height_ != pixel_h) {
        log::info(TAG, "Aligning capture %ux%u -> %ux%u (multiple of 16)",
                  pixel_w, pixel_h, width_, height_);
    }
    if (pixel_w != points_w_ || pixel_h != points_h_) {
        log::info(TAG, "Retina display: points %ux%u, backing pixels %ux%u",
                  points_w_, points_h_, pixel_w, pixel_h);
    }

    bool display_hdr = display_is_hdr(did);
    hdr_active_ = config.prefer_hdr && display_hdr;
    if (config.prefer_hdr && !display_hdr) {
        log::warn(TAG, "HDR requested but display is SDR, falling back");
    }

    SCContentFilter* filter = [[SCContentFilter alloc]
        initWithDisplay:impl_->display excludingWindows:@[]];

    SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
    cfg.width = width_;
    cfg.height = height_;
    cfg.minimumFrameInterval = CMTimeMake(1, (int32_t)config.fps);
    cfg.showsCursor = config.show_cursor ? YES : NO;
    cfg.queueDepth = 5;

    if (hdr_active_) {
        // 10-bit 4:2:0 full range for HDR (PQ / BT.2020).
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
        if (@available(macOS 13.0, *)) {
            cfg.colorSpaceName = kCGColorSpaceITUR_2100_PQ;
        }
    } else {
        // 8-bit 4:2:0 video range for SDR (NV12).
        cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        if (@available(macOS 13.0, *)) {
            cfg.colorSpaceName = kCGColorSpaceSRGB;
        }
    }

    impl_->output = [[DBSCStreamOutput alloc] init];
    impl_->output.mutex = &impl_->mutex;
    impl_->output.latestFrame = &impl_->latest_frame;
    impl_->output.latestPtsUs = &impl_->latest_pts_us;
    impl_->output.framesDelivered = &impl_->frames_delivered;
    impl_->output.cachedLastFrame = &impl_->cached_last_frame;
    impl_->output.cachedLastPtsUs = &impl_->cached_last_pts_us;
    impl_->output.parent = this;

    impl_->queue = dispatch_queue_create("dev.vivora.capture", DISPATCH_QUEUE_SERIAL);
    impl_->control_q = dispatch_queue_create("dev.vivora.capture.ctrl", DISPATCH_QUEUE_SERIAL);
    // Tag control_q so stop() can tell whether it is already running on it and
    // run its teardown inline instead of dispatch_sync-ing into itself (which
    // would deadlock a serial queue).
    dispatch_queue_set_specific(impl_->control_q, kControlQueueKey,
                                impl_->control_q, nullptr);

    impl_->stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:cfg
                                             delegate:impl_->output];

    NSError* err = nil;
    BOOL ok = [impl_->stream addStreamOutput:impl_->output
                                        type:SCStreamOutputTypeScreen
                          sampleHandlerQueue:impl_->queue
                                       error:&err];
    // SCStream retains filter + config internally; release our +1 refs.
    [filter release];
    [cfg release];
    [content release];
    if (!ok) {
        log::error(TAG, "addStreamOutput failed: %s",
                   err ? [[err localizedDescription] UTF8String] : "unknown");
        return false;
    }

    // Subscribe to system wake notifications — lid open / sleep timer
    // wake leaves SCStream in a silent-dead state without firing any
    // stop event.  This observer triggers an explicit restart on every
    // wake so the user doesn't have to Stop/Start Sharing manually.
    MacScreenCapture* self_ptr = this;
    auto alive_token = impl_->alive;   // VIV-95
    impl_->wake_observer = [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* /*note*/) {
        if (!alive_token->load()) return;   // VIV-95
        log::info(TAG, "System wake — restarting SCStream");
        self_ptr->restart_stream();
    }];

    log::info(TAG, "Initialized: %ux%u @ %u fps, %s, cursor=%s",
              width_, height_, config.fps,
              hdr_active_ ? "HDR10" : "SDR",
              config.show_cursor ? "on" : "off");
    return true;
}

bool MacScreenCapture::restart_stream() {
    if (!impl_ || !impl_->control_q) return false;
    if (!impl_->alive->load()) return false;
    auto alive = impl_->alive;   // VIV-95: outlives `this`
    dispatch_async(impl_->control_q, ^{
        // stop() may have run (and the object been destroyed) between the
        // dispatch and now — the token is the only thing safe to read here.
        if (!alive->load()) return;
        // Tear down the existing stream.  Stop is synchronous to make
        // sure the OS won't call back into the output we're about to
        // release.  3-second timeout covers a wedged stop callback.
        if (impl_->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [impl_->stream stopCaptureWithCompletionHandler:^(NSError* /*e*/) {
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
            [impl_->stream removeStreamOutput:impl_->output
                                         type:SCStreamOutputTypeScreen
                                        error:nil];
            [impl_->stream release];
            impl_->stream = nil;
        }
        if (impl_->output) {
            [impl_->output release];
            impl_->output = nil;
        }

        // Rebuild content filter + config from saved init args.
        SCContentFilter* filter = [[SCContentFilter alloc]
            initWithDisplay:impl_->display excludingWindows:@[]];
        SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
        cfg.width = width_;
        cfg.height = height_;
        cfg.minimumFrameInterval = CMTimeMake(1, (int32_t)impl_->saved_config.fps);
        cfg.showsCursor = impl_->saved_config.show_cursor ? YES : NO;
        cfg.queueDepth = 5;
        if (hdr_active_) {
            cfg.pixelFormat = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
            if (@available(macOS 13.0, *)) cfg.colorSpaceName = kCGColorSpaceITUR_2100_PQ;
        } else {
            cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            if (@available(macOS 13.0, *)) cfg.colorSpaceName = kCGColorSpaceSRGB;
        }

        impl_->output = [[DBSCStreamOutput alloc] init];
        impl_->output.mutex = &impl_->mutex;
        impl_->output.latestFrame = &impl_->latest_frame;
        impl_->output.latestPtsUs = &impl_->latest_pts_us;
        impl_->output.framesDelivered = &impl_->frames_delivered;
        impl_->output.cachedLastFrame = &impl_->cached_last_frame;
        impl_->output.cachedLastPtsUs = &impl_->cached_last_pts_us;
        impl_->output.parent = this;

        impl_->stream = [[SCStream alloc] initWithFilter:filter
                                            configuration:cfg
                                                 delegate:impl_->output];
        NSError* err = nil;
        BOOL add_ok = [impl_->stream addStreamOutput:impl_->output
                                                type:SCStreamOutputTypeScreen
                                  sampleHandlerQueue:impl_->queue
                                               error:&err];
        [filter release];
        [cfg release];

        if (!add_ok) {
            log::error(TAG, "Restart addStreamOutput failed: %s",
                       err ? [[err localizedDescription] UTF8String] : "unknown");
            this->schedule_restart_retry();
            return;
        }

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block BOOL start_ok = NO;
        __block NSError* start_err = nil;
        [impl_->stream startCaptureWithCompletionHandler:^(NSError* e) {
            start_err = e;
            start_ok = (e == nil);
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

        if (!start_ok) {
            log::error(TAG, "Restart startCapture failed: %s",
                start_err ? [[start_err localizedDescription] UTF8String] : "unknown");
            this->schedule_restart_retry();
            return;
        }

        impl_->restart_attempt = 0;
        log::info(TAG, "SCStream restart succeeded");
    });
    return true;
}

void MacScreenCapture::schedule_restart_retry() {
    // Exponential backoff: 250ms, 500ms, 1s, 2s, 4s, ..., cap at 10s.
    int attempt = ++impl_->restart_attempt;
    int64_t delay_ms = 250LL << std::min(attempt - 1, 6);  // 250 * 2^(attempt-1)
    if (delay_ms > 10'000) delay_ms = 10'000;
    log::warn(TAG, "Scheduling SCStream restart retry #%d in %lldms",
              attempt, (long long)delay_ms);
    MacScreenCapture* self_ptr = this;
    auto alive = impl_->alive;   // VIV-95: dispatch_after can't be cancelled
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * NSEC_PER_MSEC),
        impl_->control_q, ^{
        if (!alive->load()) return;   // capture was stopped/destroyed meanwhile
        self_ptr->restart_stream();
    });
}

bool MacScreenCapture::start() {
    if (!impl_->stream) return false;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool ok = false;
    __block NSError* start_err = nil;
    [impl_->stream startCaptureWithCompletionHandler:^(NSError* e) {
        start_err = e;
        ok = (e == nil);
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

    if (!ok) {
        log::error(TAG, "startCapture failed: %s",
            start_err ? [[start_err localizedDescription] UTF8String] : "unknown");
        return false;
    }
    log::info(TAG, "Capture started");
    return true;
}

uint32_t MacScreenCapture::display_id() const {
    // SCDisplay carries the CGDirectDisplayID the stream is bound to.
    return (impl_ && impl_->display) ? (uint32_t)impl_->display.displayID : 0;
}

void MacScreenCapture::stop() {
    if (!impl_) return;

    // VIV-95: restart_stream() tears down and rebuilds stream/output on
    // control_q.  stop() used to mutate the same pointers straight from the
    // calling thread, so a disconnect landing on an SCK error or a system wake
    // could double-release them (use-after-free crash).  Both paths now run on
    // control_q, which serialises them.
    //
    // Clearing `alive` first means any restart already queued (including the
    // uncancellable dispatch_after retry) turns into a no-op instead of
    // rebuilding a stream we are about to drop.
    impl_->alive->store(false);

    auto teardown = ^{
        if (impl_->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [impl_->stream stopCaptureWithCompletionHandler:^(NSError* /*e*/) {
                dispatch_semaphore_signal(sem);
            }];
            // NB: the semaphore is deliberately not released.  If the wait
            // times out and the completion handler fires afterwards it would
            // signal freed memory — a crash traded for a one-off leak.  Same
            // reasoning applies to the other timed waits in this file.
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            [impl_->stream release];  impl_->stream = nil;
        }
        if (impl_->output)  { [impl_->output release];  impl_->output = nil; }
        if (impl_->display) { [impl_->display release]; impl_->display = nil; }
    };

    // Already on control_q (a future caller inside a restart block): run
    // inline — dispatch_sync onto our own serial queue would deadlock.
    if (impl_->control_q && dispatch_get_specific(kControlQueueKey) == nullptr) {
        dispatch_sync(impl_->control_q, teardown);
    } else {
        teardown();
    }

    // VIV-95: the sample and control queues are dispatch_queue_create'd in a
    // non-ARC file and were never released — one pair leaked per capture
    // session.  Safe here because start() requires a live stream, so stop() is
    // terminal for this object.  Released after the teardown above so nothing
    // is still scheduled on them.
    if (impl_->queue)     { dispatch_release(impl_->queue);     impl_->queue = nullptr; }
    if (impl_->control_q) { dispatch_release(impl_->control_q); impl_->control_q = nullptr; }
}

CVPixelBufferRef MacScreenCapture::try_get_frame(uint64_t* out_pts_us) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->latest_frame) return nullptr;
    if (impl_->frames_delivered == impl_->last_pulled_count) return nullptr;

    CVPixelBufferRef pb = impl_->latest_frame;
    impl_->latest_frame = nullptr;
    impl_->last_pulled_count = impl_->frames_delivered;
    if (out_pts_us) *out_pts_us = impl_->latest_pts_us;
    return pb;  // caller takes ownership
}

CVPixelBufferRef MacScreenCapture::get_last_frame_for_force(uint64_t* out_pts_us) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->cached_last_frame) return nullptr;
    // Hand out an extra retain — the cached slot stays populated so the
    // next force call after another static interval still has something.
    CFRetain(impl_->cached_last_frame);
    if (out_pts_us) *out_pts_us = impl_->cached_last_pts_us;
    return impl_->cached_last_frame;
}

} // namespace vivora::host

#endif // VIVORA_MACOS
