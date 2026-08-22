# Vivora — Project Status

Last updated: 2026-08-19

A snapshot of what works, what's planned, and the open questions.
For the architectural overview see `docs/ARCHITECTURE.md`.

---

## What's done

### Per-platform feature matrix

#### Host (capture → encode → audio → input → cursor)

|                          | Windows                       | macOS                  | Linux                            |
| ------------------------ | ----------------------------- | ---------------------- | -------------------------------- |
| Screen capture           | ✅ DXGI Desktop Duplication   | ✅ ScreenCaptureKit     | ✅ PipeWire + xdg-desktop-portal |
| Video encode             | ✅ NVENC + QSV (auto-fallback) + AMF | ✅ VideoToolbox  | ✅ VAAPI (iHD) — H.264 default   |
| HDR (HEVC Main10)        | ✅ NVENC, QSV; AMF works      | ✅                      | ❌ 8-bit pipeline                |
| Audio capture            | ✅ WASAPI loopback            | ✅ CoreAudio tap        | ✅ PulseAudio `@DEFAULT_MONITOR@`|
| Input injection          | ✅ SendInput                  | ✅ CGEventPost          | ✅ `/dev/uinput`                 |
| Cursor sync (shape+pos)  | ✅ DXGI cursor                | ✅ SCK cursor metadata  | ⚠️  EMBEDDED (cursor in pixels) |
| Static-screen heartbeat  | ✅                             | ✅                      | ✅ (via cached NV12 surface)     |
| Multi-encoder fallback   | ✅ NVENC → QSV after 120 fails | n/a                    | n/a                              |

Notes
- **Linux HEVC** is opt-in via `--codec hevc`. Default is H.264 because Intel iHD VAAPI hits a known assertion at certain resolutions on the first IDR. On hardware where it works, `--codec hevc` enables it.
- **Linux cursor** uses EMBEDDED mode (cursor baked into the captured frame) because the METADATA path needs `xdg-desktop-portal-gnome ≥ 1.18` (Ubuntu 24.04+). We target Ubuntu 20.04 / 22.04 as the supported floor — too many users on those releases to drop. The METADATA code is dormant in `pipewire_capture.cpp` and will activate if the user happens to run a new enough portal.

#### Client (receive → decode → render → input → cursor)

|                          | Windows                       | macOS                          | Linux                                |
| ------------------------ | ----------------------------- | ------------------------------ | ------------------------------------ |
| Video decode             | ✅ MediaFoundation D3D11VA    | ✅ VideoToolbox                | ✅ FFmpeg + VAAPI HW                 |
| Render                   | ✅ D3D11 swapchain            | ✅ AVSampleBufferDisplayLayer  | ✅ Qt OpenGL                         |
| HDR display              | ✅ scRGB FP16 native          | ✅ Metal native                 | ✅ BT.2020+PQ shader (auto-exposure) |
| Audio playback           | ✅ WASAPI                     | ✅ CoreAudio                    | ✅ PulseAudio (PipeWire-shim ok)     |
| Input capture (KB+mouse) | ✅ Raw Input                  | ✅ NSEvent + IOKit modifiers   | ✅ Qt events, physical scancodes     |
| Relative-mouse mode      | ✅ Raw Input + ClipCursor     | ✅ NSEvent.deltaX/Y + warp     | ✅ X11 only (warp + grabMouse)       |
| Cursor shape (host→client)| ✅ QCursor cache             | ✅ CALayer cache                | ✅ QCursor cache                     |
| Diagnostics HUD (F9)     | ✅                             | ✅ (HDR detected from CMVideoFormatDescription) | ✅                          |

Notes
- **Linux relative-mode** is X11-only by design. Wayland refuses programmatic pointer warps for security; supporting it would need `zwp_relative_pointer_v1` + `zwp_pointer_constraints_v1`. X11 is still the default on most distros and works on 24.04/26.04, so this is shelved indefinitely.
- **HDR auto-exposure** runs only on Linux (Qt OpenGL shader path needs to map PQ→sRGB manually). Windows scRGB FP16 and macOS Metal hand off to the OS compositor for native HDR display, no manual gain.

### Encoder / decoder coverage by GPU vendor

#### Host-side encoders

| Platform | Backend | File | Vendors | Codecs       | HDR (Main10) |
| -------- | ------- | ---- | ------- | ------------ | ------------ |
| Windows  | NVENC                 | `src/host/encode/nvenc_encoder.{h,cpp}`           | NVIDIA           | **HEVC only**   | ✅            |
| Windows  | AMF                   | `src/host/encode/amf_encoder.{h,cpp}`             | AMD              | H.264 + HEVC    | ✅            |
| Windows  | QSV (oneVPL)          | `src/host/encode/qsv_encoder.{h,cpp}`             | Intel            | H.264 + HEVC    | ✅            |
| Windows  | NVENC→QSV fallback    | `src/host/encode/fallback_encoder.{h,cpp}`        | NVIDIA + Intel hybrid | inherits      | inherits     |
| macOS    | VideoToolbox          | `src/host/encode/mac_videotoolbox_encoder.{h,mm}` | All Apple GPUs   | H.264 + HEVC    | ✅            |
| Linux    | VAAPI (libavcodec)    | `src/host/encode/vaapi_encoder.{h,cpp}`           | **Intel iHD only (tested)** | H.264 + HEVC | ❌ 8-bit pipeline |
| Linux    | NVENC (CUDA, dlopen)  | `src/host/encode/nvenc_linux_encoder.{h,cpp}`     | NVIDIA           | H.264 + HEVC    | ❌ 8-bit pipeline |

Auto-probe order on Windows: AMF → NVENC → QSV (`encoder_factory.cpp`). On Linux the factory prefers NVENC when the NVIDIA driver libraries are present and falls back to VAAPI. The `--encoder` flag forces a specific backend.

#### Client-side decoders

| Platform | Backend | File | Vendors | Codecs | HW fallback |
| -------- | ------- | ---- | ------- | ------ | ----------- |
| Windows  | MediaFoundation D3D11VA   | `src/client/decode/mf_decoder.{h,cpp}`        | NVIDIA + AMD + Intel (vendor-agnostic via D3D11) | H.264 + HEVC | ❌ none |
| macOS    | VideoToolbox (AVSampleBufferDisplayLayer) | `src/client/render/mac_video_view.mm` | All Apple GPUs | H.264 + HEVC | ❌ none |
| Linux    | FFmpeg ± VAAPI HW         | `src/client/decode/ffmpeg_decoder.{h,cpp}`    | Intel (primary), AMD radeonsi (untested), NVIDIA via nvidia-vaapi-driver (untested) | H.264 + HEVC | ✅ silent SW fallback on HW init fail |

#### Known gaps in coverage

Listed so we know what to say when someone reports "doesn't work on my X."

1. **Linux + AMD host** — VAAPI through `radeonsi` should work in theory. We have never tested it. The first report from an RDNA / Radeon user will tell us.
2. **No software encoder fallback anywhere.** If hardware encoder init fails (driver mismatch, headless server, container without DRM nodes) the host bails. An x264 path would buy server / VM / very-old-hardware compatibility at the cost of CPU.
3. **NVENC on Windows: HEVC only.** Deliberate — AMF and QSV cover H.264, and codec negotiation picks a common one. It does mean an NVIDIA-only Windows host cannot serve an H.264-only client.
4. **Client software decode fallback on Windows / macOS** — same shape as Linux's. Not in today.
5. **macOS key injection is unverified.** All three hosts now press the
   physical key the wire carries rather than a layout-derived virtual key, and
   the mapping tables are unit-tested in both directions, but the macOS half
   has only been compiled — it has not been driven from a viewer on a non-Latin
   layout the way Windows and Linux have.
6. **Linux host cannot switch monitors.** It enumerates displays and advertises them, but `select_monitor` is unimplemented, so the client's picker cannot actually change the captured display.

### Measured pipeline latency

Every stage Vivora owns is timed and logged, so these are readings rather than
targets. Taken 2026-08-22, Windows host and Windows viewer on one machine —
3440×1440, hardware encode, D3D11VA decode, ~57 fps, both ends sharing one GPU:

| Stage | Average | Worst seen |
| --- | --- | --- |
| Capture (DXGI) | 0.08 ms | 0.26 ms |
| Encode | 0.53 ms | 1.14 ms |
| Decode (D3D11VA) | 0.34 ms | 0.89 ms |
| Render (D3D11) | 0.53 ms | 1.70 ms |
| **Everything but the network** | **~1.5 ms** | |

Read them with three caveats. Both ends shared one GPU, so a two-machine setup
should do no worse. The network is deliberately absent: it is the one stage
Vivora does not own, and any number here would describe the test LAN. And these
are per-frame processing costs, not glass-to-glass — display and compositor
latency sit outside what the code can see.

To reproduce: run host and viewer, and read the `capture:` / `encode:` fields
in the host log and `decode:` / `render:` in the viewer's.

The Linux host reports its encode differently: capture and encode happen on the
PipeWire thread, so the host loop's figure there is queue wait, not encoder
time. Worth fixing before quoting a Linux number.

### Network / protocol stack (cross-platform)

| Component                                | Status |
| ---------------------------------------- | ------ |
| UDP video transport                      | ✅      |
| FEC (Reed-Solomon)                       | ✅ adaptive M with sticky floor + keyframe boost |
| Retransmits (NACK)                       | ✅      |
| Adaptive bitrate (RTT-K)                 | ✅      |
| Adaptive framerate (PerfReport)          | ✅      |
| Adaptive audio jitter buffer             | ✅ 30..200 ms, grows on PLC / shrinks in quiet |
| Crypto (Noise_IK + ChaCha20-Poly1305)    | ✅ handshake, transport, audio, replay window |
| TOFU peer pinning (`known_peers.txt`)    | ✅      |
| Rendezvous + memorable codes             | ✅ `rdv.vivora.dev:7000`, or self-hosted |
| Hole-punching (STUN + PunchHint)         | ✅ tested inter-ISP (1123ms handshake) |
| Same-NAT short-circuit (LAN candidates)  | ✅ tested (11ms handshake same WiFi) |
| Relay daemon (`vivora-relay`)          | ✅ AGPL self-host code, smoke-tested localhost |
| Relay client/host integration            | ✅ `--relay HOST:PORT`, lazy BIND on first need |
| Relay license auth (Pro-managed)         | ✅ Ed25519-signed 95B tokens, `--require-license` on relay |
| Rendezvous-minted relay session_id       | ✅ rdv `--relay-endpoint` advertises endpoint+session_id; peers auto-adopt |
| Relay auto-fallback (direct → relay)     | ✅ client switches after 3 s of no HELLO_ACK, resets Noise on relay path |

---

## What's pending

### Before 0.1 ships

- **Publishing.** The artefacts build and have been smoke-tested — Windows
  installer and portable zip, Linux AppImage (glibc floor 2.30, verified on
  Fedora 40, Rocky 9 and Ubuntu 22.04), and a server tarball that installs on
  Debian 11. Nothing is published yet; the repository is still private.
- **CI.** Linux and Windows both build and test on every push. The Linux
  AppImage and the server tarball are built on tag; the Windows zip and MSI
  are still made by hand and attached to the draft.

### After 0.1

- **Gamepad forwarding.** XInput on Windows, IOKit HID on macOS, evdev on Linux; ViGEm or equivalent virtual pad on the host side.
- **Mobile viewers.** iOS reuses the macOS VideoToolbox decode path; Android needs a JNI + MediaCodec wrapper.
- **Stream zoom and pan** when host and client aspect ratios differ. Mouse mapping has to stay correct through the transform.
- **File transfer** — drag and drop into the stream window. The approval dialog already reserves a grant for it.
- **Cheaper loss recovery.** Long-term reference frames or partial-GOP recovery instead of a full keyframe on every unrecoverable loss: at this point the visible freeze *is* the keyframe.
- **Path MTU discovery.** Datagrams are a fixed 1360 bytes, which breaks on sub-1400 paths (some VPNs, IPv6 tunnels, cellular).
- **macOS distribution.** Needs an Apple Developer account for signing and notarisation.

### Shelved (intentionally not on the roadmap)

- **METADATA cursor mode on Linux** — needs portal ≥ 1.18 (Ubuntu 24.04+). We target 20.04 / 22.04 as the floor.
- **Linux DMA-BUF zero-copy** (capture→encode + decode→render). Big architectural piece (~week+); the SHM path is functional and Linux user share doesn't justify the investment yet.
- **Wayland relative-mouse path** on Linux client — needs `zwp_relative_pointer_v1` + pointer constraints. X11 covers 24.04/26.04.

---
