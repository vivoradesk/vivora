# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DeskBeam is an open-source, low-latency remote desktop application. The goal is Parsec-level streaming quality (4-8ms pipeline latency on LAN) with a self-hosted, open-source model. Written in C++17 (C++20 where appropriate) with Qt 6/QML for UI.

## Build System

- **Build tool:** CMake
- **Dependencies:** Qt 6.5+, NVIDIA Video Codec SDK, FFmpeg (libavcodec, libavutil), Opus, OpenSSL 3.x
- **Optional deps:** AMD AMF SDK, Intel oneVPL, x264
- **Target platforms (MVP):** Windows host + client, Linux client

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Run tests
ctest --test-dir build
```

## Architecture

Two executables (host and client) plus a relay server, sharing common protocol/codec/utils code.

**Host pipeline:** Screen Capture (DXGI) -> GPU Encoder (NVENC/AMF/QSV) -> Custom UDP Protocol -> Network. Zero-copy: frame stays in GPU memory from capture through encoding (ID3D11Texture2D -> NVENC directly). Input injection receives events via separate reliable UDP channel and uses SendInput API.

**Client pipeline:** Network -> GPU Decoder (D3D11VA/VAAPI) -> Qt Quick Render. Zero-copy: decoded frame stays as GPU texture, rendered via custom QQuickItem. Input events captured via Raw Input API and sent immediately without batching.

**Network protocol:** Custom UDP (not TCP, not WebRTC). Four channels:
- Video: unreliable UDP + FEC (lost frames are skipped, not retransmitted)
- Audio: semi-reliable UDP (Opus codec, 20-40ms jitter buffer)
- Input: reliable UDP with lightweight ACK mechanism (separate from video)
- Control: TCP for handshake, auth, clipboard, file transfer

**Encryption:** DTLS 1.3 for UDP channels, TLS 1.3 for TCP. AES-256-GCM symmetric, Curve25519 key exchange.

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
- Current phase is **Phase 1 (Proof of Concept):** get a 1080p60 video stream working over LAN with DXGI capture, NVENC encoding, basic UDP transport, D3D11VA decoding, and basic mouse+keyboard input. Direct IP:port connection, no NAT traversal yet.

## Packet Header Format

```
Type(1B) | SeqNo(2B) | Timestamp(4B) | Flags(1B) | PayloadLen(2B) | Payload | FEC
```
Total header: 10 bytes.
