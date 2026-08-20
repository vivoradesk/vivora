# Third-party notices

Vivora itself is licensed under the GNU Affero General Public License v3.0 or
later; see `LICENSE`. This file lists the third-party components a shipped
build contains or links against, and the licences they carry.

Full licence texts are in the source tree under `third_party/`, and in the
`LICENSE` files of the SDKs named below.

---

## Bundled in the binary

**Opus** — audio codec. BSD 3-Clause.
Copyright Xiph.Org Foundation, Skype Limited, Octasic, Jean-Marc Valin,
Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding,
Erik de Castro Lopo, Mozilla, Amazon. See `third_party/opus/COPYING`.

**SpeexDSP** — resampler. BSD 3-Clause.
Copyright Xiph.org Foundation, Jean-Marc Valin, Analog Devices Inc.,
Commonwealth Scientific and Industrial Research Organisation.
See `third_party/speexdsp_src/COPYING`.

**Monocypher** — cryptography (Curve25519, ChaCha20-Poly1305, BLAKE2b,
EdDSA). Dual-licensed BSD 2-Clause or CC0-1.0, at your option.
Copyright Loup Vaillant and contributors. See
`third_party/monocypher/LICENCE.md`.

**Intel oneVPL / libvpl** — Quick Sync video encoding on Windows. MIT.
Copyright (c) 2020 Intel Corporation. See `third_party/onevpl/LICENSE`.

## Linked

**Qt 6** — application framework and UI. Used under the **LGPL v3**.
Windows builds link Qt statically. LGPL v3 section 4(d)(0) requires that you
be able to relink the application against a modified Qt: the complete
corresponding source of this program is published under the AGPL at
<https://github.com/vivoradesk/vivora>, and Qt's own source is available from
<https://download.qt.io/>. Building from that source with a Qt of your choice
is documented in the README.

**FFmpeg** (`libavcodec`, `libavutil`, `libswscale`) — video decoding, and
VA-API encoding on Linux. LGPL v2.1 or later; distribution builds may enable
GPL components, in which case the combination is GPL v2 or later. Either is
compatible with this program's AGPL v3. Source: <https://ffmpeg.org/>.
Not bundled on Windows or macOS.

**OpenSSL 3** — Windows builds only, linked statically. Apache Licence 2.0.
Copyright The OpenSSL Project.

Present for a single function: one translation unit inside Qt's network module
implements PBKDF2 with the OpenSSL 3 key-derivation API. **No Vivora traffic
goes through OpenSSL** — TLS uses Schannel, the Windows system implementation,
and the streaming path uses Monocypher. Earlier 0.1 builds shipped two
end-of-life OpenSSL 1.1 DLLs beside the exe for the same indirect reason; those
are gone.

## Headers only, not redistributed

**NVIDIA Video Codec SDK** (`nvEncodeAPI.h`) and **AMD Advanced Media
Framework** (`third_party/amf/`) headers are compiled against to call the
respective drivers. The runtimes themselves ship with the GPU driver and are
loaded dynamically; no NVIDIA or AMD binary is redistributed with Vivora.
Their SDK licence terms apply to the headers.

## Fonts

**Inter** and **JetBrains Mono** are embedded in the binary's resources, both
under the **SIL Open Font License 1.1**.
Copyright The Inter Project Authors; Copyright JetBrains s.r.o.
