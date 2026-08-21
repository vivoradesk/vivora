#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andrii Myronov
# SPDX-License-Identifier: AGPL-3.0-or-later

# Sign Vivora.app, and do not leave a half-signed bundle behind if that fails.
#
#   codesign-app.sh <identity> <path/to/Vivora.app>
#
# Signing with a keychain certificate needs an unlocked keychain, which an SSH
# session does not have: codesign fails with errSecInternalComponent and the
# build stops after the link. What is left on disk then still runs -- the
# linker's own ad-hoc signature is valid -- so the failure is easy to miss
# while looking at a build log, and the bundle carries the wrong signing
# identifier, which is what TCC keys grants on.
#
# So: try the configured identity, and if it will not sign, say so plainly and
# fall back to ad-hoc rather than failing the build. Ad-hoc is a downgrade in
# one respect only, that its designated requirement is the cdhash and so
# permission grants die on the next rebuild.
set -uo pipefail

IDENTITY="${1:?usage: $0 <identity> <app bundle>}"
APP="${2:?usage: $0 <identity> <app bundle>}"
BUNDLE_ID="dev.vivora.app"

sign() {
    codesign --force --sign "$1" --identifier "$BUNDLE_ID" "$APP" 2>&1
}

if out=$(sign "$IDENTITY"); then
    echo "$out"
    echo "Codesigned $APP as $BUNDLE_ID (identity: $IDENTITY)"
else
    echo "$out" >&2
    if [ "$IDENTITY" = "-" ]; then
        echo "codesign failed even ad-hoc -- the bundle is not signed" >&2
        exit 1
    fi
    echo "" >&2
    echo "  codesign could not use the identity '$IDENTITY'." >&2
    echo "  The usual cause is a locked keychain, which is what an SSH" >&2
    echo "  session always has; build from the Mac's own terminal, or run" >&2
    echo "  'security unlock-keychain', to sign with the certificate." >&2
    echo "  Falling back to an ad-hoc signature so the build completes." >&2
    echo "  Consequence: TCC grants will not survive the next rebuild." >&2
    echo "" >&2
    if out=$(sign "-"); then
        echo "$out"
        echo "Codesigned $APP as $BUNDLE_ID (ad-hoc fallback)"
    else
        echo "$out" >&2
        exit 1
    fi
fi

# Reset the permission grants on every build, deliberately.
#
# An ad-hoc signature changes its cdhash each time, so the old grants are dead
# anyway and leaving them makes System Settings accumulate stale entries. With
# a certificate they would survive -- and we still clear them, because a test
# run should start from the same place a new user does. Never fatal.
tccutil reset ScreenCapture "$BUNDLE_ID" >/dev/null 2>&1 || true
tccutil reset Accessibility "$BUNDLE_ID" >/dev/null 2>&1 || true
echo "Reset ScreenCapture and Accessibility grants for $BUNDLE_ID"
