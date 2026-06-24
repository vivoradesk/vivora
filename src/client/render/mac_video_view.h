#pragma once

#ifdef VIVORA_MACOS

#include "app/view_platform.h"
#include "common/codec/video_codec.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"

#include <cstdint>
#include <cstddef>
#include <functional>

namespace vivora {

// Cocoa window hosting an AVSampleBufferDisplayLayer.
// Accepts HEVC or H.264 Annex-B encoded frames and displays them.
class MacVideoView {
public:
    MacVideoView();
    ~MacVideoView();

    MacVideoView(const MacVideoView&) = delete;
    MacVideoView& operator=(const MacVideoView&) = delete;

    // Create window and activate NSApplication. Must be called from main thread.
    bool create_window(const char* title, uint32_t width, uint32_t height);

    // Select codec before first frame. Defaults to HEVC.
    void set_codec(VideoCodec codec);

    // Feed one encoded frame (Annex-B format with 00 00 00 01 start codes).
    // Keyframe frames should contain HEVC VPS/SPS/PPS+IDR or H.264 SPS/PPS+IDR.
    // Returns false if the frame was dropped (e.g. no format yet on non-keyframe).
    bool submit_frame(const uint8_t* data, size_t len, uint64_t pts_us, bool keyframe);

    // Reset the display layer and drop cached parameter sets — next keyframe
    // re-builds the format description. Called after packet loss / IDR retry.
    void flush_decoder();

    // Authoritative pre-padding stream size (host sends this in StreamInfo
    // when the encoder pads to alignment). Used for mouse normalization.
    void set_stream_size(uint32_t width, uint32_t height);

    // Cursor overlay. upload_cursor_shape caches the bitmap keyed by shape_id;
    // update_cursor_position is called per-frame with normalized coords.
    void upload_cursor_shape(const protocol::CursorShapeMessage& shape);
    void update_cursor_position(const protocol::CursorPositionMessage& pos);

    // Push diagnostics snapshot to the F9 HUD overlay (no-op when hidden).
    void update_stats(const StatsView& stats);

    // Centred status overlay shown before the first frame ("Connecting…",
    // "Waiting for host to accept…", close reason).  Empty hides it (VIV-62).
    void set_status(const char* text);

    // Pump NSApplication events non-blocking. Call regularly from the main loop.
    void pump_events();

    // True if the user closed the window.
    bool should_close() const { return should_close_; }

    // Set input event callback. Called from main thread during pump_events().
    using InputCallback = std::function<void(const protocol::InputEvent&)>;
    void set_input_callback(InputCallback cb);

    // In-stream menu (VIV-74).  keep aspect ratio (letterbox) vs stretch.
    void set_keep_aspect(bool keep);
    // Toggle the Cocoa window between fullscreen and windowed.
    void toggle_fullscreen();
    // Invoked on the main thread when the user presses the menu hotkey
    // (Ctrl+F1) over the stream.  The platform shows/hides the Qt menu.
    void set_menu_hotkey_callback(std::function<void()> cb);

    // While the in-stream menu is open the stream view must release/show the
    // cursor (so the user can click the menu even if the host hid it / we
    // were in relative mode) and stop forwarding mouse+keys to the host.  On
    // close it briefly ignores input so the click-away that dismissed the
    // menu isn't also delivered to the host.
    void set_menu_open(bool open);

    // Invoked when the user clicks the stream while the menu is open — i.e. a
    // click outside the (separate, Qt) menu window.  The platform dismisses
    // the menu.  Needed because the Qt overlay doesn't reliably get a
    // deactivation event under the Cocoa app, so click-away can't rely on it.
    void set_menu_dismiss_callback(std::function<void()> cb);

private:
    void* impl_;           // opaque pointer to Obj-C++ impl struct
    bool should_close_ = false;
};

} // namespace vivora

#endif // VIVORA_MACOS
