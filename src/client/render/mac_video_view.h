#pragma once

#ifdef DESKBEAM_MACOS

#include "common/codec/video_codec.h"
#include "common/protocol/cursor_message.h"
#include "common/protocol/input_event.h"

#include <cstdint>
#include <cstddef>
#include <functional>

namespace deskbeam {

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

    // Pump NSApplication events non-blocking. Call regularly from the main loop.
    void pump_events();

    // True if the user closed the window.
    bool should_close() const { return should_close_; }

    // Set input event callback. Called from main thread during pump_events().
    using InputCallback = std::function<void(const protocol::InputEvent&)>;
    void set_input_callback(InputCallback cb);

private:
    void* impl_;           // opaque pointer to Obj-C++ impl struct
    bool should_close_ = false;
};

} // namespace deskbeam

#endif // DESKBEAM_MACOS
