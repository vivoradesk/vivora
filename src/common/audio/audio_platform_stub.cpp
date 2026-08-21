// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

// Audio platform stubs for environments without a native implementation.
// Linux now provides `create_default_audio_output()` from
// `pulse_audio_output.cpp`, so this file only defines the loopback capture
// stub on Linux (host-side capture is L3 territory) and full stubs for any
// other UNIX-like target that may appear later.

#if !defined(VIVORA_LINUX) && !defined(_WIN32) && !defined(__APPLE__)

#include "common/audio/audio_capture.h"
#include "common/audio/audio_output.h"

namespace vivora::audio {

std::unique_ptr<AudioCapture> create_default_loopback_capture() {
    return nullptr;
}

std::unique_ptr<AudioOutput> create_default_audio_output() {
    return nullptr;
}

} // namespace vivora::audio

#endif
