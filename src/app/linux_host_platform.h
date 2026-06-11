#pragma once
#ifdef VIVORA_LINUX

#include "app/host_platform.h"
#include "host/capture/pipewire_capture.h"
#include "host/capture/x11_cursor.h"
#include "host/encode/linux_encoder.h"
#include "host/encode/video_encoder.h"  // EncoderKind

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>

class LinuxHostPlatform : public vivora::HostPlatform {
public:
    bool init(uint32_t manual_bitrate_bps, vivora::VideoCodec codec,
              vivora::EncoderKind encoder_kind);

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;
    vivora::VideoCodec actual_codec() const override { return codec_; }

    // PipeWire is event-driven — capture happens on its own thread and
    // encoded packets land in queued_pkts_.  capture_and_encode() returns
    // true if any new packets accumulated since the last call (so the
    // host_loop knows to drain).  re_encode_last() re-encodes the most
    // recent capture buffer for static-screen heartbeat.
    bool capture_and_encode(uint64_t& pts_us, bool& content_changed, bool force) override;
    bool re_encode_last(uint64_t pts_us) override;
    bool get_encoded_packet(EncodedPacketView& out) override;

    bool get_cursor_state(CursorState& out) override;
    bool take_cursor_shape(CursorShapeView& out) override;

    void shutdown() override;

private:
    void on_pw_frame(const vivora::host::PipeWireCapture::Frame& f);

    vivora::host::PipeWireCapture cap_;
    // X11 cursor source (VIV-66): when available, supplies the host cursor
    // position + shape directly, and we tell PipeWire to leave the cursor out
    // of the frame.  Falls back to the portal's embedded cursor if X is absent.
    vivora::host::X11Cursor x11cursor_;
    bool                    x11_cursor_active_ = false;
    vivora::host::X11Cursor::Shape pending_x11_shape_{};
    bool                    have_pending_x11_shape_ = false;
    uint32_t                x11_shape_id_ = 0;
    std::unique_ptr<vivora::host::ILinuxEncoder> enc_;
    vivora::VideoCodec  codec_        = vivora::VideoCodec::H264;
    vivora::EncoderKind encoder_kind_ = vivora::EncoderKind::Auto;
    // Set when first PipeWire frame arrives — init() blocks until then
    // so host_loop sees real capture dimensions for bitrate sizing.
    std::condition_variable first_frame_cv_;
    std::mutex first_frame_mu_;
    bool first_frame_seen_ = false;

    // Capture geometry — set on first frame after PipeWire negotiates.
    uint32_t cap_w_ = 0, cap_h_ = 0;
    bool     enc_ready_ = false;
    uint32_t bitrate_bps_ = 0;

    // Encoder + output queue: PipeWire thread (or heartbeat path) feeds
    // the encoder and pushes any drained packets into queued_pkts_; the
    // host_loop pops via get_encoded_packet().  The heartbeat tag rides
    // alongside the packet so the wire layer can switch off FEC for it.
    struct QueuedPacket {
        vivora::host::ILinuxEncoder::Packet pkt;
        bool heartbeat = false;
    };
    std::mutex enc_mu_;
    std::queue<QueuedPacket> queued_pkts_;

    // Set by shutdown() before tearing down the encoder.  PipeWire's
    // pw_thread_loop_stop() only signals the loop to exit but doesn't
    // synchronously join in-flight callbacks; an on_pw_frame already
    // dispatched can resume after cap_.stop() returned, race against
    // enc_.shutdown(), and segfault inside sws_scale on a freed
    // sw_frame_.  on_pw_frame checks this flag (under enc_mu_) and
    // bails before touching the encoder.
    std::atomic<bool> shutting_down_{false};

    // Outgoing packet view returned from get_encoded_packet — buffer
    // owned by us so the EncodedPacketView's data ptr stays valid until
    // the next call.
    QueuedPacket pkt_buf_;
};

#endif // VIVORA_LINUX
