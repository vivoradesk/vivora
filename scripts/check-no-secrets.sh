#!/usr/bin/env bash
# Fail if anything that must never be published has made it into git.
#
# The signing key for Pro license tokens (license.sk) lives in the working
# tree of whoever runs the relay, right next to a .gitignore entry telling
# git to skip it.  One `git add -f`, one `git stash -u`, one packaging
# script that tars the source tree, and it is public forever.  This check
# is cheap; run it in CI and from a pre-commit hook.
#
#   scripts/check-no-secrets.sh            # check the whole tracked tree
#   scripts/check-no-secrets.sh --staged   # check what is about to be committed
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ "${1:-}" == "--staged" ]]; then
    mapfile -t files < <(git diff --cached --name-only --diff-filter=ACMR)
    what="staged"
else
    mapfile -t files < <(git ls-files)
    what="tracked"
fi

# Filenames that are never legitimate in this repository.
FORBIDDEN_NAMES='(^|/)(license\.sk|license\.pk|token\.bin|.*\.pem|.*\.p12|.*\.pfx|id_rsa|id_ed25519|oracle\.key)$'

fail=0
for f in "${files[@]:-}"; do
    [[ -z "$f" ]] && continue
    if [[ "$f" =~ $FORBIDDEN_NAMES ]]; then
        echo "REFUSED: $f is $what but must never be committed" >&2
        fail=1
    fi
done

# Content sweep for private-key blocks pasted into a source or config file.
# Restricted to the file list above so it stays fast on a full checkout.
for f in "${files[@]:-}"; do
    [[ -z "$f" ]] && continue
    [[ -f "$f" ]] || continue
    case "$f" in third_party/*) continue ;; esac
    if head -c 65536 -- "$f" 2>/dev/null | grep -qE 'BEGIN (RSA |EC |OPENSSH |PGP )?PRIVATE KEY'; then
        echo "REFUSED: $f contains a private key block" >&2
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo "" >&2
    echo "Nothing was committed.  If a match is a false positive, narrow the" >&2
    echo "pattern in scripts/check-no-secrets.sh rather than skipping the hook." >&2
    exit 1
fi

echo "check-no-secrets: ok (${#files[@]} $what files)"
