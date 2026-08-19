#!/usr/bin/env bash
# Install the repository's git hooks into .git/hooks.
#
# Hooks are not versioned by git itself, so this has to be run once per
# clone.  Today it installs a single pre-commit hook that refuses to commit
# signing keys and other secrets (see check-no-secrets.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
hooks="$(git rev-parse --git-path hooks)"
mkdir -p "$hooks"

cat > "$hooks/pre-commit" <<'HOOK'
#!/usr/bin/env bash
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
exec "$root/scripts/check-no-secrets.sh" --staged
HOOK

chmod +x "$hooks/pre-commit"
echo "installed $hooks/pre-commit"
