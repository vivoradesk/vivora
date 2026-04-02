#include "common/utils/log.h"
#include "host/capture/screen_capture.h"
#include <cstdio>

#ifdef DESKBEAM_WINDOWS
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
    deskbeam::log::info("HOST", "DeskBeam Host v0.1.0 starting...");

    auto capture = deskbeam::IScreenCapture::create();
    if (!capture) {
        deskbeam::log::error("HOST", "Failed to create screen capture");
        return 1;
    }

    // Enumerate monitors
    auto monitors = capture->enumerate_monitors();
    for (const auto& mon : monitors) {
        deskbeam::log::info("HOST", "Monitor %u: %s (%ux%u)%s",
                            mon.index, mon.name.c_str(),
                            mon.resolution.width, mon.resolution.height,
                            mon.primary ? " [primary]" : "");
    }

    // Initialize capture on primary monitor
    if (!capture->init(0)) {
        deskbeam::log::error("HOST", "Failed to initialize capture");
        return 1;
    }

    auto res = capture->get_resolution();
    deskbeam::log::info("HOST", "Capturing at %ux%u", res.width, res.height);

    // Capture loop — for now just captures and measures timing
    deskbeam::log::info("HOST", "Starting capture loop (Ctrl+C to stop)...");

    uint64_t total_frames = 0;
    auto start = deskbeam::Clock::now();

    while (true) {
        deskbeam::CapturedFrame frame;
        if (capture->capture_frame(frame, 100)) {
            total_frames++;

            if (total_frames % 60 == 0) {
                auto elapsed = std::chrono::duration<double>(
                    deskbeam::Clock::now() - start).count();
                double fps = total_frames / elapsed;
                deskbeam::log::info("HOST", "Frames: %llu, FPS: %.1f, dirty_rects: %zu",
                                    static_cast<unsigned long long>(total_frames),
                                    fps, frame.dirty_rects.size());
            }

            capture->release_frame(frame);
        }
    }

    return 0;
}
