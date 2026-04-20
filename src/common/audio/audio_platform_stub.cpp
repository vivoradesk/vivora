// Linux audio platform stubs. macOS and Windows have real implementations;
// Linux (PulseAudio/PipeWire) TBD.

#if !defined(_WIN32) && !defined(__APPLE__)

#include "common/audio/audio_capture.h"
#include "common/audio/audio_output.h"

namespace deskbeam::audio {

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    return nullptr;
}

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return nullptr;
}

} // namespace deskbeam::audio

#endif
