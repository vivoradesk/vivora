#ifdef VIVORA_MACOS

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
#include <unordered_map>
#include <cmath>

// Device-dependent modifier flag bits (from IOKit, stable ABI).
#define DB_NX_DEVICELCTLKEYMASK   0x00000001
#define DB_NX_DEVICELSHIFTKEYMASK 0x00000002
#define DB_NX_DEVICERSHIFTKEYMASK 0x00000004
#define DB_NX_DEVICELCMDKEYMASK   0x00000008
#define DB_NX_DEVICERCMDKEYMASK   0x00000010
#define DB_NX_DEVICELALTKEYMASK   0x00000020
#define DB_NX_DEVICERALTKEYMASK   0x00000040
#define DB_NX_DEVICERCTLKEYMASK   0x00002000

namespace vivora { struct MacVideoViewImpl; }

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
    vivora::MacVideoViewImpl* impl;  // back pointer for input callback
    NSTrackingArea* trackingArea;
    NSUInteger lastModifierFlags;
    // Relative-mouse mode: on when host cursor is hidden (e.g. FPS game).
    // In this mode mouseMoved/mouseDragged forward raw hardware deltas via
    // NSEvent.deltaX/deltaY as MouseMoveRelative, and the OS cursor is
    // decoupled from physical motion so the trackpad keeps producing deltas
    // even when the virtual cursor would hit a window edge.
    BOOL relativeMode;
}
@property (nonatomic, strong) AVSampleBufferDisplayLayer* videoLayer;
@property (nonatomic, strong) CALayer* cursorLayer;
@property (nonatomic, strong) CATextLayer* hudLayer;
@property (nonatomic, assign) BOOL hudVisible;
@property (nonatomic, strong) CATextLayer* statusLayer;
- (void)enterRelativeMode;
- (void)exitRelativeMode;
- (void)toggleHud;
- (void)setHudText:(NSString*)text;
- (void)layoutHud;
- (void)setStatusText:(NSString*)text;
- (void)layoutStatus;
@end

// Forward declaration so the view can call into C++.
namespace vivora {
static void emit_input(MacVideoViewImpl* impl, const protocol::InputEvent& ev);
// Returns true if a mapping exists. Extended-key handling relies on vk_code
// on the Windows injector side (arrows, nav keys, etc.).
static bool mac_key_to_win(uint16_t mac_kc, uint16_t* out_scan, uint16_t* out_vk);
// Compute video-space normalized coords accounting for letterbox/pillarbox.
// px, py are in view space with top-left origin.
static void normalize_mouse(MacVideoViewImpl* impl, double px, double py,
                            double vw, double vh, float* out_xn, float* out_yn);
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

        _cursorLayer = [[CALayer alloc] init];
        _cursorLayer.hidden = YES;
        _cursorLayer.contentsGravity = kCAGravityResize;
        [self.layer addSublayer:_cursorLayer];

        // F9 diagnostics HUD: CATextLayer composes natively over the video
        // layer (no NSTextField z-order issues with AVSampleBufferDisplayLayer).
        // Hidden by default; geometry recomputed in -layoutHud on each text
        // change so it sticks to the top-right of the view.
        _hudLayer = [[CATextLayer alloc] init];
        _hudLayer.hidden = YES;
        _hudLayer.font = (__bridge CFTypeRef)[NSFont fontWithName:@"Menlo" size:12]
                       ?: (__bridge CFTypeRef)[NSFont userFixedPitchFontOfSize:12];
        _hudLayer.fontSize = 12.0;
        _hudLayer.foregroundColor = [[NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.9 alpha:1.0] CGColor];
        _hudLayer.backgroundColor = [[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.43] CGColor];
        _hudLayer.cornerRadius = 6.0;
        _hudLayer.alignmentMode = kCAAlignmentLeft;
        _hudLayer.contentsScale = self.window.backingScaleFactor ?: 2.0;
        _hudLayer.string = @"HUD ready (F9)";
        [self.layer addSublayer:_hudLayer];
        _hudVisible = NO;

        // Centred status overlay (VIV-62) — "Connecting…" / "Waiting for host
        // to accept…" before the first frame.  Hidden until set_status.
        _statusLayer = [[CATextLayer alloc] init];
        _statusLayer.hidden = YES;
        _statusLayer.font = (__bridge CFTypeRef)[NSFont systemFontOfSize:15];
        _statusLayer.fontSize = 15.0;
        _statusLayer.foregroundColor = [[NSColor colorWithCalibratedRed:0.94 green:0.94 blue:0.94 alpha:1.0] CGColor];
        _statusLayer.backgroundColor = [[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.55] CGColor];
        _statusLayer.cornerRadius = 8.0;
        _statusLayer.alignmentMode = kCAAlignmentCenter;
        _statusLayer.contentsScale = self.window.backingScaleFactor ?: 2.0;
        [self.layer addSublayer:_statusLayer];

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
    [self layoutHud];
    [self layoutStatus];
}

- (void)toggleHud {
    self.hudVisible = !self.hudVisible;
    self.hudLayer.hidden = !self.hudVisible;
    if (self.hudVisible) [self layoutHud];
}

- (void)setHudText:(NSString*)text {
    if (!text) text = @"";
    [CATransaction begin];
    [CATransaction setDisableActions:YES];   // no implicit fade on every refresh
    self.hudLayer.string = text;
    [self layoutHud];
    [CATransaction commit];
}

- (void)layoutHud {
    if (!self.hudLayer || self.hudLayer.hidden) return;
    NSString* s = (NSString*)self.hudLayer.string ?: @"";
    NSDictionary* attrs = @{ NSFontAttributeName: (__bridge id)self.hudLayer.font ?: [NSFont userFixedPitchFontOfSize:12] };
    NSSize text_size = [s sizeWithAttributes:attrs];
    const CGFloat pad = 8.0;
    const CGFloat margin = 12.0;
    CGFloat w = ceil(text_size.width)  + pad * 2;
    CGFloat h = ceil(text_size.height) + pad * 2;
    // Anchor top-right.  NSView coords are bottom-up by default for backing
    // layers, so the y origin is (height - hud_h - margin).
    CGFloat x = self.bounds.size.width  - w - margin;
    CGFloat y = self.bounds.size.height - h - margin;
    self.hudLayer.frame = CGRectMake(x, y, w, h);
}

- (void)setStatusText:(NSString*)text {
    if (!text) text = @"";
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (text.length == 0) {
        self.statusLayer.hidden = YES;
    } else {
        self.statusLayer.string = text;
        self.statusLayer.hidden = NO;
        [self layoutStatus];
    }
    [CATransaction commit];
}

- (void)layoutStatus {
    if (!self.statusLayer || self.statusLayer.hidden) return;
    NSString* s = (NSString*)self.statusLayer.string ?: @"";
    NSDictionary* attrs = @{ NSFontAttributeName: (__bridge id)self.statusLayer.font ?: [NSFont systemFontOfSize:15] };
    NSSize ts = [s sizeWithAttributes:attrs];
    const CGFloat padx = 22.0, pady = 14.0;
    CGFloat w = ceil(ts.width)  + padx * 2;
    CGFloat h = ceil(ts.height) + pady * 2;
    CGFloat x = (self.bounds.size.width  - w) / 2.0;
    CGFloat y = (self.bounds.size.height - h) / 2.0;
    self.statusLayer.frame = CGRectMake(x, y, w, h);
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
    if (relativeMode) {
        // Raw HID deltas — decoupled from cursor clamping by
        // CGAssociateMouseAndMouseCursorPosition(false). Matches the
        // Windows client's WM_INPUT path (RAWMOUSE.lLastX/lLastY).
        // Mac reports sub-pixel float deltas on trackpads; round to int.
        CGFloat dx = event.deltaX;
        CGFloat dy = event.deltaY;
        if (dx == 0 && dy == 0) return;
        vivora::protocol::InputEvent ev;
        ev.type = vivora::protocol::InputEventType::MouseMoveRelative;
        ev.dx = static_cast<int32_t>(llround(dx));
        ev.dy = static_cast<int32_t>(llround(dy));
        if (ev.dx == 0 && ev.dy == 0) return;
        vivora::emit_input(impl, ev);
        return;
    }

    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat vw = self.bounds.size.width;
    CGFloat vh = self.bounds.size.height;
    if (vw <= 0 || vh <= 0 || !impl) return;

    float xn = 0, yn = 0;
    vivora::normalize_mouse(impl, p.x, vh - p.y, vw, vh, &xn, &yn);

    vivora::protocol::InputEvent ev;
    ev.type = vivora::protocol::InputEventType::MouseMove;
    ev.x_norm = xn;
    ev.y_norm = yn;
    vivora::emit_input(impl, ev);
}

- (void)enterRelativeMode {
    if (relativeMode) return;
    relativeMode = YES;
    // Decouple OS cursor from physical motion — trackpad/mouse events still
    // carry raw deltas on NSEvent.deltaX/deltaY, but the Mac cursor stops
    // moving, so we never hit a screen edge that would kill further deltas.
    // Do NOT warp the cursor: when host flips visibility briefly (UI hover),
    // any warp would teleport the user's mouse on each flip.
    // Cursor visibility is managed by the tracking-area enter/exit pair —
    // calling [NSCursor hide] here would unbalance that counter.
    CGAssociateMouseAndMouseCursorPosition(false);
}

- (void)exitRelativeMode {
    if (!relativeMode) return;
    relativeMode = NO;
    CGAssociateMouseAndMouseCursorPosition(true);
}

- (void)mouseMoved:(NSEvent*)event        { [self sendMouseMove:event]; }
- (void)mouseDragged:(NSEvent*)event      { [self sendMouseMove:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self sendMouseMove:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self sendMouseMove:event]; }

// Hide native Mac cursor while over the stream — host-side cursor is
// drawn into the video via cursorLayer, so we'd otherwise see two.
- (void)mouseEntered:(NSEvent*)event { (void)event; [NSCursor hide]; }
- (void)mouseExited:(NSEvent*)event  { (void)event; [NSCursor unhide]; }

- (void)sendMouseButton:(vivora::protocol::MouseButton)btn pressed:(BOOL)down {
    vivora::protocol::InputEvent ev;
    ev.type = vivora::protocol::InputEventType::MouseButton;
    ev.button = btn;
    ev.pressed = down ? true : false;
    vivora::emit_input(impl, ev);
}

- (void)mouseDown:(NSEvent*)event        { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Left   pressed:YES]; }
- (void)mouseUp:(NSEvent*)event          { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Left   pressed:NO];  }
- (void)rightMouseDown:(NSEvent*)event   { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Right  pressed:YES]; }
- (void)rightMouseUp:(NSEvent*)event     { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Right  pressed:NO];  }
- (void)otherMouseDown:(NSEvent*)event   { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Middle pressed:YES]; }
- (void)otherMouseUp:(NSEvent*)event     { (void)event; [self sendMouseButton:vivora::protocol::MouseButton::Middle pressed:NO];  }

- (void)scrollWheel:(NSEvent*)event {
    // Mac scroll deltas are in lines (or pixels for precise scrolling).
    // Windows wheel uses 120 per notch. Multiply by 30 as a reasonable default.
    CGFloat dx = event.scrollingDeltaX;
    CGFloat dy = event.scrollingDeltaY;
    int16_t sx = (int16_t)std::max(-32000.0, std::min(32000.0, (double)dx * 30.0));
    int16_t sy = (int16_t)std::max(-32000.0, std::min(32000.0, (double)dy * 30.0));
    if (sx == 0 && sy == 0) return;

    vivora::protocol::InputEvent ev;
    ev.type = vivora::protocol::InputEventType::MouseScroll;
    ev.scroll_dx = sx;
    ev.scroll_dy = sy;
    vivora::emit_input(impl, ev);
}

- (void)sendKey:(uint16_t)macKeyCode down:(BOOL)down {
    uint16_t scan = 0, vk = 0;
    if (!vivora::mac_key_to_win(macKeyCode, &scan, &vk)) return;

    vivora::protocol::InputEvent ev;
    ev.type = down ? vivora::protocol::InputEventType::KeyDown
                   : vivora::protocol::InputEventType::KeyUp;
    ev.scan_code = scan;
    ev.vk_code = vk;
    vivora::emit_input(impl, ev);
}

- (void)keyDown:(NSEvent*)event {
    if (event.isARepeat) return; // host handles auto-repeat
    // F9 (mac kVK 0x65) toggles the diagnostics HUD locally.  Eat it so
    // the host doesn't see a phantom keypress.
    if (event.keyCode == 0x65) { [self toggleHud]; return; }
    [self sendKey:event.keyCode down:YES];
}
- (void)keyUp:(NSEvent*)event {
    if (event.keyCode == 0x65) return; // local toggle, don't forward
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

namespace vivora {

static const char* TAG = "MAC_RENDER";

struct CursorShapeEntry {
    std::vector<uint8_t> bgra;  // owned backing store for the CGImage
    CGImageRef image = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t hotspot_x = 0;
    uint16_t hotspot_y = 0;
};

struct MacVideoViewImpl {
    NSWindow* window = nil;
    DBStreamView* view = nil;
    DBWindowDelegate* delegate = nil;

    CMVideoFormatDescriptionRef format_desc = nullptr;
    std::vector<uint8_t> vps, sps, pps;
    bool have_params = false;
    uint64_t frames_submitted = 0;
    uint32_t host_w = 0;
    uint32_t host_h = 0;
    VideoCodec codec = VideoCodec::HEVC;
    // True if the active format description carries a PQ or HLG transfer
    // function — surfaced through update_stats so the F9 HUD reports HDR.
    // Detected from kCMFormatDescriptionExtension_TransferFunction once
    // the CMVideoFormatDescription is created from VPS/SPS/PPS.
    bool is_hdr = false;

    // Cursor cache: shape_id -> CGImage (+ owned backing BGRA buffer).
    std::unordered_map<uint32_t, CursorShapeEntry> cursor_shapes;

    MacVideoView::InputCallback input_cb;
};

static void emit_input(MacVideoViewImpl* impl, const protocol::InputEvent& ev) {
    if (impl && impl->input_cb) impl->input_cb(ev);
}

static void normalize_mouse(MacVideoViewImpl* impl, double px, double py,
                            double vw, double vh, float* out_xn, float* out_yn) {
    float xn, yn;
    if (impl && impl->host_w > 0 && impl->host_h > 0) {
        // AVLayerVideoGravityResizeAspect — fit preserving aspect ratio.
        double host_aspect = (double)impl->host_w / (double)impl->host_h;
        double view_aspect = vw / vh;
        double video_w, video_h, off_x, off_y;
        if (view_aspect > host_aspect) {
            // Pillarbox: bars on left/right.
            video_h = vh;
            video_w = vh * host_aspect;
            off_x = (vw - video_w) * 0.5;
            off_y = 0;
        } else {
            // Letterbox: bars on top/bottom.
            video_w = vw;
            video_h = vw / host_aspect;
            off_x = 0;
            off_y = (vh - video_h) * 0.5;
        }
        xn = (float)((px - off_x) / video_w);
        yn = (float)((py - off_y) / video_h);
    } else {
        xn = (float)(px / vw);
        yn = (float)(py / vh);
    }
    if (xn < 0) xn = 0; if (xn > 1) xn = 1;
    if (yn < 0) yn = 0; if (yn > 1) yn = 1;
    *out_xn = xn;
    *out_yn = yn;
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

// Parse H.264 NAL unit type: low 5 bits of first byte.
static int h264_nal_type(const uint8_t* nal, size_t len) {
    if (len < 1) return -1;
    return nal[0] & 0x1F;
}

// HEVC NAL types of interest.
static constexpr int HEVC_NAL_VPS = 32;
static constexpr int HEVC_NAL_SPS = 33;
static constexpr int HEVC_NAL_PPS = 34;

// H.264 NAL types of interest.
static constexpr int H264_NAL_SPS = 7;
static constexpr int H264_NAL_PPS = 8;

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
        // Defensive teardown — VIV-56.  Closing the window after the
        // host went away occasionally crashes in NSEventThread CFRetain
        // on an event being pushed to our CGEventQueue while half-torn
        // objects are still associated.  Restore CG state to defaults
        // BEFORE releasing the NS objects so AppKit's event dispatch
        // doesn't see a half-bound cursor mode.
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];   // balance any pending mouseEntered hide
        // Detach the delegate so its windowWillClose: can't fire on a
        // half-released window callback chain.
        if (impl->window) [impl->window setDelegate:nil];
        if (impl->format_desc) CFRelease(impl->format_desc);
        for (auto& kv : impl->cursor_shapes) {
            if (kv.second.image) CGImageRelease(kv.second.image);
        }
        impl->cursor_shapes.clear();
        // ARC alone leaves the NSWindow up because NSApp keeps it in
        // its window list — explicit -close removes it from the
        // screen and the global list so the window disappears the
        // moment the session ends (host timeout, manual disconnect).
        // Matches the Windows StreamWindow behaviour where the window
        // tears down as soon as ViewSession::onTick drops the
        // platform.  Without this the user sees a frozen frame
        // indefinitely after the host went away.
        if (impl->window) [impl->window close];
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

void MacVideoView::set_codec(VideoCodec codec) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    impl->codec = codec;
}

void MacVideoView::flush_decoder() {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl) return;
    @autoreleasepool {
        if (impl->view && impl->view.videoLayer) {
            AVSampleBufferVideoRenderer* renderer = impl->view.videoLayer.sampleBufferRenderer;
            [renderer flush];
        }
    }
    if (impl->format_desc) {
        CFRelease(impl->format_desc);
        impl->format_desc = nullptr;
    }
    impl->vps.clear();
    impl->sps.clear();
    impl->pps.clear();
    impl->have_params = false;
}

void MacVideoView::set_stream_size(uint32_t width, uint32_t height) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl || width == 0 || height == 0) return;
    impl->host_w = width;
    impl->host_h = height;
}

void MacVideoView::update_stats(const StatsView& stats) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl || !impl->view) return;
    @autoreleasepool {
        // view_loop doesn't know HDR-ness — use the flag detected from the
        // active CMVideoFormatDescription's transfer function.
        const bool hdr = impl->is_hdr;
        NSString* txt = [NSString stringWithFormat:
            @"FPS:    %5.1f decoded / %5.1f arrived / target %u\n"
             "RTT:    %5.1f ms   Bitrate: %u kbps\n"
             "Reject: %5.2f%% (%llu)   Drop: %5.2f%% (%llu)\n"
             "Audio:  %u pps   PLC %u%%\n"
             "FEC:    %llu recovered / %llu failed\n"
             "Stream: %ux%u%s\n"
             "Decoder: VTB HW",
            stats.fps, stats.arrived_fps, (unsigned)stats.target_fps,
            stats.rtt_ms, (unsigned)stats.bitrate_kbps,
            stats.reject_pct, (unsigned long long)stats.total_rejected,
            stats.drop_pct,   (unsigned long long)stats.total_dropped,
            (unsigned)stats.audio_pps, (unsigned)stats.plc_pct,
            (unsigned long long)stats.fec_recovered,
            (unsigned long long)stats.fec_groups_failed,
            (unsigned)stats.width, (unsigned)stats.height,
            hdr ? " HDR" : ""];
        [impl->view setHudText:txt];
    }
}

void MacVideoView::set_status(const char* text) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl || !impl->view) return;
    @autoreleasepool {
        NSString* s = text ? [NSString stringWithUTF8String:text] : @"";
        [impl->view setStatusText:s];
    }
}

void MacVideoView::upload_cursor_shape(const protocol::CursorShapeMessage& shape) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl || shape.width == 0 || shape.height == 0) return;
    const size_t expected = (size_t)shape.width * shape.height * 4;
    if (shape.bgra.size() < expected) return;

    auto it = impl->cursor_shapes.find(shape.shape_id);
    if (it != impl->cursor_shapes.end()) {
        // Same id already cached — no rebuild.
        return;
    }

    CursorShapeEntry entry;
    entry.width     = shape.width;
    entry.height    = shape.height;
    entry.hotspot_x = shape.hotspot_x;
    entry.hotspot_y = shape.hotspot_y;
    entry.bgra      = shape.bgra;  // own the pixels; CGDataProvider aliases.

    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr, entry.bgra.data(), expected, nullptr);
    if (!provider) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) { CGDataProviderRelease(provider); return; }

    // BGRA bytes read as little-endian 32 → 0xAARRGGBB → PremultipliedFirst.
    entry.image = CGImageCreate(
        shape.width, shape.height, 8, 32, (size_t)shape.width * 4, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
        provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    if (!entry.image) return;

    impl->cursor_shapes.emplace(shape.shape_id, std::move(entry));
}

void MacVideoView::update_cursor_position(const protocol::CursorPositionMessage& pos) {
    auto* impl = static_cast<MacVideoViewImpl*>(impl_);
    if (!impl || !impl->view) return;

    CALayer* cursor = impl->view.cursorLayer;
    if (!cursor) return;

    // Host cursor visibility drives relative-mouse mode: when the host hides
    // its cursor (typical for FPS/camera-locked apps), we switch to sending
    // raw hardware deltas and decouple the local cursor so motion isn't
    // lost at screen edges. Mirrors Windows stream_window.cpp enter/exit.
    if (!pos.visible) {
        [impl->view enterRelativeMode];
        cursor.hidden = YES;
        return;
    }
    [impl->view exitRelativeMode];
    auto it = impl->cursor_shapes.find(pos.shape_id);
    if (it == impl->cursor_shapes.end()) {
        cursor.hidden = YES;
        return;
    }
    const CursorShapeEntry& entry = it->second;

    const CGFloat view_w = impl->view.bounds.size.width;
    const CGFloat view_h = impl->view.bounds.size.height;
    if (view_w <= 0 || view_h <= 0) return;
    if (impl->host_w == 0 || impl->host_h == 0) return;

    // Compute letterboxed/pillarboxed video rect (bottom-up coords).
    const double host_ar = (double)impl->host_w / (double)impl->host_h;
    const double view_ar = (double)view_w / (double)view_h;
    double video_w, video_h, off_x, off_y_bu;
    if (view_ar > host_ar) {
        video_h   = view_h;
        video_w   = view_h * host_ar;
        off_x     = (view_w - video_w) * 0.5;
        off_y_bu  = 0;
    } else {
        video_w   = view_w;
        video_h   = view_w / host_ar;
        off_x     = 0;
        off_y_bu  = (view_h - video_h) * 0.5;
    }

    // Cursor size in view space: preserve host-relative size.
    const double cw = (double)entry.width  * video_w / (double)impl->host_w;
    const double ch = (double)entry.height * video_h / (double)impl->host_h;
    // Hotspot offset in view space.
    const double hsx = (double)entry.hotspot_x * cw / (double)entry.width;
    const double hsy = (double)entry.hotspot_y * ch / (double)entry.height;
    // Hotspot target position (top-down, inside video rect).
    const double px_td = (double)pos.x_norm * video_w;
    const double py_td = (double)pos.y_norm * video_h;
    // Cursor layer origin in bottom-up view coords.
    const double origin_x     = off_x + (px_td - hsx);
    const double origin_y_bu  = off_y_bu + video_h - (py_td - hsy) - ch;

    CGFloat scale = impl->view.window ? impl->view.window.backingScaleFactor : 2.0;
    if (scale <= 0) scale = 1.0;
    cursor.contentsScale = scale;

    // Avoid implicit animation on every mouse move — cursor should track instantly.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    cursor.contents = (__bridge id)entry.image;
    cursor.frame = CGRectMake(origin_x, origin_y_bu, cw, ch);
    cursor.hidden = NO;
    [CATransaction commit];
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

    @autoreleasepool {

    auto nals = split_annexb(data, len);
    if (nals.empty()) return false;

    const bool is_hevc = (impl->codec == VideoCodec::HEVC);

    // Extract parameter sets from keyframes; build format description once we have
    // HEVC: VPS+SPS+PPS, H.264: SPS+PPS.
    if (keyframe) {
        for (const auto& n : nals) {
            if (is_hevc) {
                switch (hevc_nal_type(n.data, n.size)) {
                    case HEVC_NAL_VPS: impl->vps.assign(n.data, n.data + n.size); break;
                    case HEVC_NAL_SPS: impl->sps.assign(n.data, n.data + n.size); break;
                    case HEVC_NAL_PPS: impl->pps.assign(n.data, n.data + n.size); break;
                    default: break;
                }
            } else {
                switch (h264_nal_type(n.data, n.size)) {
                    case H264_NAL_SPS: impl->sps.assign(n.data, n.data + n.size); break;
                    case H264_NAL_PPS: impl->pps.assign(n.data, n.data + n.size); break;
                    default: break;
                }
            }
        }

        const bool have_all = is_hevc
            ? (!impl->vps.empty() && !impl->sps.empty() && !impl->pps.empty())
            : (!impl->sps.empty() && !impl->pps.empty());

        if (have_all && !impl->have_params) {
            if (impl->format_desc) {
                CFRelease(impl->format_desc);
                impl->format_desc = nullptr;
            }
            OSStatus st;
            if (is_hevc) {
                const uint8_t* params[3] = { impl->vps.data(), impl->sps.data(), impl->pps.data() };
                const size_t   sizes[3]  = { impl->vps.size(), impl->sps.size(), impl->pps.size() };
                st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    kCFAllocatorDefault, 3, params, sizes,
                    4, nullptr, &impl->format_desc);
            } else {
                const uint8_t* params[2] = { impl->sps.data(), impl->pps.data() };
                const size_t   sizes[2]  = { impl->sps.size(), impl->pps.size() };
                st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    kCFAllocatorDefault, 2, params, sizes,
                    4, &impl->format_desc);
            }
            if (st != noErr) {
                log::error(TAG, "CMVideoFormatDescriptionCreateFromParameterSets failed: %d (codec=%s)",
                           (int)st, is_hevc ? "HEVC" : "H264");
                return false;
            }
            impl->have_params = true;
            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(impl->format_desc);
            // Respect stream-size override from set_stream_size() if already set.
            if (impl->host_w == 0 || impl->host_h == 0) {
                impl->host_w = static_cast<uint32_t>(dims.width);
                impl->host_h = static_cast<uint32_t>(dims.height);
            }
            // Detect HDR from the format description's transfer function.
            // PQ (SMPTE 2084) or HLG (Rec.2100) → treat as HDR for the HUD.
            CFStringRef tfn = (CFStringRef)CMFormatDescriptionGetExtension(
                impl->format_desc, kCMFormatDescriptionExtension_TransferFunction);
            impl->is_hdr = false;
            if (tfn) {
                if (CFEqual(tfn, kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ) ||
                    CFEqual(tfn, kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG)) {
                    impl->is_hdr = true;
                }
            }
            log::info(TAG, "%s format description ready: %dx%d%s",
                      is_hevc ? "HEVC" : "H264", dims.width, dims.height,
                      impl->is_hdr ? " HDR" : "");
        }
    }

    if (!impl->have_params) return false;

    // Build the AVCC-format payload: concatenate non-parameter NAL units with
    // 4-byte big-endian length prefix instead of start codes.
    std::vector<uint8_t> avcc;
    avcc.reserve(len);
    for (const auto& n : nals) {
        if (is_hevc) {
            int t = hevc_nal_type(n.data, n.size);
            if (t == HEVC_NAL_VPS || t == HEVC_NAL_SPS || t == HEVC_NAL_PPS) continue;
        } else {
            int t = h264_nal_type(n.data, n.size);
            if (t == H264_NAL_SPS || t == H264_NAL_PPS) continue;
        }
        uint32_t sz = static_cast<uint32_t>(n.size);
        avcc.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(sz & 0xFF));
        avcc.insert(avcc.end(), n.data, n.data + n.size);
    }
    if (avcc.empty()) return false;

    // Wrap in CMBlockBuffer. Allocate with malloc and hand ownership to CM
    // (kCFAllocatorMalloc → CM frees with free() when the block is released).
    void* block_mem = std::malloc(avcc.size());
    if (!block_mem) return false;
    std::memcpy(block_mem, avcc.data(), avcc.size());

    CMBlockBufferRef block = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        block_mem,
        avcc.size(),
        kCFAllocatorMalloc,      // CM owns block_mem, frees via free()
        nullptr,
        0, avcc.size(),
        0,
        &block);
    if (st != noErr || !block) {
        log::error(TAG, "CMBlockBufferCreateWithMemoryBlock failed: %d", (int)st);
        std::free(block_mem);
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

    } // @autoreleasepool
    return false;
}

} // namespace vivora

#endif // VIVORA_MACOS
