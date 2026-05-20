#pragma once
#ifdef VIVORA_MACOS

#include "app/view_platform.h"
#include "client/render/mac_video_view.h"

class MacViewPlatform : public vivora::ViewPlatform {
public:
    bool init(const char* host_ip, uint16_t port);

    void set_input_callback(InputCallback cb) override;
    bool pump_events() override;
    bool init_decoder(vivora::VideoCodec codec) override;
    bool decode(const uint8_t* data, size_t len,
                uint32_t timestamp, bool keyframe,
                uint16_t seq_no) override;
    int  render() override;
    void flush_decoder() override;
    void set_stream_size(uint32_t width, uint32_t height) override;
    void upload_cursor_shape(const vivora::protocol::CursorShapeMessage& shape) override;
    void update_cursor_position(const vivora::protocol::CursorPositionMessage& pos) override;
    void update_stats(const vivora::StatsView& stats) override;
    void shutdown() override;

private:
    vivora::MacVideoView view_;
};

#endif // VIVORA_MACOS
