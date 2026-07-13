#!/usr/bin/env bash
# Publish @danielraffel/web-player to npm — safely, from any machine, unattended.
#
#   ./scripts/publish.sh            # publish
#   ./scripts/publish.sh --dry-run  # do everything except the publish itself
#
# WHY THIS EXISTS. The obvious way to publish this package is to cd into a checkout
# and run `npm publish`. That is how it gets published WRONG: a stale checkout (a
# feature branch, an old clone) ships source that does not match `main` under a
# fresh version number, and npm versions are immutable — you cannot take it back.
# That exact footgun was live: a saved note pointed at a clone parked on an old
# branch which did not contain the file-upload code at all.
#
# So this script refuses to publish unless the tree it is publishing IS origin/main,
# clean, tested, and carrying the files it claims to carry.
#
# THE TOKEN never touches disk in plaintext and is never held in a persistent npmrc.
# It lives in 1Password (which already syncs across the machines), is read at run
# time, written to a 0600 temp npmrc, and shredded on exit — including on failure.
# npm 2FA is a passkey with no typeable OTP, so the token is a GRANULAR token with
# "Bypass 2FA" enabled; that is the only combination that can publish non-interactively.
set -euo pipefail

readonly PKG="@danielraffel/web-player"
readonly OP_REF="op://Private/jd6btv2ja4rnoxcvmt2zy53nau/working token NO 2fa"
readonly MUST_SHIP=("src/shell.js" "src/index.js" "src/ui/file-upload.js" "src/state/plugin-state.js")

cd "$(dirname "$0")/.."
DRY_RUN=0; [[ "${1:-}" == "--dry-run" ]] && DRY_RUN=1

die() { printf '\n  ✗ %s\n' "$*" >&2; exit 1; }
say() { printf '  %s\n' "$*"; }

# ── 1. We publish origin/main. Nothing else. ────────────────────────────────────
git fetch origin main --quiet
[[ -z "$(git status --porcelain -- .)" ]] \
  || die "the package tree is dirty — commit or stash before publishing"
head="$(git rev-parse HEAD)"
main="$(git rev-parse origin/main)"
if [[ "$head" != "$main" ]]; then
  git merge-base --is-ancestor "$head" "$main" 2>/dev/null \
    && die "HEAD is BEHIND origin/main — you would publish stale source. Check out origin/main."
  die "HEAD is not origin/main ($(git rev-parse --short HEAD) vs $(git rev-parse --short origin/main)) — refusing to publish a branch."
fi
say "✓ publishing origin/main ($(git rev-parse --short HEAD))"

VERSION="$(node -p "require('./package.json').version")"
say "✓ version $VERSION"

# ── 2. That version must not already exist. npm versions are immutable. ─────────
if npm view "$PKG@$VERSION" version >/dev/null 2>&1; then
  die "$PKG@$VERSION is ALREADY published — bump the version first (npm versions are immutable)."
fi
say "✓ $VERSION is unpublished"

# ── 3. Tests must pass on the exact commit being published. ─────────────────────
npm test >/dev/null 2>&1 || die "tests FAIL on this commit — refusing to publish"
say "✓ tests pass"

# ── 4. The tarball must actually contain what we think it does. A packaging or
#       `files` regression silently ships an empty-ish package otherwise.
manifest="$(npm pack --dry-run --json 2>/dev/null)"
for f in "${MUST_SHIP[@]}"; do
  grep -q "\"$f\"" <<<"$manifest" || die "tarball is MISSING $f — packaging regression, not publishing"
done
say "✓ tarball carries all ${#MUST_SHIP[@]} required entrypoints"

if (( DRY_RUN )); then
  printf '\n  DRY RUN — everything passed; not publishing.\n'
  exit 0
fi

# ── 5. Publish. Token read at run time; npmrc is 0600 and shredded on ANY exit. ─
command -v op >/dev/null || die "1Password CLI (op) not found — needed to read the npm token"
TMPNPMRC="$(mktemp -t webplayer-npmrc.XXXXXX)"
cleanup() { rm -f "$TMPNPMRC"; }
trap cleanup EXIT INT TERM
chmod 600 "$TMPNPMRC"

token="$(op read "$OP_REF" 2>/dev/null)" \
  || die "could not read the npm token from 1Password. Is 1Password unlocked? ($OP_REF)"
[[ "$token" == npm_* ]] || die "the value in 1Password does not look like an npm token"
printf '//registry.npmjs.org/:_authToken=%s\n' "$token" > "$TMPNPMRC"
unset token

npm publish --userconfig "$TMPNPMRC" --access public

# ── 6. Verify the registry actually serves the new code. ────────────────────────
sleep 5
published="$(npm view "$PKG" version 2>/dev/null || echo '?')"
[[ "$published" == "$VERSION" ]] \
  || die "published, but the registry still reports $published — check npm"
printf '\n  ✓ %s@%s is live\n' "$PKG" "$VERSION"
