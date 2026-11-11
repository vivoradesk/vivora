#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build the macOS release artefact: a signed, optionally notarised .dmg.
#
#   scripts/package-macos.sh <build-dir> <dist-dir> <identity> [--notarize]
#
#   <identity>   a "Developer ID Application: ..." certificate in the login
#                keychain, or "-" for an ad-hoc dry run that exercises every
#                step but signing for real (and can run over SSH)
#   --notarize   submit to Apple with the notarytool keychain profile
#                "vivora" (VIVORA_NOTARY_PROFILE overrides) and staple the
#                ticket to the dmg
#
# The build dir must be configured against an official Qt (qt.io / aqt), not
# Homebrew's: brew bottles are built for the OS they were bottled on, so a
# brew-Qt build only runs on that macOS version or newer.  See
# packaging/README.md.
#
# Run it from the Mac's own Terminal: codesign and notarytool both need the
# login keychain unlocked, and an SSH session never has it unlocked.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD="${1:?usage: $0 <build-dir> <dist-dir> <identity> [--notarize]}"
DIST="${2:?usage: $0 <build-dir> <dist-dir> <identity> [--notarize]}"
IDENTITY="${3:?usage: $0 <build-dir> <dist-dir> <identity> [--notarize]}"
NOTARIZE=0
[[ "${4:-}" == "--notarize" ]] && NOTARIZE=1
PROFILE="${VIVORA_NOTARY_PROFILE:-vivora}"
ENTITLEMENTS="$ROOT/packaging/macos/entitlements.plist"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "macOS only." >&2; exit 1
fi
if [[ "$NOTARIZE" == 1 && "$IDENTITY" == "-" ]]; then
    echo "An ad-hoc signature cannot be notarised." >&2; exit 1
fi

# shellcheck disable=SC1091
source "$BUILD/artefact.env"
BASE="$VIVORA_ARTEFACT_BASENAME"          # Vivora-0.1.1-macos-universal
APP_SRC="$VIVORA_BINDIR/Vivora.app"
[[ -d "$APP_SRC" ]] || { echo "$APP_SRC not found; build first." >&2; exit 1; }

# macdeployqt from the same Qt the build used, never whichever is on PATH.
QT_PREFIX="$(sed -n 's/^CMAKE_PREFIX_PATH:[A-Z]*=//p' "$BUILD/CMakeCache.txt" | cut -d';' -f1)"
MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
[[ -x "$MACDEPLOYQT" ]] || { echo "macdeployqt not found under $QT_PREFIX" >&2; exit 1; }
case "$QT_PREFIX" in
    /opt/homebrew/*|/usr/local/Cellar/*|/usr/local/opt/*)
        echo "Refusing to package against Homebrew Qt ($QT_PREFIX)." >&2
        echo "It only runs on the macOS it was bottled for." >&2
        exit 1 ;;
esac

# ------------------------------------------------------------- stage -----
STAGE="$BUILD/stage/$BASE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
echo "==> staging $BASE"
# ditto keeps the bundle's symlinks and extended attributes intact.
ditto "$APP_SRC" "$STAGE/Vivora.app"
APP="$STAGE/Vivora.app"

echo "==> macdeployqt"
"$MACDEPLOYQT" "$APP" -qmldir="$ROOT/qml" -verbose=1

# Anything still pointing outside the bundle would work here and fail on
# every other Mac, which is exactly the failure this step exists to prevent.
leaks=$(find "$APP/Contents" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 |
    xargs -0 otool -L 2>/dev/null |
    grep -E '^\s+/(opt/homebrew|usr/local|Users)/' || true)
if [[ -n "$leaks" ]]; then
    echo "The bundle still links outside itself:" >&2
    echo "$leaks" >&2
    exit 1
fi

# -------------------------------------------------------------- sign -----
# Inside out: a container's signature covers its contents, so every nested
# binary must be signed before whatever holds it.  --deep is not used because
# it signs nested code with the outer options and is deprecated for this.
sign() {
    local flags=(--force --sign "$IDENTITY")
    if [[ "$IDENTITY" != "-" ]]; then
        flags+=(--options runtime --timestamp)
    fi
    codesign "${flags[@]}" "$@"
}

echo "==> signing nested code"
while IFS= read -r -d '' f; do
    sign "$f"
done < <(find "$APP/Contents" -type f -name '*.dylib' -print0)
while IFS= read -r -d '' f; do
    sign "$f"
done < <(find "$APP/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0)

echo "==> signing Vivora.app"
sign --identifier dev.vivora.app --entitlements "$ENTITLEMENTS" "$APP"
codesign --verify --strict --deep --verbose=2 "$APP"

# --------------------------------------------------------------- dmg -----
mkdir -p "$DIST"
DMG="$DIST/$BASE.dmg"
rm -f "$DMG"
echo "==> building $DMG"
DMGROOT="$BUILD/stage/dmg-root"
rm -rf "$DMGROOT"
mkdir -p "$DMGROOT"
ditto "$APP" "$DMGROOT/Vivora.app"
ln -s /Applications "$DMGROOT/Applications"
hdiutil create -volname "Vivora" -srcfolder "$DMGROOT" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$DMGROOT"
sign "$DMG"

if [[ "$NOTARIZE" == 1 ]]; then
    echo "==> notarising (profile: $PROFILE); this takes a few minutes"
    out=$(xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait 2>&1) || true
    echo "$out"
    if ! grep -q 'status: Accepted' <<<"$out"; then
        id=$(sed -n 's/^ *id: //p' <<<"$out" | head -1)
        [[ -n "$id" ]] && xcrun notarytool log "$id" --keychain-profile "$PROFILE" >&2 || true
        echo "Notarisation was not accepted." >&2
        exit 1
    fi
    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
    spctl --assess --type open --context context:primary-signature -vv "$DMG"
fi

# One checksum file per dist directory, appended to so artefacts built on
# other machines can join the same list (same rule as the other platforms).
SUMS="$DIST/SHA256SUMS.txt"
name="$(basename "$DMG")"
line="$(shasum -a 256 "$DMG" | cut -d' ' -f1)  $name"
{ [[ -f "$SUMS" ]] && grep -v "  $name\$" "$SUMS" || true; echo "$line"; } > "$SUMS.tmp"
mv "$SUMS.tmp" "$SUMS"
echo "$line"
