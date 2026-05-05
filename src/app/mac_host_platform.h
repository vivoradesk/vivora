#pragma once
#ifdef DESKBEAM_MACOS

#include "app/host_platform.h"
#include "host/capture/mac_screen_capture.h"
#include "host/encode/mac_videotoolbox_encoder.h"
#include "host/encode/video_encoder.h"
#include <cstdint>
#include <vector>

class MacHostPlatform : public deskbeam::HostPlatform {
public:
    bool init(uint32_t display_index, uint32_t manual_bitrate_bps,
              deskbeam::VideoCodec codec);

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;
    uint32_t input_width()    const override;
    uint32_t input_height()   const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;

    bool capture_and_encode(uint64_t& pts_us,
                            bool& content_changed,
                            bool force) override;
    bool get_encoded_packet(EncodedPacketView& out) override;
    void on_idle() override;
    void shutdown() override;

    bool get_cursor_state(CursorState& out) override;
    bool take_cursor_shape(CursorShapeView& out) override;

private:
    deskbeam::host::MacScreenCapture capture_;
    deskbeam::host::MacVideoToolboxEncoder encoder_;
    std::vector<uint8_t> pkt_buf_;

    // Cursor tracking state.
    uint64_t  last_shape_hash_ = 0;
    uint32_t  shape_id_counter_ = 0;
    uint32_t  current_shape_id_ = 0;
    bool      pending_shape_    = false;
    uint16_t  pending_shape_w_  = 0;
    uint16_t  pending_shape_h_  = 0;
    uint16_t  pending_hotspot_x_ = 0;
    uint16_t  pending_hotspot_y_ = 0;
    std::vector<uint8_t> pending_shape_bgra_;

    // Pointer-identity cache: skip expensive CGBitmapContext render when
    // NSCursor and its NSImage are the same objects as last tick. Plain
    // uintptr_t — no retain: system cursors are Apple singletons, and app
    // custom cursors outlive the window of a single tick. Hash comparison
    // is the fallback if the ABA problem ever occurs.
    std::uintptr_t last_cursor_id_ = 0;
    std::uintptr_t last_image_id_  = 0;
};

#endif // DESKBEAM_MACOS
