// Non-Windows stubs for the audio platform factories. Replace with
// CoreAudio (macOS) and PulseAudio (Linux) implementations later.

#if !defined(_WIN32)

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
