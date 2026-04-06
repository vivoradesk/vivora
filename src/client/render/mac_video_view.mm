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
#include <functional>

// Device-dependent modifier flag bits (from IOKit, stable ABI).
#define DB_NX_DEVICELCTLKEYMASK   0x00000001
#define DB_NX_DEVICELSHIFTKEYMASK 0x00000002
#define DB_NX_DEVICERSHIFTKEYMASK 0x00000004
#define DB_NX_DEVICELCMDKEYMASK   0x00000008
#define DB_NX_DEVICERCMDKEYMASK   0x00000010
#define DB_NX_DEVICELALTKEYMASK   0x00000020
#define DB_NX_DEVICERALTKEYMASK   0x00000040
#define DB_NX_DEVICERCTLKEYMASK   0x00002000

namespace deskbeam { struct MacVideoViewImpl; }

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

@interface DBStreamView : NSView {
@public
    deskbeam::MacVideoViewImpl* impl;  // back pointer for input callback
    NSTrackingArea* trackingArea;
    NSUInteger lastModifierFlags;
}
@property (nonatomic, strong) AVSampleBufferDisplayLayer* videoLayer;
@end

// Forward declaration so the view can call into C++.
namespace deskbeam {
static void emit_input(MacVideoViewImpl* impl, const protocol::InputEvent& ev);
// Returns true if a mapping exists. Extended-key handling relies on vk_code
// on the Windows injector side (arrows, nav keys, etc.).
static bool mac_key_to_win(uint16_t mac_kc, uint16_t* out_scan, uint16_t* out_vk);
}

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
        lastModifierFlags = 0;
    }
    return self;
}
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)layout {
    [super layout];
    _videoLayer.frame = self.bounds;
}

- (void)updateTrackingAreas {
    if (trackingArea) {
        [self removeTrackingArea:trackingArea];
        trackingArea = nil;
    }
    NSTrackingAreaOptions opts = NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                 NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited;
    trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                options:opts
                                                  owner:self
                                               userInfo:nil];
    [self addTrackingArea:trackingArea];
    [super updateTrackingAreas];
}

- (void)sendMouseMove:(NSEvent*)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat w = self.bounds.size.width;
    CGFloat h = self.bounds.size.height;
    if (w <= 0 || h <= 0) return;
    // NSView default coord: origin bottom-left. Flip Y.
    float xn = (float)(p.x / w);
    float yn = (float)((h - p.y) / h);
    if (xn < 0) xn = 0; if (xn > 1) xn = 1;
    if (yn < 0) yn = 0; if (yn > 1) yn = 1;

    deskbeam::protocol::InputEvent ev;
    ev.type = deskbeam::protocol::InputEventType::MouseMove;
    ev.x_norm = xn;
    ev.y_norm = yn;
    deskbeam::emit_input(impl, ev);
}

- (void)mouseMoved:(NSEvent*)event        { [self sendMouseMove:event]; }
- (void)mouseDragged:(NSEvent*)event      { [self sendMouseMove:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendMouseMove:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendMouseMove:event]; }

- (void)sendMouseButton:(deskbeam::protocol::MouseButton)btn pressed:(BOOL)down {
    deskbeam::protocol::InputEvent ev;
    ev.type = deskbeam::protocol::InputEventType::MouseButton;
    ev.button = btn;
    ev.pressed = down ? true : false;
    deskbeam::emit_input(impl, ev);
}

- (void)mouseDown:(NSEvent*)event        { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Left   pressed:YES]; }
- (void)mouseUp:(NSEvent*)event          { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Left   pressed:NO];  }
- (void)rightMouseDown:(NSEvent*)event   { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Right  pressed:YES]; }
- (void)rightMouseUp:(NSEvent*)event     { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Right  pressed:NO];  }
- (void)otherMouseDown:(NSEvent*)event   { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Middle pressed:YES]; }
- (void)otherMouseUp:(NSEvent*)event     { (void)event; [self sendMouseButton:deskbeam::protocol::MouseButton::Middle pressed:NO];  }

- (void)scrollWheel:(NSEvent*)event {
    // Mac scroll deltas are in lines (or pixels for precise scrolling).
    // Windows wheel uses 120 per notch. Multiply by 30 as a reasonable default.
    CGFloat dx = event.scrollingDeltaX;
    CGFloat dy = event.scrollingDeltaY;
    int16_t sx = (int16_t)std::max(-32000.0, std::min(32000.0, (double)dx * 30.0));
    int16_t sy = (int16_t)std::max(-32000.0, std::min(32000.0, (double)dy * 30.0));
    if (sx == 0 && sy == 0) return;

    deskbeam::protocol::InputEvent ev;
    ev.type = deskbeam::protocol::InputEventType::MouseScroll;
    ev.scroll_dx = sx;
    ev.scroll_dy = sy;
    deskbeam::emit_input(impl, ev);
}

- (void)sendKey:(uint16_t)macKeyCode down:(BOOL)down {
    uint16_t scan = 0, vk = 0;
    if (!deskbeam::mac_key_to_win(macKeyCode, &scan, &vk)) return;

    deskbeam::protocol::InputEvent ev;
    ev.type = down ? deskbeam::protocol::InputEventType::KeyDown
                   : deskbeam::protocol::InputEventType::KeyUp;
    ev.scan_code = scan;
    ev.vk_code = vk;
    deskbeam::emit_input(impl, ev);
}

- (void)keyDown:(NSEvent*)event {
    if (event.isARepeat) return; // host handles auto-repeat
    [self sendKey:event.keyCode down:YES];
}
- (void)keyUp:(NSEvent*)event {
    [self sendKey:event.keyCode down:NO];
}

- (void)flagsChanged:(NSEvent*)event {
    NSUInteger newFlags = event.modifierFlags;
    uint16_t kc = event.keyCode;
    // Decide down/up from the device-specific bit belonging to this keyCode.
    NSUInteger mask = 0;
    switch (kc) {
        case 0x38: mask = DB_NX_DEVICELSHIFTKEYMASK; break; // LShift
        case 0x3C: mask = DB_NX_DEVICERSHIFTKEYMASK; break; // RShift
        case 0x3B: mask = DB_NX_DEVICELCTLKEYMASK;   break; // LControl
        case 0x3E: mask = DB_NX_DEVICERCTLKEYMASK;   break; // RControl
        case 0x3A: mask = DB_NX_DEVICELALTKEYMASK;   break; // LOption
        case 0x3D: mask = DB_NX_DEVICERALTKEYMASK;   break; // ROption
        case 0x37: mask = DB_NX_DEVICELCMDKEYMASK;   break; // LCommand
        case 0x36: mask = DB_NX_DEVICERCMDKEYMASK;   break; // RCommand
        case 0x39: {
            // CapsLock toggles — emit synthesized down+up on each change.
            [self sendKey:kc down:YES];
            [self sendKey:kc down:NO];
            lastModifierFlags = newFlags;
            return;
        }
        default: break;
    }
    if (mask == 0) {
        lastModifierFlags = newFlags;
        return;
    }
    BOOL isDown = (newFlags & mask) != 0;
    [self sendKey:kc down:isDown];
    lastModifierFlags = newFlags;
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

    MacVideoView::InputCallback input_cb;
};

static void emit_input(MacVideoViewImpl* impl, const protocol::InputEvent& ev) {
    if (impl && impl->input_cb) impl->input_cb(ev);
}

// Mac kVK_ → (Windows PS/2 set 1 scan code, Windows VK).
// Scan codes are the low-byte form; the Windows injector adds the E0 prefix
// automatically for extended keys based on the VK.
static bool mac_key_to_win(uint16_t mac_kc, uint16_t* out_scan, uint16_t* out_vk) {
    uint16_t scan = 0, vk = 0;
    switch (mac_kc) {
        // Letters
        case 0x00: scan = 0x1E; vk = 0x41; break; // A
        case 0x01: scan = 0x1F; vk = 0x53; break; // S
        case 0x02: scan = 0x20; vk = 0x44; break; // D
        case 0x03: scan = 0x21; vk = 0x46; break; // F
        case 0x04: scan = 0x23; vk = 0x48; break; // H
        case 0x05: scan = 0x22; vk = 0x47; break; // G
        case 0x06: scan = 0x2C; vk = 0x5A; break; // Z
        case 0x07: scan = 0x2D; vk = 0x58; break; // X
        case 0x08: scan = 0x2E; vk = 0x43; break; // C
        case 0x09: scan = 0x2F; vk = 0x56; break; // V
        case 0x0B: scan = 0x30; vk = 0x42; break; // B
        case 0x0C: scan = 0x10; vk = 0x51; break; // Q
        case 0x0D: scan = 0x11; vk = 0x57; break; // W
        case 0x0E: scan = 0x12; vk = 0x45; break; // E
        case 0x0F: scan = 0x13; vk = 0x52; break; // R
        case 0x10: scan = 0x15; vk = 0x59; break; // Y
        case 0x11: scan = 0x14; vk = 0x54; break; // T
        case 0x1F: scan = 0x18; vk = 0x4F; break; // O
        case 0x20: scan = 0x16; vk = 0x55; break; // U
        case 0x22: scan = 0x17; vk = 0x49; break; // I
        case 0x23: scan = 0x19; vk = 0x50; break; // P
        case 0x25: scan = 0x26; vk = 0x4C; break; // L
        case 0x26: scan = 0x24; vk = 0x4A; break; // J
        case 0x28: scan = 0x25; vk = 0x4B; break; // K
        case 0x2D: scan = 0x31; vk = 0x4E; break; // N
        case 0x2E: scan = 0x32; vk = 0x4D; break; // M

        // Digits (top row)
        case 0x12: scan = 0x02; vk = 0x31; break; // 1
        case 0x13: scan = 0x03; vk = 0x32; break; // 2
        case 0x14: scan = 0x04; vk = 0x33; break; // 3
        case 0x15: scan = 0x05; vk = 0x34; break; // 4
        case 0x17: scan = 0x06; vk = 0x35; break; // 5
        case 0x16: scan = 0x07; vk = 0x36; break; // 6
        case 0x1A: scan = 0x08; vk = 0x37; break; // 7
        case 0x1C: scan = 0x09; vk = 0x38; break; // 8
        case 0x19: scan = 0x0A; vk = 0x39; break; // 9
        case 0x1D: scan = 0x0B; vk = 0x30; break; // 0

        // Punctuation
        case 0x1B: scan = 0x0C; vk = 0xBD; break; // - _
        case 0x18: scan = 0x0D; vk = 0xBB; break; // = +
        case 0x21: scan = 0x1A; vk = 0xDB; break; // [ {
        case 0x1E: scan = 0x1B; vk = 0xDD; break; // ] }
        case 0x2A: scan = 0x2B; vk = 0xDC; break; // backslash |
        case 0x29: scan = 0x27; vk = 0xBA; break; // ; :
        case 0x27: scan = 0x28; vk = 0xDE; break; // ' "
        case 0x32: scan = 0x29; vk = 0xC0; break; // ` ~
        case 0x2B: scan = 0x33; vk = 0xBC; break; // , <
        case 0x2F: scan = 0x34; vk = 0xBE; break; // . >
        case 0x2C: scan = 0x35; vk = 0xBF; break; // / ?

        // Control
        case 0x24: scan = 0x1C; vk = 0x0D; break; // Return → Enter
        case 0x30: scan = 0x0F; vk = 0x09; break; // Tab
        case 0x31: scan = 0x39; vk = 0x20; break; // Space
        case 0x33: scan = 0x0E; vk = 0x08; break; // Delete → Backspace
        case 0x35: scan = 0x01; vk = 0x1B; break; // Escape

        // Navigation (extended)
        case 0x75: scan = 0x53; vk = 0x2E; break; // Fwd Delete
        case 0x73: scan = 0x47; vk = 0x24; break; // Home
        case 0x77: scan = 0x4F; vk = 0x23; break; // End
        case 0x74: scan = 0x49; vk = 0x21; break; // PageUp
        case 0x79: scan = 0x51; vk = 0x22; break; // PageDown
        case 0x7B: scan = 0x4B; vk = 0x25; break; // Left
        case 0x7C: scan = 0x4D; vk = 0x27; break; // Right
        case 0x7D: scan = 0x50; vk = 0x28; break; // Down
        case 0x7E: scan = 0x48; vk = 0x26; break; // Up

        // Modifiers
        case 0x38: scan = 0x2A; vk = 0xA0; break; // LShift
        case 0x3C: scan = 0x36; vk = 0xA1; break; // RShift
        case 0x3B: scan = 0x1D; vk = 0xA2; break; // LControl
        case 0x3E: scan = 0x1D; vk = 0xA3; break; // RControl
        case 0x3A: scan = 0x38; vk = 0xA4; break; // LOption → LAlt
        case 0x3D: scan = 0x38; vk = 0xA5; break; // ROption → RAlt
        case 0x37: scan = 0x5B; vk = 0x5B; break; // LCommand → LWin
        case 0x36: scan = 0x5C; vk = 0x5C; break; // RCommand → RWin
        case 0x39: scan = 0x3A; vk = 0x14; break; // CapsLock

        // F-keys
        case 0x7A: scan = 0x3B; vk = 0x70; break; // F1
        case 0x78: scan = 0x3C; vk = 0x71; break; // F2
        case 0x63: scan = 0x3D; vk = 0x72; break; // F3
        case 0x76: scan = 0x3E; vk = 0x73; break; // F4
        case 0x60: scan = 0x3F; vk = 0x74; break; // F5
        case 0x61: scan = 0x40; vk = 0x75; break; // F6
        case 0x62: scan = 0x41; vk = 0x76; break; // F7
        case 0x64: scan = 0x42; vk = 0x77; break; // F8
        case 0x65: scan = 0x43; vk = 0x78; break; // F9
        case 0x6D: scan = 0x44; vk = 0x79; break; // F10
        case 0x67: scan = 0x57; vk = 0x7A; break; // F11
        case 0x6F: scan = 0x58; vk = 0x7B; break; // F12

        default: return false;
    }
    *out_scan = scan;
    *out_vk = vk;
    return true;
}

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
        impl->view->impl = impl;
        [impl->window setContentView:impl->view];
        [impl->window makeFirstResponder:impl->view];
        [impl->window setAcceptsMouseMovedEvents:YES];

        [impl->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        log::info(TAG, "Window created: %ux%u", width, height);
    }
    return true;
}

void MacVideoView::set_input_callback(InputCallback cb) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    impl->input_cb = std::move(cb);
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
