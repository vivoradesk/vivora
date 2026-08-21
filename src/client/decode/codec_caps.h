// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>

namespace vivora::client {

// Probe which video codecs this client can actually decode (VIV-112).
//
// The receiver's decoder is the binding constraint on codec selection, so the
// client advertises its real decode capability to the host in the HELLO and the
// host picks the best codec it can encode from that set.
//
// Returns a bitmask laid out like VideoCodecCaps (bit N = codec enum value N):
//   bit0 = H.264, bit1 = HEVC.
//
// The probe is per-platform and cheap (a decoder-availability query, not a full
// decode session):
//   * Windows : MFTEnumEx for a decoder MFT of each subtype.  This is where the
//               distinction actually bites — many Windows machines lack the
//               HEVC Video Extension and cannot decode HEVC via Media Foundation.
//   * Linux   : avcodec_find_decoder() for each codec id.
//   * macOS   : VideoToolbox decodes both H.264 and HEVC on every supported Mac,
//               so we report both.
//
// Robustness: if a platform probe cannot run or comes back empty, we fall back
// to CODEC_CAP_ALL_KNOWN so a probe failure never strands the client with no
// codec (matching the pre-VIV-112 assumption that both were decodable).  The
// result is computed once and cached.
uint8_t probe_decode_caps();

} // namespace vivora::client
