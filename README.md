# Vivora

**Low-latency remote desktop that you can read, build and host yourself.**

[![License: AGPL v3](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)

Vivora streams a desktop over a custom UDP protocol with a GPU-to-GPU video
path: the frame is captured into GPU memory, encoded there, and on the other
side decoded and presented without ever touching system memory. The design
target is under 15 ms glass-to-glass on a LAN — close enough to local that
dragging a window feels attached to the mouse.

It exists because the fast remote desktops are closed and the open ones are
slow. Vivora is AGPL-3.0: the client, the host, the rendezvous server and the
relay are all here, and you can run every piece of the infrastructure yourself.

> **0.1 is the first public release.** The streaming pipeline has been in daily
> use for months, but this is the first build packaged for people who did not
> write it. Expect rough edges in the desktop app before you expect them in the
> stream.

---

## What works

| | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Host (share your screen) | yes | yes | yes, build from source |
| Client (view a screen) | yes | yes | yes, build from source |
| Hardware encode | NVENC, AMD AMF, Intel QSV | VAAPI, NVENC | VideoToolbox |
| Hardware decode | MediaFoundation / D3D11VA | VAAPI, software fallback | VideoToolbox |
| HDR passthrough | yes (HEVC Main10) | no, 8-bit pipeline | yes |
| Audio | yes | yes | yes |
| Clipboard (text) | yes | yes | yes |
| Multi-monitor pick | yes | host advertises, switching not yet implemented | yes |
| Start at login | yes | yes | not yet |

We publish binaries for **Windows and Linux**. macOS builds and runs from
source; signed and notarised macOS downloads need an Apple Developer account
and are the first thing on the 0.1.1 list.

**Not here yet:** file transfer, gamepad forwarding, mobile clients, software
encoder fallback for machines with no usable GPU encoder.

---

## Install

Grab the latest build from [Releases](https://github.com/vivoradesk/vivora/releases).

**Windows** — run the `.msi` (installs per-user, no administrator prompt), or
unzip the portable build anywhere. Windows will warn about an unrecognised app:
the binaries are not code-signed yet, so verify the SHA-256 against
`SHA256SUMS.txt` in the release and choose *More info → Run anyway*.

**Linux** — download the `.AppImage`, `chmod +x` it, run it. If your system has
no libfuse2 and the AppImage refuses to mount, run it with
`--appimage-extract-and-run`.

To **control** a Linux machine remotely (keyboard and mouse injection) the app
needs write access to `/dev/uinput`, which is root-only by default:

```sh
sudo cp packaging/linux/60-vivora-uinput.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo modprobe uinput
sudo usermod -aG input "$USER"      # then log out and back in
```

Without this the video stream still works and input silently does not.

---

## Use it

Open Vivora on both machines. The one being controlled shows a **peer code**
like `swift-tiger-4271` — type it on the other machine and press Connect. The
host gets a prompt and has to accept. That is the whole flow; there is no
account required and nothing to configure.

The first time you connect to a host, its public key is pinned. If it ever
changes you get a warning instead of a silent reconnection.

There is also a headless CLI, which is what you want on a server:

```sh
vivora --host                                # share this screen
vivora --view 192.168.1.50                   # connect over the LAN
vivora --view --peer swift-tiger-4271        # connect through rendezvous
```

`vivora --help` lists the rest: `--port`, `--codec h264|hevc`, `--encoder`,
`--bitrate`, `--display`, `--rendezvous`, `--relay`, `--no-stun`.

---

## Build from source

The build is three commands. Assembling the dependencies is the part that takes
a few minutes, and **Qt 6.5 or newer is the one that catches people out** --
most distributions still ship something older. Read your platform's section
below first; these commands assume it is done.

```sh
git clone --recursive https://github.com/vivoradesk/vivora.git
cd vivora
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<your Qt>
cmake --build build
ctest --test-dir build
```

If you forgot `--recursive`: `git submodule update --init --recursive`.

### Linux

```sh
sudo apt install build-essential cmake ninja-build pkg-config git \
    libavcodec-dev libavutil-dev libswscale-dev \
    libpipewire-0.3-dev libdbus-1-dev libpulse-dev libwayland-dev \
    libgl-dev libegl-dev libglx-dev libopengl-dev \
    libxkbcommon-dev libxkbcommon-x11-dev \
    libx11-dev libx11-xcb-dev libxcb1-dev libfontconfig1-dev
```

That is the list CI installs, so it is known to be sufficient.

Then Qt 6, with `Core Widgets Qml Quick QuickControls2 Network OpenGL
OpenGLWidgets`. Vivora is built against **Qt 6.8**, and anything older than 6.5
will not do -- Ubuntu 24.04 still ships 6.4, so the distribution package is
usually not an option. The quickest route is
[aqtinstall](https://github.com/miurahr/aqtinstall):

```sh
python3 -m venv ~/venv && ~/venv/bin/pip install aqtinstall
~/venv/bin/aqt install-qt linux desktop 6.8.2 linux_gcc_64 \
    -m qtdeclarative qtshadertools --outputdir ~/qt6
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=~/qt6/6.8.2/gcc_64
```

Hosting on Linux needs a working `xdg-desktop-portal` with the ScreenCast
interface (any current GNOME or KDE session has one) and a VAAPI or NVENC
capable GPU.

### Windows

Needs MSVC 2022 and Qt 6.8 or newer. A stock Qt installation works:

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<qt>/msvc2022_64
cmake --build build
```

The **released** binaries are built against a statically linked Qt so that
`vivora.exe` is a single self-contained file with no runtime to install. That
is the only difference; point `CMAKE_PREFIX_PATH` at a static Qt and the same
commands produce it.

A static Qt needs one extra thing, and only a static one. Qt's network module
implements PBKDF2 with the OpenSSL 3 key-derivation API; in a shared build
`Qt6Network.dll` carries that itself, but linked statically the symbols have to
come from somewhere. Point `VIVORA_OPENSSL3_STATIC_DIR` at a directory holding
`libcrypto_static.lib` from an OpenSSL 3 `/MT` build (the default is
`C:/OpenSSL-WinUniversal/lib/VC/x64/MT`). No Vivora traffic goes through
OpenSSL either way — TLS is Schannel — and a shared-Qt build never asks for it.

### macOS

```sh
brew install qt cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build build
```

The result is `build/bin/Vivora.app`. Two permissions have to be granted before
it does anything useful, and neither is optional: **Screen Recording** for
capture, and **Accessibility** for remote keyboard and mouse. macOS asks for
each the first time it is needed; without Accessibility, input is dropped
silently.

The build signs the bundle ad-hoc, which is enough to run it. That signature
changes on every rebuild, so macOS treats each build as a new application and
asks for both permissions again — see
[`docs/macos-codesign.md`](docs/macos-codesign.md) for a self-signed
certificate that makes the grants stick.

---

## Run your own infrastructure

Direct connections need nothing but a reachable IP. Everything beyond that is
optional and self-hostable:

- **`vivora-rendezvous`** (UDP 7000) maps peer codes to addresses and helps two
  machines behind NAT punch a hole to each other. Signalling only — kilobytes
  per session, no media.
- **`vivora-relay`** (UDP 7100) forwards the stream when hole punching fails
  (symmetric NAT, CGNAT, corporate firewalls). It sees ciphertext only, but it
  does carry the full bitrate, so budget bandwidth accordingly.

Both are single Qt-free binaries with a hardened systemd unit in
[`deploy/selfhost/`](deploy/selfhost). See [`deploy/README.md`](deploy/README.md).
Point the app at yours in Settings, or with `--rendezvous host:port` and
`--relay host:port`.

---

## What Vivora sends where

Everything below is documented rather than buried, because "open-source remote
desktop" and "phones home" deserve to be reconciled explicitly.

| Endpoint | When | What |
| --- | --- | --- |
| `rdv.vivora.dev:7000` | while sharing, and when connecting by peer code | your public key and address, so peers can find you |
| `relay.vivora.dev:7100` | only if a direct connection fails **and** you have Pro | encrypted stream payload |
| `stun.l.google.com:19302` | at connect time | a STUN binding request to discover your public address |
| `vivora.dev/version.json` | at startup | nothing — a plain GET to compare version numbers |
| `cloud.vivora.dev` | only if you sign in | account, device list, license |

There is no analytics, no telemetry and no crash reporting. Every endpoint above
is configurable in Settings; point them at your own servers or clear them.
The media stream itself is peer-to-peer and end-to-end encrypted regardless.

---

## Security

Transport is `Noise_IK_25519_ChaChaPoly_BLAKE2b` on
[Monocypher](https://monocypher.org/) — Curve25519 key exchange,
ChaCha20-Poly1305 for every channel including audio, with a per-channel replay
window. A host's Curve25519 public key is its identity, pinned on first use.

Not TLS, and no OpenSSL in the media path: the handshake is two messages and
the whole implementation is small enough to read.

Found a vulnerability? [`SECURITY.md`](SECURITY.md) — please do not open a
public issue.

---

## Licensing

The client, host, rendezvous server and relay are **AGPL-3.0-or-later**. Use
them, change them, run them, redistribute them; if you offer a modified version
to others over a network, publish your source.

**Vivora Pro** ($9.90/month) is a commercial license that lifts the AGPL
obligation, plus access to the Vivora-operated relay and cloud address-book
sync. It buys convenience and a legal position, not features held hostage — the
open build streams at full quality with nothing switched off. Details at
[vivora.dev](https://vivora.dev).

---

## Contributing

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first — it explains why the
code is shaped the way it is, which is most of what a first patch needs.
[`CONTRIBUTING.md`](CONTRIBUTING.md) covers the practical parts.
[`STATUS.md`](STATUS.md) is the honest per-platform state of everything.

The one rule worth stating up front: **latency is the primary metric**. A change
that makes the code cleaner and the pipeline slower will be turned down.

---

## Copyright and contact

Copyright (C) 2026 Andrii Myronov. Licensed under the GNU Affero General Public
License v3 or later — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Every
file Vivora owns carries an SPDX header; anything under `third_party/` keeps
its own, listed in
[`packaging/THIRD-PARTY-NOTICES.md`](packaging/THIRD-PARTY-NOTICES.md).

- Bugs and features: [GitHub issues](https://github.com/vivoradesk/vivora/issues)
- Security: **security@vivora.dev** — read [`SECURITY.md`](SECURITY.md) first,
  and please do not use the issue tracker for these
- Anything else: **hello@vivora.dev**
