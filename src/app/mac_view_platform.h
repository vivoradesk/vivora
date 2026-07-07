#pragma once
#ifdef VIVORA_MACOS

#include "app/mac_video_pipeline.h"
#include "app/view_platform.h"
#include "client/render/mac_video_view.h"
#include "client/render/stream_menu.h"

#include <QApplication>
#include <memory>

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
    void set_status(const char* text) override;
    void set_menu_actions(const vivora::MenuActions& actions) override;
    vivora::IVideoPipeline* video_pipeline() override;
    void shutdown() override;

private:
    void feed_menu_info();

    // CLI path only: the in-stream menu is a QWidget, so a QApplication must
    // exist before init() constructs it.  The GUI app already has one (Qt
    // forbids two per process); the CLI crashed on --view since VIV-74 —
    // found by the VIV-84 scripted Mac run.
    std::unique_ptr<QApplication> app_;
    vivora::MacVideoView view_;
    // Threaded pipeline (VIV-84) — created lazily by video_pipeline() when
    // VIVORA_PIPELINE=threaded; fused over the AVSampleBufferVideoRenderer.
    std::unique_ptr<vivora::MacVideoPipeline> pipeline_;
    // In-stream control menu (VIV-74) — a Qt overlay shown over the Cocoa
    // stream window, toggled by the Ctrl+F1 callback from the view.
    std::unique_ptr<vivora::StreamMenu> menu_;
    vivora::StatsView last_stats_{};
};

#endif // VIVORA_MACOS
