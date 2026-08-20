# Packaging — Vivora

Templates and identity files for OS-specific bundling.  The GUI binary is
`vivora` on all three platforms and all three are bootstrapped; what
differs is distribution.  Windows and Linux get published artefacts
(installer / portable zip / AppImage); macOS builds from source until the
signing certificate is in place.

## Layout

```
packaging/
├── macos/
│   └── Info.plist.in                 configure_file()'d into Vivora.app
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

## Apple Developer notes (for whoever does notarization)

The bundle id `dev.vivora.app` must be registered in App Store Connect
*before* a Developer ID Application certificate can be issued for it.
That's a separate step from the $99/year membership — go to
"Identifiers" in developer.apple.com and add an explicit App ID.

Notarization flow once the cert is in hand:

```bash
codesign --deep --force --options runtime \
    --sign "Developer ID Application: Vivora Contributors (TEAMID)" \
    Vivora.app
xcrun notarytool submit Vivora.app.zip \
    --apple-id you@example.com --team-id TEAMID --password APP_SPECIFIC_PW \
    --wait
xcrun stapler staple Vivora.app
```

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

## Windows code signing notes (future)

For an MSI installer the same `dev.vivora.app` string goes into the
manifest `assemblyIdentity name`.  Use SignTool with whichever signing
cert we pick up (likely DigiCert EV — works without Defender SmartScreen
nag).
