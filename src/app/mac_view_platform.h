#pragma once
#ifdef DESKBEAM_MACOS

#include "app/view_platform.h"
#include "client/render/mac_video_view.h"

class MacViewPlatform : public deskbeam::ViewPlatform {
public:
    bool init(const char* host_ip, uint16_t port);

    void set_input_callback(InputCallback cb) override;
    bool pump_events() override;
    bool decode(const uint8_t* data, size_t len,
                uint32_t timestamp, bool keyframe,
                uint16_t seq_no) override;
    int  render() override;
    void shutdown() override;

private:
    deskbeam::MacVideoView view_;
};

#endif // DESKBEAM_MACOS
