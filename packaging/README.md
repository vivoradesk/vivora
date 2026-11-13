# Packaging — Vivora

Templates and identity files for OS-specific bundling.  The GUI binary is
`vivora` on all three platforms and all three are bootstrapped; what
differs is distribution: a signed installer and portable zip on Windows, a
signed and notarised `.dmg` on macOS, an AppImage on Linux.

## Layout

```
packaging/
├── macos/
│   ├── Info.plist.in                 configure_file()'d into Vivora.app
│   └── entitlements.plist            hardened-runtime entitlements for the
│                                     release signature
├── linux/
│   ├── vivora.desktop.in             installed as dev.vivora.app.desktop
│   ├── dev.vivora.app.metainfo.xml   AppStream metainfo (Flathub, GNOME
│   │                                 Software).  Its <releases> block must
│   │                                 be updated on every release.
│   └── 60-vivora-uinput.rules        udev rule granting the "input" group
│                                     access to /dev/uinput, without which a
│                                     Linux host cannot inject remote input
└── windows/
    └── vivora.exe.manifest           embedded via .rc once installer flow lands
```

## Reverse-DNS app id

`dev.vivora.app` is the canonical bundle id across all platforms:

| Where                       | Value                            |
|-----------------------------|----------------------------------|
| macOS `CFBundleIdentifier`  | `dev.vivora.app`                 |
| Linux `.desktop`            | filename `dev.vivora.app.desktop`, `Exec=vivora` |
| Linux AppStream metainfo    | `<id>dev.vivora.app</id>`        |
| Windows assembly identity   | `name="dev.vivora.app"`          |
| GCD dispatch_queue labels   | `dev.vivora.audio_capture`, `dev.vivora.capture` |
| QSettings org domain        | `vivora.dev` (Qt reverse-DNS'es it for macOS) |

## Building the macOS artefact

`scripts/package-macos.sh` turns a build into a universal, signed and
notarised `Vivora-<version>-macos-universal.dmg`.

**Use the official Qt, not Homebrew's.** Homebrew bottles are built for the
macOS they were bottled on — its QtQuick currently requires macOS 26 — so a
bundle made from them would not start on anything older. Install the
official universal Qt with [aqt](https://github.com/miurahr/aqtinstall):

```bash
python3 -m venv ~/aqtvenv && ~/aqtvenv/bin/pip install aqtinstall
~/aqtvenv/bin/aqt install-qt mac desktop 6.8.3 clang_64 --outputdir ~/Qt
```

Configure for both architectures (the macOS floor, 14, is set in
`CMakeLists.txt`) and build:

```bash
cmake -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos \
    "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-rel
```

Package it. The identity is the name of a *Developer ID Application*
certificate in the login keychain (`security find-identity -v -p codesigning`
lists it), or `-` for an ad-hoc dry run that exercises every other step:

```bash
scripts/package-macos.sh build-rel dist "Developer ID Application: …" --notarize
```

The script runs `macdeployqt`, drops the SQL driver plugins (pulled in by
QtQuick.LocalStorage, unused, and linked against libraries no user has),
refuses a bundle that still links outside itself, signs every nested binary
inside out with the hardened runtime and a secure timestamp, builds and signs
the `.dmg`, and with `--notarize` submits it, staples the ticket and checks it
with `spctl`. It appends the `.dmg` to `dist/SHA256SUMS.txt`.

`--notarize` uses a notarytool keychain profile named `vivora`
(`VIVORA_NOTARY_PROFILE` overrides). Create it once, with an app-specific
password from account.apple.com:

```bash
xcrun notarytool store-credentials vivora
```

**Run all of this from the Mac's own Terminal**, not over SSH: codesign and
notarytool need the login keychain unlocked, and an SSH session never has it.
See [`docs/macos-codesign.md`](../docs/macos-codesign.md) for the development
signing setup that keeps permission grants across rebuilds.

## Building the Windows artefacts

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
scripts\package-windows.ps1            # portable zip
scripts\package-windows.ps1 -Msi       # zip + installer
```

Everything is staged through `cmake --install`, never by copying from the
source tree -- the working tree holds `license.sk`, and a packaging script
that globs is one bad pattern away from publishing it.

The `.msi` needs the **WiX toolset v5**, a dotnet global tool:

```powershell
dotnet tool install --global wix --version 5.0.2
```

Pinned to v5 deliberately.  WiX v6 and v7 are gated behind the Open Source
Maintenance Fee: v7 refuses to build at all until you accept its EULA, which
is a licensing decision and not one a build script should make.  v5 is MIT.

`icons/win/vivora.ico` is checked in rather than generated at build time --
otherwise every Windows contributor needs an image converter for an asset that
changes about once a year.  Regenerate it with `scripts/make-ico.ps1` after
editing the source PNGs.

### What the installer does

Per-user, into `%LOCALAPPDATA%\Programs\Vivora`, so it never raises a UAC
prompt.  Start menu shortcut, an entry in Settings -> Apps, and a working
uninstall.  Settings under `HKCU\Software\Vivora` deliberately survive an
uninstall.

The `UpgradeCode` GUID in `vivora.wxs` must never change: it is how Windows
recognises a later release as an upgrade rather than a second parallel
install.

## Building the Linux artefacts

Both are built inside the image from `linux/Containerfile`, which is Debian 11
plus a from-source Qt and OpenSSL 3.  Debian 11 on purpose: glibc 2.31 is the
oldest thing worth targeting, and a binary linked against it runs on everything
newer.  Building on a current distribution instead would lock out Ubuntu 20.04,
Debian 11 and the whole RHEL 9 family.

```sh
podman build -t vivora-build -f packaging/linux/Containerfile .   # ~2.5h, cached
podman run --rm -v "$PWD:/work" -w /work vivora-build     bash scripts/package-linux-appimage.sh build-appimage dist
podman run --rm -v "$PWD:/work" -w /work vivora-build     bash scripts/package-servers.sh build-servers dist
```

The image compiles Qt, so the first build takes hours and every one after that
is a normal incremental build.  Do not build the release artefacts outside it:
the servers built on Ubuntu 22.04 will not even start on Debian 11.

Then check the result somewhere it was not built:

```sh
scripts/smoke-appimage.sh dist/Vivora-0.1.0-linux-x64.AppImage
scripts/smoke-appimage.sh dist/*.AppImage docker.io/library/rockylinux:9
```

That script has earned its place several times over.  It caught a missing
libxcb-cursor (Qt dlopens it, so nothing deploys it and Qt aborts), a
libxkbcommon pair split between bundle and host (segfault before the first
window), and a Qt built against the wrong OpenSSL major (no HTTPS at all,
which the app is perfectly happy to start without).

## Windows code signing

Releases are signed with a Certum Open Source Code Signing certificate kept in
Certum's cloud (SimplySign).  The private key never leaves the cloud: SimplySign
Desktop exposes it to Windows as a virtual smart card, and only while a session
is open.  A session needs a one-time code from the SimplySign phone app and
lasts about two hours, so signing is a manual release step and cannot run in
CI.

```powershell
# 1. Connect SimplySign Desktop (tray icon -> Connect, code from the phone app)
# 2. Build and package as usual, adding -Sign
scripts\package-windows.ps1 -Msi -Sign
```

`-Sign` signs `vivora.exe` in staging, before it is zipped and packed into the
MSI, then signs the MSI, and verifies each signature as it goes.  It needs the
Windows SDK for `signtool`.  With one code-signing certificate in the user's
store it picks that one; with several, pass `-SignThumbprint`.

Every signature carries an RFC 3161 timestamp from `time.certum.pl`, so it
stays valid after the certificate's one-year term ends.  A renewed certificate
changes nothing here.

SmartScreen still warns about a new certificate until it builds up reputation
through downloads; an OV certificate only shortens that period.  The signature
already replaces "Unknown publisher" with the certificate's name.
