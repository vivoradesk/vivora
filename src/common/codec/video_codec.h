#pragma once

#include <cstdint>

namespace deskbeam {

enum class VideoCodec : uint8_t {
    H264 = 0,
    HEVC = 1,
};

} // namespace deskbeam
