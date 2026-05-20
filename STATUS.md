# Vivora — Project Status

Last updated: 2026-05-07

A snapshot of what works, what's planned, and the open questions.
For the architectural overview see `CLAUDE.md`.

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
| Input capture (KB+mouse) | ✅ Raw Input                  | ✅ NSEvent + IOKit modifiers   | ✅ Qt events (Qt-key→Win-VK table)   |
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
| macOS    | VideoToolbox          | `src/host/encode/mac_videotoolbox_encoder.{h,mm}` | All Apple GPUs   | **HEVC only**   | ✅            |
| Linux    | VAAPI (libavcodec)    | `src/host/encode/vaapi_encoder.{h,cpp}`           | **Intel iHD only (tested)** | H.264 + HEVC | ❌ 8-bit pipeline |

Auto-probe order on Windows: AMF → NVENC → QSV (`encoder_factory.cpp`). The `--encoder` flag forces a specific backend.

#### Client-side decoders

| Platform | Backend | File | Vendors | Codecs | HW fallback |
| -------- | ------- | ---- | ------- | ------ | ----------- |
| Windows  | MediaFoundation D3D11VA   | `src/client/decode/mf_decoder.{h,cpp}`        | NVIDIA + AMD + Intel (vendor-agnostic via D3D11) | H.264 + HEVC | ❌ none |
| macOS    | VideoToolbox (AVSampleBufferDisplayLayer) | `src/client/render/mac_video_view.mm` | All Apple GPUs | H.264 + HEVC | ❌ none |
| Linux    | FFmpeg ± VAAPI HW         | `src/client/decode/ffmpeg_decoder.{h,cpp}`    | Intel (primary), AMD radeonsi (untested), NVIDIA via nvidia-vaapi-driver (untested) | H.264 + HEVC | ✅ silent SW fallback on HW init fail |

#### Known gaps in coverage

These are **not** on the active roadmap — listed so we know what to say when a user reports "doesn't work on my X."

1. **Linux + NVIDIA host** — there's no native NVENC path on Linux. NVIDIA users would need to install `nvidia-vaapi-driver` so the existing VAAPI encoder picks up the GeForce; not a smooth experience. A direct NVENC-on-Linux backend would be ~2 days of work mirroring the Windows NVENC code, but it's untested ground.
2. **Linux + AMD host** — VAAPI through `radeonsi` should work in theory. We never tested. First report from an RDNA / Radeon user will tell us.
3. **No software encoder fallback anywhere.** If HW encoder init fails (driver mismatch, headless server, container without DRM nodes), the host bails. An x264 path would gain server / VM / very-old-hardware compatibility at the cost of CPU.
4. **macOS host: H.264 missing.** VideoToolbox supports H.264 — we only wired HEVC. Matters when a non-HEVC client connects.
5. **NVENC Windows: HEVC only.** Intentional today (everyone has HEVC), but breaks against an H.264-only client (very old Android viewer, etc.). AMF + QSV cover both codecs.
6. **Client SW fallback on Windows / macOS** — same shape as Linux's SW fallback. Not in today.

### Network / protocol stack (cross-platform)

| Component                                | Status |
| ---------------------------------------- | ------ |
| UDP video transport                      | ✅      |
| FEC (Reed-Solomon)                       | ✅ adaptive M with sticky floor + keyframe boost |
| Retransmits (NACK)                       | ✅      |
| Adaptive bitrate (RTT-K)                 | ✅      |
| Adaptive framerate (PerfReport)          | ✅      |
| Adaptive audio jitter buffer             | ✅ 30..200 ms, grows on PLC / shrinks in quiet |
| Crypto (Noise_NK + ChaCha20-Poly1305)    | ✅ handshake, transport, audio, replay window |
| TOFU peer pinning (`known_peers.txt`)    | ✅      |
| Rendezvous + memorable codes             | ✅ public server on Oracle Free Tier |
| Hole-punching (STUN + PunchHint)         | ✅ tested inter-ISP (1123ms handshake) |
| Same-NAT short-circuit (LAN candidates)  | ✅ tested (11ms handshake same WiFi) |
| Relay daemon (`vivora-relay`)          | ✅ AGPL self-host code, smoke-tested localhost |
| Relay client/host integration            | ✅ `--relay HOST:PORT`, lazy BIND on first need |
| Relay license auth (Pro-managed)         | ✅ Ed25519-signed 95B tokens, `--require-license` on relay |
| Rendezvous-minted relay session_id       | ✅ rdv `--relay-endpoint` advertises endpoint+session_id; peers auto-adopt |
| Relay auto-fallback (direct → relay)     | ✅ client switches after 3 s of no HELLO_ACK, resets Noise on relay path |

### Production deploy artefacts

Live public rendezvous: **`89.168.124.37:7000`** on Oracle Cloud Always Free (ARM, 1 GB RAM). Operations:

- Logs:    `ssh ubuntu@89.168.124.37 'sudo journalctl -u vivora-rendezvous -f'`
- Update:  rerun `deploy/deploy-rendezvous.sh ubuntu@89.168.124.37` (auto-restarts the unit)
- Memory: 600 KB RSS at idle, 64 MiB cap

### Open-source hygiene

- Single binary `vivora` (host + client + view) — `--host` / `--view` flags pick mode.
- Standalone server binary `vivora-rendezvous`.
- AGPL on the table for client + relay; closed-core for the future console.
- Tests: `noise`, `fec`, `fragment`, `audio` build green on Linux + Windows.

---

## What's pending

In priority order. Tier numbers match the historical roadmap.

### P2 — networking / reach (1 step left)

5.2 **Relay daemon** for symmetric-NAT cases — see the cost/benefit analysis at the bottom of this doc. This is the only remaining P2 item.

### P3 — input expansion

12. **Scancode-based keyboard.** Today the Linux client emits Qt key codes mapped to Windows VK via a hand-written ASCII-only table. Switch every platform to scancodes (`QKeyEvent::nativeScanCode`, X11 keycodes, `SendInput KEYEVENTF_SCANCODE`) so non-Latin layouts (Cyrillic, CJK) work. Host derives the eventual character from its own active layout.
13. **Gamepad forwarding.** XInput on Windows source, IOKit HID on Mac, evdev on Linux. ViGEm or equivalent virtual-pad on the host side.

### P4 — mobile clients (viewer-only)

14. **iOS client** — reuse the macOS VideoToolbox decode path.
15. **Android client** — JNI + MediaCodec wrapper.

### P5 — UX / shipping

16. **UI polish (QML).** Replace the CLI flag soup with a home screen, recent connections, settings panel.
17. **Fullscreen toggle hotkey** (F11 / Ctrl+Shift+F, auto-hide titlebar).
18. **Stream zoom + pan** when host aspect ≠ client aspect (e.g. 21:9 host on 16:9 client). Mouse mapping must stay correct after the transform.
19. **Clipboard sync** — text first, then files via control channel.
20. **File transfer** — drag-drop into the stream window.
21. **TOFU UI flow** — today the pin file is plaintext; surface mismatches in the UI rather than logs only.
22. **Packaging + auto-update.** AppImage (Linux), MSI (Windows), notarised .dmg (macOS).

### Always-Free relay shopping list (if/when we host)

- Default rendezvous URL baked into the client (today users pass `--rendezvous` manually).
- Domain (`rdv.vivora.dev`) with A-record at the Oracle IP.
- Document `--rendezvous` self-host path for org users.

### Shelved (intentionally not on the roadmap)

- **METADATA cursor mode on Linux** — needs portal ≥ 1.18 (Ubuntu 24.04+). We target 20.04 / 22.04 as the floor.
- **Linux DMA-BUF zero-copy** (capture→encode + decode→render). Big architectural piece (~week+); the SHM path is functional and Linux user share doesn't justify the investment yet.
- **Wayland relative-mouse path** on Linux client — needs `zwp_relative_pointer_v1` + pointer constraints. X11 covers 24.04/26.04.

---

## Monetization model (per docs/PROJECT_SPEC.md, updated 2026-05-08)

The 0.1 launch ships with a paid tier from day one.  Spec change moves us from "open OSS, paid console later" to a clear dual-licensing split.

### Open / closed split

| Component                                  | License            | Hosted by us                    |
| ------------------------------------------ | ------------------ | ------------------------------- |
| `vivora` (client + host)                 | **AGPL-3.0**       | n/a — runs on user machines     |
| `vivora-rendezvous`                      | **AGPL-3.0**       | ✅ free public (Oracle Free Tier) |
| `vivora-relay`                           | **AGPL-3.0**       | **Pro only** — see below         |
| `vivora-cloud` (accounts, licenses, sync)| Closed proprietary | Pro infrastructure              |
| `vivora-console` (web admin)             | Closed proprietary | Future (≥0.3)                   |

Anyone can self-host the relay + rendezvous from the AGPL repo.  The Vivora-operated relay is gated behind a Pro account.

### Free tier — AGPL, personal use

- Full streaming quality, every client feature (multi-monitor, all codecs, HDR, etc.)
- Direct IP connection — unlimited
- Rendezvous via the public free server (or self-hosted)
- LAN candidates / hole-punching — full P2 stack
- Local address book (no cloud sync)
- AGPL forces open-source for any commercial use → effectively personal-use-only without paying

### Pro tier — $9.90 / mo or $99 / year (0.1 launch)

- **Commercial license** (drops the AGPL obligation — standard MongoDB / Grafana model)
- **Access to the Vivora-operated public relay** (closes the symmetric-NAT / CGNAT gap without self-hosting)
- **Cloud-synced address book** across user's devices
- Priority support
- Checkout via Paddle (Merchant of Record for UA — handles tax + chargebacks)

**Anti-piracy posture:** Pro features are gated server-side. The AGPL client itself never blocks anything; it just can't authenticate against the Pro endpoints (managed relay, address-book sync) without a valid license token. Patching the binary buys nothing.

### What this means for relay engineering

The relay code is **needed** — both as the AGPL self-host artefact and as the foundation of the Pro managed relay. Two things to build:

1. **`vivora-relay` daemon** — UDP forwarder, allocate-on-demand. Open-source, self-hostable.  Same shape as the rendezvous: small standalone binary, systemd unit, deploy script.
2. **License auth for the managed instance** — only the Pro-operated relay checks license tokens. Self-hosted instances skip the check (no `--require-license` flag, or off by default).

NAT-traversal coverage with this in place:

| Scenario                                     | Today                  | With relay (Pro)        |
| -------------------------------------------- | ---------------------- | ----------------------- |
| Home WiFi ↔ home WiFi (different ISPs)        | ✅ STUN punch ~95%      | ✅ same                  |
| Same NAT (one router)                        | ✅ LAN candidate        | ✅ same                  |
| Mobile (4G/5G), one side                     | ⚠️ ~50–70% (CGNAT)      | ✅ relay covers the rest |
| Corporate / UDP-blocked                      | ❌ broken               | ✅ relay if reachable    |
| Self-host relay (Free tier)                  | n/a                    | ✅ same coverage         |

### Bandwidth cost reminder (informs Pro pricing)

Relay carries the full media stream. At 1080p60 HEVC ≈ 15 Mbps:

- 1 hour ≈ 6.7 GB
- 8 h/day × 30 days ≈ 1.6 TB / month / heavy user

Provider economics (egress):

| Provider              | Plan / month | Bandwidth                         | Notes                                |
| --------------------- | ------------ | --------------------------------- | ------------------------------------ |
| Oracle Cloud Free     | $0           | 10 TB egress / mo                 | Fits the rendezvous; not enough for relay at any meaningful scale |
| Hetzner CCX23         | €27          | 20 TB included, €1/TB after       | ~12–15 sustained streams; main candidate for Pro relay |
| AWS / GCP             | $$$          | $0.05–0.09 / GB                   | Out of the question for streaming relay |

At $9.90/mo a Pro user has to bring in less than one Hetzner CCX23's worth of bandwidth to be cost-neutral — comfortable headroom even for heavy users.

### Roadmap impact

- The "do we host a free public relay" question is **closed: no.** Public relay is a Pro feature.
- The relay daemon code (open-source, AGPL) is the next chunk of P2 step 5.2.
- A new tier (P6 — 0.1 launch monetization) covers the closed-source infra: license JWT system, account backend, Paddle integration, address-book sync API, relay license-token check.
