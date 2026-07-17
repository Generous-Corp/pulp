#!/usr/bin/env bash
# ensure_signing_ready.sh — make macOS Developer ID signing + notarization work
# NON-INTERACTIVELY (no keychain GUI dialog, no 1Password prompt). Idempotent and
# self-healing: safe to run before every sign/notarize, in CI, or over SSH.
#
# WHY THIS EXISTS
#   Two things make `codesign` / `notarytool` pop a GUI prompt on this kind of
#   host, which wedges headless/SSH/CI signing:
#     1. The signing key lives in the *login* keychain, so `codesign` asks the
#        user to "allow access" (and 1Password may intercept if the cert/app
#        password is brokered through it).
#     2. `notarytool` driven by a keychain *profile* re-prompts when the login
#        keychain locks, and the profile itself periodically vanishes when the
#        login keychain churns.
#   The hardened fix, which this script materializes:
#     - Sign from a DEDICATED keychain whose key is authorized for codesign via
#       `security set-key-partition-list` — never the login keychain, never 1Password.
#     - Notarize from a file-based App Store Connect API key (.p8) — no keychain
#       at all, so nothing to lock or lose.
#
# CONFIG (secrets live OUTSIDE the repo — never commit these):
#   ~/.config/pulp/secrets/keychain.env   dedicated-keychain + .p12 identity
#   ~/.config/pulp/secrets/notary.env     App Store Connect .p8 API key
#   Each value may also be supplied via the same-named environment variable;
#   env wins over the file. Override the secrets dir with PULP_SECRETS_DIR, or
#   point at a single notary env file with PULP_NOTARY_ENV.
#
#   keychain.env keys:
#     PULP_SIGN_KEYCHAIN        path to the dedicated *.keychain-db
#     PULP_SIGN_KEYCHAIN_PW     password for that keychain
#     PULP_SIGN_P12             path to the Developer ID .p12 (cert + key)
#     PULP_SIGN_P12_PW          .p12 export password
#     PULP_SIGN_IDENTITY_HASH   SHA-1 of the Developer ID Application cert
#     PULP_SIGN_INSTALLER_HASH  (optional) SHA-1 of the Developer ID Installer cert
#   notary.env keys:
#     PULP_NOTARY_KEY_PATH / PULP_NOTARY_KEY_ID / PULP_NOTARY_ISSUER_ID
#
# USAGE
#   tools/scripts/ensure_signing_ready.sh                 # heal + report, exit 0 = ready
#   tools/scripts/ensure_signing_ready.sh --check-online  # also prove notary creds vs Apple (read-only)
#   tools/scripts/ensure_signing_ready.sh --print-env     # emit resolved identity/keychain for callers
#   tools/scripts/ensure_signing_ready.sh --quiet         # only emit errors + final status
#
# EXIT CODES
#   0  signing ready (notarization too, unless creds absent — see summary lines)
#   1  signing NOT ready (no usable identity in any keychain)
#   2  configuration error (bad flag, secrets dir unreadable)
#
# The script NEVER prints secret values. It is deliberately written against the
# stock `security` / `xcrun` / `codesign` tools so it is testable with PATH shims.

set -euo pipefail

# ── flags ────────────────────────────────────────────────────────────────────
CHECK_ONLINE=0
PRINT_ENV=0
QUIET=0
for arg in "$@"; do
  case "$arg" in
    --check-online) CHECK_ONLINE=1 ;;
    --print-env)    PRINT_ENV=1 ;;
    --quiet)        QUIET=1 ;;
    -h|--help)      awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0 ;;
    *) echo "ensure_signing_ready: unknown flag '$arg'" >&2; exit 2 ;;
  esac
done

# --print-env is meant for `eval "$(... --print-env)"`: keep stdout to the env
# lines only, so the human-readable progress/summary never pollutes the eval.
[ "$PRINT_ENV" -eq 1 ] && QUIET=1

say()  { [ "$QUIET" -eq 1 ] || printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }

# Converge the user keychain search list to a deduplicated set that contains ONLY
# keychains still present on disk, with the dedicated signing keychain ($1) first
# and every other surviving entry (login keychain, etc.) kept in its original
# relative order.
#
# This must CONVERGE, not accumulate: signing/notarization churn deletes temp and
# scratch keychains, and a naive "prepend when absent" rebuild neither prunes the
# resulting dangling entries nor deduplicates, so the list grows without bound and
# fills with keychain files that no longer exist. securityd walks that list on
# every codesign, so a bloated list of dangling entries is a real signing
# slowness/flakiness source. Convergence makes the operation idempotent: running
# it N times leaves the same list as running it once.
#
# The `security list-keychains` read/write is the injectable seam — the whole
# script is written against stock `security`, so a PATH shim can drive this logic
# against a fake list without touching a real keychain.
converge_keychain_search_list() {
  local want="$1"
  local line entry i
  local -a current=() converged=() seen=()

  while IFS= read -r line; do
    line="${line#"${line%%[![:space:]]*}"}"          # ltrim
    line="${line%"${line##*[![:space:]]}"}"           # rtrim
    line="${line%\"}"; line="${line#\"}"              # strip surrounding quotes
    [ -n "$line" ] || continue
    current+=("$line")
  done < <(security list-keychains -d user)

  # An empty read is treated as a transient failure: never write an empty list,
  # which would wipe the login keychain from the search path.
  if [ "${#current[@]}" -eq 0 ]; then
    warn "keychain search list read empty — leaving it unchanged"
    return 0
  fi

  _seen_has() { local x; for x in "${seen[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }

  # The dedicated keychain is authoritative (just created/unlocked/imported above),
  # so it always leads the list and is never subject to the existence prune below.
  converged+=("$want"); seen+=("$want")

  # Keep every other entry that STILL EXISTS on disk, once, in original order.
  # Dropping duplicates and dangling (deleted) keychains is the convergence.
  for entry in "${current[@]}"; do
    [ "$entry" = "$want" ] && continue     # already leads the list
    _seen_has "$entry" && continue          # dedup
    [ -f "$entry" ] || continue             # prune dangling
    converged+=("$entry"); seen+=("$entry")
  done

  # Idempotent write: only touch the search list when the converged form differs
  # from what is already installed, so a steady state is a genuine no-op.
  if [ "${#converged[@]}" -eq "${#current[@]}" ]; then
    for i in "${!converged[@]}"; do
      [ "${converged[$i]}" = "${current[$i]}" ] || { security list-keychains -d user -s "${converged[@]}"; return 0; }
    done
    return 0
  fi
  security list-keychains -d user -s "${converged[@]}"
}

SECRETS_DIR="${PULP_SECRETS_DIR:-$HOME/.config/pulp/secrets}"

# Load an env file WITHOUT clobbering values already set in the environment
# (env wins over file). Tolerates a missing file.
load_env_file() {
  local file="$1"
  [ -f "$file" ] || return 0
  local key val line
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|\#*) continue ;; esac
    case "$line" in *=*) ;; *) continue ;; esac   # skip malformed (no '=') lines
    key="${line%%=*}"
    case "$key" in PULP_*) ;; *) continue ;; esac
    # only set if not already in the environment
    if [ -z "${!key:-}" ]; then
      val="${line#*=}"
      # strip surrounding single/double quotes and expand a leading ~ / $HOME
      val="${val%\"}"; val="${val#\"}"
      val="${val%\'}"; val="${val#\'}"
      val="${val/#\~/$HOME}"
      val="${val//\$HOME/$HOME}"
      export "$key=$val"
    fi
  done < "$file"
}

[ -d "$SECRETS_DIR" ] || warn "secrets dir not found: $SECRETS_DIR (relying on environment only)"
load_env_file "$SECRETS_DIR/keychain.env"
load_env_file "$SECRETS_DIR/notary.env"

SIGN_READY=0
NOTARY_READY=0

# ── 1. dedicated signing keychain (kills the codesign / 1Password prompt) ──────
KC="${PULP_SIGN_KEYCHAIN:-}"
if [ -n "$KC" ] && [ -n "${PULP_SIGN_KEYCHAIN_PW:-}" ]; then
  if [ ! -f "$KC" ]; then
    say "• creating dedicated keychain"
    security create-keychain -p "$PULP_SIGN_KEYCHAIN_PW" "$KC"
  fi
  # Unlock + disable auto-lock so it never re-locks mid-session (the re-lock is
  # what reintroduces the prompt).
  security unlock-keychain -p "$PULP_SIGN_KEYCHAIN_PW" "$KC"
  security set-keychain-settings "$KC"   # no -t => no inactivity auto-lock timeout

  # Match the ready check below ("Developer ID Application"): a keychain holding
  # only a Developer ID *Installer* cert must still trigger the .p12 import.
  if ! security find-identity -v -p codesigning "$KC" 2>/dev/null | grep -q "Developer ID Application"; then
    if [ -n "${PULP_SIGN_P12:-}" ] && [ -f "${PULP_SIGN_P12:-}" ]; then
      say "• importing Developer ID .p12 into dedicated keychain"
      security import "$PULP_SIGN_P12" -k "$KC" -P "${PULP_SIGN_P12_PW:-}" \
        -T /usr/bin/codesign -T /usr/bin/security -T /usr/bin/productsign >/dev/null
    else
      warn "no identity in $KC and no importable .p12 (PULP_SIGN_P12)"
    fi
  fi

  # THE zero-prompt step: authorize codesign/productsign to use the private key
  # without an interactive "allow access" dialog. Idempotent.
  security set-key-partition-list -S apple-tool:,apple:,codesign: \
    -s -k "$PULP_SIGN_KEYCHAIN_PW" "$KC" >/dev/null 2>&1 \
    || warn "set-key-partition-list failed (signing may prompt)"

  # Ensure the keychain is on the user search list so bare `codesign --sign HASH`
  # resolves it even without an explicit --keychain. Converge the list every run
  # (dedup + prune deleted keychains, spaced paths preserved as single tokens) so
  # it never accumulates dangling entries — see converge_keychain_search_list.
  converge_keychain_search_list "$KC"

  if security find-identity -v -p codesigning "$KC" 2>/dev/null | grep -q "Developer ID Application"; then
    SIGN_READY=1
    say "• signing keychain READY: $(basename "$KC")"
  fi
fi

# Fallback: a usable Developer ID already in the default search list (login).
# Works, but WILL prompt — flag it loudly.
if [ "$SIGN_READY" -eq 0 ]; then
  if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
    SIGN_READY=1
    warn "signing falls back to the login keychain — codesign MAY prompt. Configure $SECRETS_DIR/keychain.env for the hardened path."
  fi
fi

# ── 2. notarization via file-based .p8 (kills the notarytool prompt) ───────────
KP="${PULP_NOTARY_KEY_PATH:-}"
if [ -n "$KP" ] && [ -f "$KP" ] && [ -n "${PULP_NOTARY_KEY_ID:-}" ] && [ -n "${PULP_NOTARY_ISSUER_ID:-}" ]; then
  NOTARY_READY=1
  say "• notary .p8 READY: $(basename "$KP")"
  # The file-based .p8 is the canonical, durable notary path (no keychain, nothing
  # to lock or lose). The `pulp-notary` keychain profile is only a convenience for
  # tools invoked with --keychain-profile; (re)minting it requires an Apple
  # round-trip to validate, and notarytool offers no reliable offline existence
  # check — so we fold it into --check-online, where a round-trip is already
  # expected. The default run stays fully offline and deterministic.
  if [ "$CHECK_ONLINE" -eq 1 ]; then
    if xcrun notarytool history --key "$KP" --key-id "$PULP_NOTARY_KEY_ID" \
         --issuer "$PULP_NOTARY_ISSUER_ID" >/dev/null 2>&1; then
      say "• notary creds verified against Apple (read-only)"
      # Self-heal the convenience profile from the same .p8 (covers keychain churn).
      xcrun notarytool store-credentials "pulp-notary" \
        --key "$KP" --key-id "$PULP_NOTARY_KEY_ID" --issuer "$PULP_NOTARY_ISSUER_ID" \
        >/dev/null 2>&1 && say "• keychain profile 'pulp-notary' refreshed" \
        || warn "could not refresh keychain profile (file-based .p8 still works)"
    else
      warn "notary creds did NOT validate against Apple"
      NOTARY_READY=0
    fi
  fi
else
  warn "notary .p8 not fully configured (PULP_NOTARY_KEY_PATH/_ID/_ISSUER_ID) — notarization will be skipped"
fi

# ── optional machine-readable export for callers (e.g. pulp ship) ──────────────
if [ "$PRINT_ENV" -eq 1 ]; then
  [ -n "${PULP_SIGN_IDENTITY_HASH:-}" ] && echo "PULP_SIGN_IDENTITY_HASH=$PULP_SIGN_IDENTITY_HASH"
  [ -n "$KC" ]                          && echo "PULP_SIGN_KEYCHAIN=$KC"
  [ -n "$KP" ]                          && echo "PULP_NOTARY_KEY_PATH=$KP"
fi

# ── summary ────────────────────────────────────────────────────────────────────
say ""
say "signing:      $([ "$SIGN_READY" -eq 1 ] && echo READY || echo 'NOT READY')"
say "notarization: $([ "$NOTARY_READY" -eq 1 ] && echo READY || echo 'NOT CONFIGURED')"

if [ "$SIGN_READY" -eq 1 ]; then exit 0; fi
err "no usable Developer ID signing identity found in any keychain"
exit 1
