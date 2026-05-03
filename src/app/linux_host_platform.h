#pragma once
#ifdef DESKBEAM_LINUX

#include "app/host_platform.h"
#include "host/capture/pipewire_capture.h"
#include "host/encode/vaapi_encoder.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <vector>

class LinuxHostPlatform : public deskbeam::HostPlatform {
public:
    bool init(uint32_t manual_bitrate_bps, deskbeam::VideoCodec codec);

    uint32_t capture_width()  const override;
    uint32_t capture_height() const override;

    void set_bitrate(uint32_t bps) override;
    void request_idr() override;
    deskbeam::VideoCodec actual_codec() const override { return codec_; }

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
    void on_pw_frame(const deskbeam::host::PipeWireCapture::Frame& f);

    deskbeam::host::PipeWireCapture cap_;
    deskbeam::host::VaapiEncoder    enc_;
    deskbeam::VideoCodec codec_ = deskbeam::VideoCodec::H264;
    // Set when first PipeWire frame arrives — init() blocks until then
    // so host_loop sees real capture dimensions for bitrate sizing.
    std::condition_variable first_frame_cv_;
    std::mutex first_frame_mu_;
    bool first_frame_seen_ = false;

    // Capture geometry — set on first frame after PipeWire negotiates.
    uint32_t cap_w_ = 0, cap_h_ = 0;
    bool     enc_ready_ = false;
    uint32_t bitrate_bps_ = 0;

    // Latest capture buffer (for heartbeat re-encode).  Updated under
    // frame_mu_ in the PipeWire callback; the heartbeat path snapshots
    // it under the same lock before encoding.
    std::mutex frame_mu_;
    std::vector<uint8_t> last_bgrx_;
    int      last_stride_ = 0;

    // Encoder + output queue: PipeWire thread feeds the encoder and
    // pushes any drained packets into queued_pkts_; host_loop pops via
    // get_encoded_packet().
    std::mutex enc_mu_;
    std::queue<deskbeam::host::VaapiEncoder::Packet> queued_pkts_;

    // Outgoing packet view returned from get_encoded_packet — buffer
    // owned by us so the EncodedPacketView's data ptr stays valid until
    // the next call.
    deskbeam::host::VaapiEncoder::Packet pkt_buf_;
};

#endif // DESKBEAM_LINUX
