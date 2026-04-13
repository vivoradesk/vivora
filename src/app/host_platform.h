#pragma once

#include <cstdint>
#include <cstddef>

namespace deskbeam {

// Platform-specific host operations: capture, encode, shutdown.
// One implementation per platform (Windows/DXGI+AMF, macOS/SCK+VTB, Linux/PW+VAAPI).
// The common host loop (run_host_loop) drives the session/bitrate/telemetry logic
// and calls into this interface for platform-specific work.
struct HostPlatform {
    virtual ~HostPlatform() = default;

    // Resolution of the captured display (pixels fed to encoder).
    virtual uint32_t capture_width()  const = 0;
    virtual uint32_t capture_height() const = 0;

    // Resolution in the input coordinate space (may differ from capture,
    // e.g. macOS Retina points vs backing pixels).  Default = capture res.
    virtual uint32_t input_width()  const { return capture_width(); }
    virtual uint32_t input_height() const { return capture_height(); }

    // Encoder control.
    virtual void set_bitrate(uint32_t bps) = 0;
    virtual void request_idr() = 0;

    // Capture + encode one frame.  Returns true if a frame was captured
    // (even if encoding produced no output yet).  pts_us receives the
    // presentation timestamp in microseconds.
    //
    // content_changed is set to false when the screen image didn't change
    // (e.g. cursor-only DXGI update).  The caller may skip encoding when
    // !content_changed && !force, but the platform decides the semantics.
    virtual bool capture_and_encode(uint64_t& pts_us,
                                    bool& content_changed,
                                    bool force) = 0;

    // Pull the next encoded packet.  Returns false when no more packets
    // are available this iteration.
    struct EncodedPacketView {
        const uint8_t* data = nullptr;
        size_t         len  = 0;
        uint64_t       pts  = 0;
        bool           keyframe = false;
    };
    virtual bool get_encoded_packet(EncodedPacketView& pkt) = 0;

    // Called when no frame was captured (platform may sleep / yield).
    virtual void on_idle() {}

    // Cleanup.
    virtual void shutdown() {}
};

} // namespace deskbeam
