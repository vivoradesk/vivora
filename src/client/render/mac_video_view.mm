#ifdef DESKBEAM_MACOS

#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <QuartzCore/QuartzCore.h>

#include "client/render/mac_video_view.h"
#include "common/utils/log.h"

#include <vector>

// ---------------------------------------------------------------------------
// Obj-C window delegate: signals main loop to exit on close.
// ---------------------------------------------------------------------------

@interface DBWindowDelegate : NSObject <NSWindowDelegate> {
@public
    bool* shouldClose;
}
@end

@implementation DBWindowDelegate
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (shouldClose) *shouldClose = true;
}
@end

// ---------------------------------------------------------------------------
// Obj-C view hosting the video layer.
// ---------------------------------------------------------------------------

@interface DBStreamView : NSView
@property (nonatomic, strong) AVSampleBufferDisplayLayer* videoLayer;
@end

@implementation DBStreamView
- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        _videoLayer = [[AVSampleBufferDisplayLayer alloc] init];
        _videoLayer.videoGravity = AVLayerVideoGravityResizeAspect;
        _videoLayer.backgroundColor = [[NSColor blackColor] CGColor];
        _videoLayer.frame = self.bounds;
        [self.layer addSublayer:_videoLayer];
    }
    return self;
}
- (BOOL)isOpaque { return YES; }
- (void)layout {
    [super layout];
    _videoLayer.frame = self.bounds;
}
@end

// ---------------------------------------------------------------------------
// C++ impl
// ---------------------------------------------------------------------------

namespace deskbeam {

static const char* TAG = "MAC_RENDER";

struct MacVideoViewImpl {
    NSWindow* window = nil;
    DBStreamView* view = nil;
    DBWindowDelegate* delegate = nil;

    CMVideoFormatDescriptionRef format_desc = nullptr;
    std::vector<uint8_t> vps, sps, pps;
    bool have_params = false;
    uint64_t frames_submitted = 0;
};

// Parse HEVC NAL unit type: bits [1..6] of first byte.
static int hevc_nal_type(const uint8_t* nal, size_t len) {
    if (len < 1) return -1;
    return (nal[0] >> 1) & 0x3F;
}

// Split Annex-B bitstream into NAL units. Each NAL unit data excludes the
// start code (00 00 00 01 or 00 00 01).
struct NalUnit {
    const uint8_t* data;
    size_t size;
};

static std::vector<NalUnit> split_annexb(const uint8_t* data, size_t total) {
    std::vector<NalUnit> out;
    size_t i = 0;

    // Find first start code
    auto find_sc = [&](size_t from) -> size_t {
        for (size_t j = from; j + 2 < total; ++j) {
            if (data[j] == 0 && data[j + 1] == 0) {
                if (data[j + 2] == 1) return j;                  // 3-byte start code
                if (j + 3 < total && data[j + 2] == 0 && data[j + 3] == 1) return j; // 4-byte
            }
        }
        return total;
    };

    i = find_sc(0);
    while (i < total) {
        // Skip start code
        size_t sc_len = (i + 3 < total && data[i + 2] == 0) ? 4 : 3;
        size_t nal_start = i + sc_len;
        size_t next = find_sc(nal_start);
        if (nal_start < next) {
            out.push_back({ data + nal_start, next - nal_start });
        }
        i = next;
    }
    return out;
}

// ---------------------------------------------------------------------------

MacVideoView::MacVideoView() {
    impl_ = new MacVideoViewImpl{};
}

MacVideoView::~MacVideoView() {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (impl) {
        if (impl->format_desc) CFRelease(impl->format_desc);
        // ARC releases Obj-C members when the struct is destroyed.
        impl->window = nil;
        impl->view = nil;
        impl->delegate = nil;
        delete impl;
    }
}

bool MacVideoView::create_window(const char* title, uint32_t width, uint32_t height) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        auto* impl = static_cast<MacVideoViewImpl*>(impl_);
        NSRect rect = NSMakeRect(100, 100, width, height);
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;

        impl->window = [[NSWindow alloc] initWithContentRect:rect
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [impl->window setTitle:[NSString stringWithUTF8String:title]];

        impl->delegate = [[DBWindowDelegate alloc] init];
        impl->delegate->shouldClose = &should_close_;
        [impl->window setDelegate:impl->delegate];

        impl->view = [[DBStreamView alloc] initWithFrame:rect];
        [impl->window setContentView:impl->view];

        [impl->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        log::info(TAG, "Window created: %ux%u", width, height);
    }
    return true;
}

void MacVideoView::pump_events() {
    @autoreleasepool {
        while (true) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                 untilDate:[NSDate distantPast]
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES];
            if (!event) break;
            [NSApp sendEvent:event];
        }
    }
}

bool MacVideoView::submit_frame(const uint8_t* data, size_t len, uint64_t pts_us, bool keyframe) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl->view) return false;

    auto nals = split_annexb(data, len);
    if (nals.empty()) return false;

    // Extract parameter sets from keyframes; build format description once we have
    // VPS+SPS+PPS.
    if (keyframe) {
        for (const auto& n : nals) {
            int t = hevc_nal_type(n.data, n.size);
            switch (t) {
                case 32: impl->vps.assign(n.data, n.data + n.size); break; // VPS_NUT
                case 33: impl->sps.assign(n.data, n.data + n.size); break; // SPS_NUT
                case 34: impl->pps.assign(n.data, n.data + n.size); break; // PPS_NUT
                default: break;
            }
        }

        if (!impl->vps.empty() && !impl->sps.empty() && !impl->pps.empty() && !impl->have_params) {
            if (impl->format_desc) {
                CFRelease(impl->format_desc);
                impl->format_desc = nullptr;
            }
            const uint8_t* params[3]   = { impl->vps.data(), impl->sps.data(), impl->pps.data() };
            const size_t   sizes[3]    = { impl->vps.size(), impl->sps.size(), impl->pps.size() };
            OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                kCFAllocatorDefault,
                3, params, sizes,
                4, // NAL unit header length (AVCC length prefix = 4 bytes)
                nullptr,
                &impl->format_desc);
            if (st != noErr) {
                log::error(TAG, "CMVideoFormatDescriptionCreateFromHEVCParameterSets failed: %d",
                           (int)st);
                return false;
            }
            impl->have_params = true;
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(impl->format_desc);
            log::info(TAG, "HEVC format description ready: %dx%d", dims.width, dims.height);
        }
    }

    if (!impl->have_params) return false;

    // Build the AVCC-format payload: concatenate non-parameter NAL units with
    // 4-byte big-endian length prefix instead of start codes.
    std::vector<uint8_t> avcc;
    avcc.reserve(len);
    for (const auto& n : nals) {
        int t = hevc_nal_type(n.data, n.size);
        if (t == 32 || t == 33 || t == 34) continue; // skip VPS/SPS/PPS
        uint32_t sz = static_cast<uint32_t>(n.size);
        avcc.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(sz & 0xFF));
        avcc.insert(avcc.end(), n.data, n.data + n.size);
    }
    if (avcc.empty()) return false;

    // Wrap in CMBlockBuffer (copy — layer keeps it until displayed).
    CMBlockBufferRef block = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        nullptr,                 // allocate internally
        avcc.size(),
        kCFAllocatorDefault,
        nullptr,
        0, avcc.size(),
        0,
        &block);
    if (st != noErr || !block) {
        log::error(TAG, "CMBlockBufferCreateWithMemoryBlock failed: %d", (int)st);
        return false;
    }
    st = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
    if (st != noErr) {
        log::error(TAG, "CMBlockBufferReplaceDataBytes failed: %d", (int)st);
        CFRelease(block);
        return false;
    }

    // Timing info — PTS in microseconds.
    CMSampleTimingInfo timing = {};
    timing.duration              = kCMTimeInvalid;
    timing.presentationTimeStamp = CMTimeMake(static_cast<int64_t>(pts_us), 1'000'000);
    timing.decodeTimeStamp       = kCMTimeInvalid;

    size_t sizes_arr[1] = { avcc.size() };
    CMSampleBufferRef sample = nullptr;
    st = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        block,
        impl->format_desc,
        1,    // num samples
        1, &timing,
        1, sizes_arr,
        &sample);
    CFRelease(block);
    if (st != noErr || !sample) {
        log::error(TAG, "CMSampleBufferCreateReady failed: %d", (int)st);
        return false;
    }

    // Mark non-keyframes as depending on prior; hint to display immediately.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFMutableDictionaryRef dict =
            (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
        if (!keyframe) {
            CFDictionarySetValue(dict, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
        }
    }

    AVSampleBufferVideoRenderer* renderer = impl->view.videoLayer.sampleBufferRenderer;
    if (renderer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        log::warn(TAG, "Renderer in failed state, flushing");
        [renderer flush];
    }
    [renderer enqueueSampleBuffer:sample];
    CFRelease(sample);

    impl->frames_submitted++;
    return true;
}

} // namespace deskbeam

#endif // DESKBEAM_MACOS
