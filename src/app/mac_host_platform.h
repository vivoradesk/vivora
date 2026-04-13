#pragma once
#ifdef DESKBEAM_MACOS

#include "app/host_platform.h"
#include "host/capture/mac_screen_capture.h"
#include "host/encode/mac_videotoolbox_encoder.h"
#include <vector>

class MacHostPlatform : public deskbeam::HostPlatform {
public:
    bool init(uint32_t display_index, bool prefer_hdr, uint32_t manual_bitrate_bps);

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

private:
    deskbeam::host::MacScreenCapture capture_;
    deskbeam::host::MacVideoToolboxEncoder encoder_;
    std::vector<uint8_t> pkt_buf_;
};

#endif // DESKBEAM_MACOS
