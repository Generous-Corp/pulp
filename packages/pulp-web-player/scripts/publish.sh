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
# THE TOKEN — file-backed, never a prompt.
#
# The live path is a FILE with strict permissions:  ~/.config/pulp/secrets/npm-token
# (0600, inside a 0700 dir — the same convention as notary.env and pulp-signing.p12).
# 1Password holds a BACKUP copy; it is NOT the live path and is never read here.
#
# That distinction is the whole point: if a release needs a human to approve a
# 1Password prompt, the release is not unattended. It is the same reason the git
# signing key is a file (~/.ssh/github-signing-<machine>) with `IdentityAgent none`
# rather than the 1Password SSH agent. 1Password is the backup, not the live path.
#
# One-time per machine:  ./scripts/publish.sh --bootstrap   (reads 1Password ONCE,
# writes the 0600 file). Every publish after that is silent.
#
# npm 2FA here is a passkey with no typeable OTP, so this must be a GRANULAR token
# with "Bypass 2FA" enabled — the only kind that can publish non-interactively.
set -euo pipefail

readonly PKG="@danielraffel/web-player"
readonly TOKEN_FILE="${PULP_NPM_TOKEN_FILE:-$HOME/.config/pulp/secrets/npm-token}"
readonly OP_BACKUP_REF="op://Private/jd6btv2ja4rnoxcvmt2zy53nau/working token NO 2fa"  # BACKUP only
readonly MUST_SHIP=("src/shell.js" "src/index.js" "src/ui/file-upload.js" "src/state/plugin-state.js")

cd "$(dirname "$0")/.."
DRY_RUN=0; [[ "${1:-}" == "--dry-run" ]] && DRY_RUN=1

# ── --check-auth: is THIS machine set up to publish unattended? Answers the only
#    question that matters — will a publish stop and ask a human? Touches nothing.
if [[ "${1:-}" == "--check-auth" ]]; then
  [[ -f "$TOKEN_FILE" ]] || { printf '  ✗ no token at %s — run: ./scripts/publish.sh --bootstrap\n' "$TOKEN_FILE"; exit 1; }
  perms="$(stat -f '%Lp' "$TOKEN_FILE" 2>/dev/null || stat -c '%a' "$TOKEN_FILE")"
  [[ "$perms" == "600" ]] || { printf '  ✗ %s is mode %s — must be 0600\n' "$TOKEN_FILE" "$perms"; exit 1; }
  [[ "$(head -c4 "$TOKEN_FILE")" == "npm_" ]] || { printf '  ✗ %s does not hold an npm token\n' "$TOKEN_FILE"; exit 1; }
  printf '  ✓ %s (0600, npm_…)\n' "$TOKEN_FILE"
  printf '  ✓ unattended: the live path is this file — no 1Password, no agent, no prompt\n'
  printf '  ✓ 1Password holds the BACKUP only (restore with --bootstrap)\n'
  exit 0
fi

# ── bootstrap: materialise the file-backed token from the 1Password BACKUP. This is
#    the ONLY code path that touches 1Password, and it runs once per machine.
if [[ "${1:-}" == "--bootstrap" ]]; then
  command -v op >/dev/null || { printf '  1Password CLI (op) not found\n' >&2; exit 1; }
  mkdir -p "$(dirname "$TOKEN_FILE")"; chmod 700 "$(dirname "$TOKEN_FILE")"
  tok="$(op read "$OP_BACKUP_REF")" || { printf '  could not read the 1Password backup\n' >&2; exit 1; }
  [[ "$tok" == npm_* ]] || { printf '  that does not look like an npm token\n' >&2; exit 1; }
  umask 077; printf '%s\n' "$tok" > "$TOKEN_FILE"; chmod 600 "$TOKEN_FILE"; unset tok
  printf '  ✓ wrote %s (0600). Publishes are unattended from now on.\n' "$TOKEN_FILE"
  exit 0
fi

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

# ── 5. Auth: the LIVE path is a 0600 file. No 1Password, no agent, NO PROMPT.
#       Checked even on a dry run — an unattended publish that would have stopped to
#       ask a human is exactly the failure this script exists to rule out.
[[ -f "$TOKEN_FILE" ]] \
  || die "no token at $TOKEN_FILE — run once per machine: ./scripts/publish.sh --bootstrap"
perms="$(stat -f '%Lp' "$TOKEN_FILE" 2>/dev/null || stat -c '%a' "$TOKEN_FILE")"
[[ "$perms" == "600" ]] || die "$TOKEN_FILE is mode $perms — must be 0600"
[[ "$(head -c4 "$TOKEN_FILE")" == "npm_" ]] || die "$TOKEN_FILE does not contain an npm token"
say "✓ token: file-backed, 0600, unattended (1Password is the backup, not the live path)"

if (( DRY_RUN )); then
  printf '\n  DRY RUN — every guard passed, auth is unattended; not publishing.\n'
  exit 0
fi

# ── 5. Publish. Token read at run time; npmrc is 0600 and shredded on ANY exit. ─
TMPNPMRC="$(mktemp -t webplayer-npmrc.XXXXXX)"
cleanup() { rm -f "$TMPNPMRC"; }
trap cleanup EXIT INT TERM
chmod 600 "$TMPNPMRC"

token="$(< "$TOKEN_FILE")"
[[ "$token" == npm_* ]] || die "$TOKEN_FILE does not contain an npm token"
printf '//registry.npmjs.org/:_authToken=%s\n' "$token" > "$TMPNPMRC"
unset token

npm publish --userconfig "$TMPNPMRC" --access public

# ── 6. Verify the registry actually serves the new version.
#       `npm view` reads through a cache and the registry itself takes a moment to
#       settle, so a single eager check right after publish reports the OLD version
#       and cries wolf on a publish that in fact succeeded. Retry with backoff and
#       read the registry directly rather than trusting the cache.
for attempt in 1 2 3 4 5 6; do
  published="$(curl -fsS "https://registry.npmjs.org/${PKG}" 2>/dev/null \
    | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{console.log(JSON.parse(s)["dist-tags"].latest)}catch{console.log("?")}})')"
  [[ "$published" == "$VERSION" ]] && break
  sleep $(( attempt * 5 ))
done
[[ "$published" == "$VERSION" ]] \
  || die "published, but after ~105s the registry still reports '$published' — check npm"
printf '\n  ✓ %s@%s is live\n' "$PKG" "$VERSION"
