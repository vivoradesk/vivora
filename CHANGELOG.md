# Changelog

Notable changes per release. Dates are the release date, not the merge date.
This project follows [semantic versioning](https://semver.org/) loosely: until
1.0, minor versions may change the wire protocol.

## 0.1.0 — unreleased

First public release. Everything below is "new" in the sense that this is the
first build anyone outside the project could download, so the list describes
what 0.1 *is* rather than what changed.

### Streaming

- GPU-to-GPU video path on every platform: DXGI Desktop Duplication, PipeWire
  via `xdg-desktop-portal`, or ScreenCaptureKit into NVENC / AMD AMF / Intel
  QSV / VAAPI / VideoToolbox, with no CPU copy between capture and encode.
- H.264 and HEVC, negotiated per connection from what the client can actually
  decode, with a live renegotiation path if the client's decoder fails at
  runtime rather than dropping the session.
- HDR passthrough (HEVC Main10) on Windows and macOS, including automatic
  promotion from H.264 when the captured surface is FP16.
- Hardware decode on all three platforms, with a silent software fallback on
  Linux and after sustained hardware failures elsewhere.
- Threaded decode pipeline: socket, decode and present run independently, so a
  slow frame stalls none of the others.
- Adaptive bitrate driven by round-trip time, loss and the client's reported
  sustainable framerate; adaptive framerate; selectable 30–144 fps cap.
- Static-screen heartbeat, so a client joining a motionless desktop still gets
  a picture.
- Lazy encoder: an idle host with no viewers keeps capture and encoder torn
  down.
- Multi-monitor: hosts advertise their displays and clients can switch between
  them (switching is implemented on Windows and macOS hosts).

### Network

- Custom UDP protocol with separate video, audio, input and control channels,
  so a video burst never delays a keystroke.
- Reed–Solomon forward error correction with an adaptive parity ratio, burst
  interleaving, extra parity on keyframe groups, and targeted NACK recovery for
  groups that are nearly complete.
- FEC overhead is carved out of the wire budget rather than added on top.
- Direct connection, or rendezvous with STUN hole punching (including a
  same-LAN short circuit), or relay fallback when hole punching cannot work.
- Automatic re-registration with the rendezvous server when the network
  changes, and auto-reconnect when a session drops.
- Relay-aware encoder profile that clamps bitrate on relayed sessions.

### Security

- `Noise_IK_25519_ChaChaPoly_BLAKE2b` on Monocypher: every channel sealed,
  audio included, with a per-channel replay window.
- A host's Curve25519 public key is its identity, pinned on first use, with a
  trust dialog on key rotation instead of a silent reconnect.
- Per-connection approval prompt with a separate audio grant, and per-lookup
  nonces binding rendezvous responses to their request.

### Application

- One binary: Qt Quick GUI by default, `--host` / `--view` for headless use.
- Peer codes (`swift-tiger-4271`) instead of typing 64 hex characters.
- Address book with pinning, recent connections, and an optional cloud-synced
  device mesh for signed-in users.
- Layout-independent keyboard: the physical key travels (PS/2 set 1) and the
  host applies its own layout, so a viewer on a Cyrillic or CJK layout types
  correctly instead of dropping every key it cannot name.
- Bidirectional text clipboard sync.
- Borderless fullscreen toggle, aspect-ratio control, an in-stream overlay
  menu, and an F9 diagnostics HUD.
- Start at login on Windows and Linux; system tray with sharing status.
- In-app update check and announcement banner, both against static endpoints
  and both switchable off.

### Servers

- `vivora-rendezvous` and `vivora-relay` as standalone Qt-free binaries with
  hardened systemd units, self-hostable from this repository under AGPL-3.0.
- Optional Ed25519 license gate on the relay for operators who want one.

### Known limitations at 0.1

- No file transfer, no gamepad forwarding, no mobile clients.
- No software encoder fallback: a host with no usable GPU encoder cannot share.
- Linux hosts advertise multiple displays but cannot switch between them yet.
- Linux HDR is an 8-bit pipeline.
- Linux relative-mouse capture works on X11 only; Wayland forbids the pointer
  warp it needs.
- macOS binaries are not published — build from source until the signing
  certificate is in place.
