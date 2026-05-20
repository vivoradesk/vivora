# Packaging — Vivora

Templates and identity files for OS-specific bundling.  Wired into the
build when each platform's GUI installer flow lands (see Phase 5 of the
DeskBeam → Vivora rename checklist).  Today the GUI binary is `vivora`
on all platforms but only the Windows GUI is fully bootstrapped; macOS
and Linux still ship the CLI.

## Layout

```
packaging/
├── macos/
│   └── Info.plist.in              configure_file()'d into Vivora.app
├── linux/
│   ├── vivora.desktop.in          (configure_file: writes vivora.desktop)
│   └── dev.vivora.app.metainfo.xml  AppStream metainfo for Flathub etc.
└── windows/
    └── vivora.exe.manifest        embedded via .rc once installer flow lands
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

## Windows code signing notes (future)

For an MSI installer the same `dev.vivora.app` string goes into the
manifest `assemblyIdentity name`.  Use SignTool with whichever signing
cert we pick up (likely DigiCert EV — works without Defender SmartScreen
nag).
