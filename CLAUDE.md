# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Vivora is an open-source, low-latency remote desktop application. The goal is Parsec-level streaming quality (4-8ms pipeline latency on LAN) with a self-hosted, open-source model. Written in C++17 (C++20 where appropriate) with Qt 6/QML for UI.

## Build System

- **Build tool:** CMake
- **Dependencies:** Qt 6.5+ (built against 6.8/6.9), NVIDIA Video Codec SDK, FFmpeg (libavcodec, libavutil, libswscale), Opus, Monocypher (vendored)
- **Optional deps:** AMD AMF SDK, Intel oneVPL, x264
- **Target platforms (MVP):** Windows host + client, macOS host + client, Linux host + client

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Run tests
ctest --test-dir build
```

## Architecture

A single `vivora` executable that runs in either host or client mode (selected at launch), plus a standalone relay server. All modes share the common protocol/codec/utils code.

**Host pipeline:** Screen Capture (DXGI) -> GPU Encoder (NVENC/AMF/QSV) -> Custom UDP Protocol -> Network. Zero-copy: frame stays in GPU memory from capture through encoding (ID3D11Texture2D -> NVENC directly). Input injection receives events via separate reliable UDP channel and uses SendInput API.

**Client pipeline:** Network -> GPU Decoder (D3D11VA/VAAPI) -> Qt Quick Render. Zero-copy: decoded frame stays as GPU texture, rendered via custom QQuickItem. Input events captured via Raw Input API and sent immediately without batching.

**Network protocol:** Custom UDP (not TCP, not WebRTC). Four channels:
- Video: unreliable UDP + FEC (lost frames are skipped, not retransmitted)
- Audio: semi-reliable UDP (Opus codec, 20-40ms jitter buffer)
- Input: its own datagrams so a video burst never delays a keystroke.  Note the ACK/retransmit in the original design was never built -- input is fire-and-forget today (VIV-97)
- Control: UDP for handshake, auth, clipboard, monitor switching (the original TCP design was dropped -- everything rides the one UDP socket)

**Encryption:** Noise Protocol (pattern IK) for all channels via Monocypher. Curve25519 key exchange + ChaCha20-Poly1305 symmetric. Host's public key is its identity (shown as a peer code on first connect, TOFU). IK rather than NK so the host also learns the client's static key.

## Source Layout

```
src/common/           # Shared code: protocol/, codec/, utils/
src/host/             # capture/, encode/, audio/, input/, session/
src/client/           # decode/, render/, audio/, input/, ui/
src/relay/            # Standalone relay server
qml/                  # QML UI files
third_party/          # nvenc/, amf/, opus/ SDK headers
tests/                # Unit tests and latency benchmarks
```

## Development Guidelines

- **Latency is the primary metric.** Every architectural decision is evaluated through latency impact. Target: <15ms end-to-end on LAN.
- **Zero-copy GPU pipeline.** Data must not leave GPU memory unnecessarily. No CPU-side copies between capture, encode, decode, and render.
- **Platform abstraction from day one.** Platform-specific code (DXGI, WASAPI, SendInput, etc.) must be isolated behind interfaces.
- **Every module must log its latency** (capture_time, encode_time, network_rtt, decode_time, render_time) for profiling.
- Code comments in English. Git commits in English using conventional commits format.
- Write tests for the protocol layer first -- it's the most critical shared component.
- Current phase is **0.1 release**: Windows and Linux binaries, packaging and CI. See `STATUS.md` for the per-platform state and `docs/ARCHITECTURE.md` for how the pipeline fits together.

## Packet Header Format

```
Type(1B) | SeqNo(2B) | Timestamp(4B) | Flags(1B) | PayloadLen(2B) | Payload | FEC
```
Total header: 10 bytes.
