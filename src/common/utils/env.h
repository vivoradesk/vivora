// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <cstring>

namespace vivora::util {

// Read a boolean environment flag.
//
// The naive `getenv(x) != nullptr` idiom treats VIVORA_FOO=0 as "on",
// which is a trap for anything that gates behaviour a user cares about
// (VIVORA_AUTO_ACCEPT bypasses the connection-approval prompt, so
// exporting it as 0 must NOT disable the prompt).  Accept the usual
// truthy spellings and treat everything else — including an empty
// value — as off.
inline bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') return false;
    return std::strcmp(v, "1")    == 0 ||
           std::strcmp(v, "on")   == 0 || std::strcmp(v, "ON")   == 0 ||
           std::strcmp(v, "yes")  == 0 || std::strcmp(v, "YES")  == 0 ||
           std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0 ||
           std::strcmp(v, "True") == 0;
}

} // namespace vivora::util
