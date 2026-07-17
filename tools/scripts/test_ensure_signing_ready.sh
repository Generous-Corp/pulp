#!/usr/bin/env bash
# test_ensure_signing_ready.sh — unit tests for ensure_signing_ready.sh.
#
# Drives the doctor against PATH-shimmed `security` / `xcrun` / `codesign` and a
# throwaway secrets dir, so it touches no real keychain and never calls Apple.
# The shims are steered by env vars the test sets per case:
#   SHIM_IDENTITY_IN_DEDICATED=1   `find-identity <kc>` reports a Developer ID
#   SHIM_IDENTITY_IN_LOGIN=1       bare `find-identity` reports a Developer ID
#   SHIM_LOG=<file>                shims append their argv here (call assertions)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCTOR="$SCRIPT_DIR/ensure_signing_ready.sh"
[ -x "$DOCTOR" ] || { echo "FATAL: $DOCTOR not executable"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ── PATH shims ────────────────────────────────────────────────────────────────
SHIMBIN="$TMP/bin"
mkdir -p "$SHIMBIN"

cat > "$SHIMBIN/security" <<'SH'
#!/usr/bin/env bash
[ -n "${SHIM_LOG:-}" ] && echo "security $*" >> "$SHIM_LOG"
cmd="${1:-}"
case "$cmd" in
  find-identity)
    # last arg is a keychain path only when an explicit keychain is passed
    last="${@: -1}"
    if [[ "$last" == *.keychain-db ]]; then
      if [ "${SHIM_IDENTITY_INSTALLER_ONLY:-0}" = "1" ]; then
        echo '  1) AAA "Developer ID Installer: Test (TEAMID0000)"'   # NOT an Application cert
      elif [ "${SHIM_IDENTITY_IN_DEDICATED:-0}" = "1" ]; then
        echo '  1) ABC "Developer ID Application: Test (TEAMID0000)"'
      fi
    else
      [ "${SHIM_IDENTITY_IN_LOGIN:-0}" = "1" ] && \
        echo '  1) DEF "Developer ID Application: Test (TEAMID0000)"'
    fi
    ;;
  list-keychains)
    if [[ "$*" == *" -s "* ]]; then
      # write form: record the resulting argv one-per-line so tests can assert
      # exact tokenization (spaced paths must stay a SINGLE token).
      [ -n "${SHIM_ARGV_LOG:-}" ] && printf '%s\n' "$@" >> "$SHIM_ARGV_LOG"
    else
      # read form: emit the configured existing search list (default: login only)
      printf '%s\n' "${SHIM_EXISTING_KEYCHAINS:-    \"/login.keychain-db\"}"
    fi
    ;;
  create-keychain|unlock-keychain|set-keychain-settings|import|set-key-partition-list) : ;;
  find-generic-password) exit 1 ;;
  *) : ;;
esac
exit 0
SH

cat > "$SHIMBIN/xcrun" <<'SH'
#!/usr/bin/env bash
[ -n "${SHIM_LOG:-}" ] && echo "xcrun $*" >> "$SHIM_LOG"
# notarytool history / store-credentials both "succeed" silently
exit 0
SH

chmod +x "$SHIMBIN/security" "$SHIMBIN/xcrun"

# ── fixtures ──────────────────────────────────────────────────────────────────
make_secrets() {   # $1 = dir ; writes keychain.env (+ notary.env if $2=with-notary)
  local d="$1"; mkdir -p "$d"
  cat > "$d/keychain.env" <<EOF
PULP_SIGN_KEYCHAIN="$d/pulp-signing.keychain-db"
PULP_SIGN_KEYCHAIN_PW="kcpw"
PULP_SIGN_P12="$d/id.p12"
PULP_SIGN_P12_PW="p12pw"
PULP_SIGN_IDENTITY_HASH="DEADBEEF"
EOF
  : > "$d/id.p12"
  if [ "${2:-}" = "with-notary" ]; then
    : > "$d/AuthKey_TEST.p8"
    cat > "$d/notary.env" <<EOF
PULP_NOTARY_KEY_PATH="$d/AuthKey_TEST.p8"
PULP_NOTARY_KEY_ID="KEYID12345"
PULP_NOTARY_ISSUER_ID="issuer-uuid-here"
EOF
  fi
}

PASS=0 FAIL=0
ok()   { PASS=$((PASS+1)); echo "ok   - $1"; }
bad()  { FAIL=$((FAIL+1)); echo "FAIL - $1"; }

run_doctor() {  # runs doctor with shims + isolated env; sets OUT / RC
  OUT="$(PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$1" \
         SHIM_IDENTITY_IN_DEDICATED="${2:-0}" SHIM_IDENTITY_IN_LOGIN="${3:-0}" \
         env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH \
         bash "$DOCTOR" "${@:4}" 2>&1)" && RC=0 || RC=$?
}

# ── cases ─────────────────────────────────────────────────────────────────────

# 1. identity in dedicated keychain + notary .p8 => READY/READY, exit 0
S="$TMP/s1"; make_secrets "$S" with-notary
run_doctor "$S" 1 0
{ [ "$RC" -eq 0 ] && grep -q "signing:      READY" <<<"$OUT" \
  && grep -q "notarization: READY" <<<"$OUT"; } \
  && ok "dedicated identity + .p8 => READY/READY exit0" \
  || bad "dedicated identity + .p8 => READY/READY exit0 (rc=$RC)"$'\n'"$OUT"

# 2. signing-only (no notary.env) => signing READY, notarization NOT CONFIGURED, exit 0
S="$TMP/s2"; make_secrets "$S"
run_doctor "$S" 1 0
{ [ "$RC" -eq 0 ] && grep -q "signing:      READY" <<<"$OUT" \
  && grep -q "notarization: NOT CONFIGURED" <<<"$OUT"; } \
  && ok "no notary creds => signing READY, notarization NOT CONFIGURED" \
  || bad "no notary creds case (rc=$RC)"$'\n'"$OUT"

# 3. no identity anywhere => NOT READY, exit 1
S="$TMP/s3"; make_secrets "$S" with-notary
run_doctor "$S" 0 0
{ [ "$RC" -eq 1 ] && grep -q "signing:      NOT READY" <<<"$OUT"; } \
  && ok "no identity => NOT READY exit1" \
  || bad "no identity => NOT READY exit1 (rc=$RC)"$'\n'"$OUT"

# 4. login-keychain fallback => READY but warns about a possible prompt
S="$TMP/s4"; mkdir -p "$S"   # no keychain.env at all
run_doctor "$S" 0 1
{ [ "$RC" -eq 0 ] && grep -q "falls back to the login keychain" <<<"$OUT"; } \
  && ok "login fallback => READY + prompt warning" \
  || bad "login fallback case (rc=$RC)"$'\n'"$OUT"

# 5. --print-env emits identity/keychain/keypath and NO secret values
S="$TMP/s5"; make_secrets "$S" with-notary
run_doctor "$S" 1 0 --quiet --print-env
{ grep -q "PULP_SIGN_IDENTITY_HASH=DEADBEEF" <<<"$OUT" \
  && grep -q "PULP_SIGN_KEYCHAIN=" <<<"$OUT" \
  && grep -q "PULP_NOTARY_KEY_PATH=" <<<"$OUT" \
  && ! grep -q "kcpw" <<<"$OUT" && ! grep -q "p12pw" <<<"$OUT"; } \
  && ok "--print-env emits handles, leaks no secrets" \
  || bad "--print-env case"$'\n'"$OUT"

# 6. unknown flag => config error exit 2
run_doctor "$TMP/s5" 1 0 --bogus
{ [ "$RC" -eq 2 ]; } && ok "unknown flag => exit2" || bad "unknown flag exit2 (rc=$RC)"

# 7. default run does NOT call notarytool (offline/deterministic);
#    --check-online DOES.
S="$TMP/s7"; make_secrets "$S" with-notary
LOG="$TMP/log7a"; RC=0; SHIM_LOG="$LOG" \
  PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$S" SHIM_IDENTITY_IN_DEDICATED=1 \
  env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH bash "$DOCTOR" --quiet >/dev/null 2>&1 || RC=$?
# offline AND actually completed (rc 0) — a premature set -e exit would also lack
# xcrun and falsely "pass", so assert both.
{ [ "$RC" -eq 0 ] && ! grep -q "xcrun" "$LOG" 2>/dev/null; } \
  && ok "default run is offline (no xcrun) and reaches READY" \
  || bad "default run offline+ready (rc=$RC, log:$(cat "$LOG" 2>/dev/null))"
LOG="$TMP/log7b"; SHIM_LOG="$LOG" \
  PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$S" SHIM_IDENTITY_IN_DEDICATED=1 \
  env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH bash "$DOCTOR" --quiet --check-online >/dev/null 2>&1 || true
grep -q "xcrun notarytool history" "$LOG" 2>/dev/null \
  && ok "--check-online verifies via notarytool history" \
  || bad "--check-online must call notarytool history"

# 8. env var wins over file (point KEY_PATH at a missing file => NOT CONFIGURED)
S="$TMP/s8"; make_secrets "$S" with-notary
OUT="$(PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$S" SHIM_IDENTITY_IN_DEDICATED=1 \
       PULP_NOTARY_KEY_PATH="$TMP/does-not-exist.p8" \
       env -u PULP_SIGN_KEYCHAIN bash "$DOCTOR" 2>&1)" && RC=0 || RC=$?
{ [ "$RC" -eq 0 ] && grep -q "notarization: NOT CONFIGURED" <<<"$OUT"; } \
  && ok "env PULP_NOTARY_KEY_PATH overrides file (missing => NOT CONFIGURED)" \
  || bad "env-wins case (rc=$RC)"$'\n'"$OUT"

# 9. Installer-only cert in the dedicated keychain must still trigger .p12 import
#    (regression guard for the "Developer ID" vs "Developer ID Application" mismatch).
S="$TMP/s9"; make_secrets "$S" with-notary
LOG="$TMP/log9"; SHIM_LOG="$LOG" \
  PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$S" SHIM_IDENTITY_INSTALLER_ONLY=1 \
  env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH bash "$DOCTOR" --quiet >/dev/null 2>&1 || true
grep -q "security import" "$LOG" 2>/dev/null \
  && ok "Installer-only identity still triggers .p12 import" \
  || bad "import must fire when only an Installer cert is present"$'\n'"$(cat "$LOG")"

# 10. A keychain path CONTAINING A SPACE must survive the search-list rebuild as a
#     single token, and an existing ON-DISK login keychain must be preserved.
S="$TMP/s10"; mkdir -p "$S"
SPACED_KC="$TMP/My Keychains/pulp-signing.keychain-db"; mkdir -p "$(dirname "$SPACED_KC")"; : > "$SPACED_KC"
SPACED_LOGIN="$TMP/My Keychains/login.keychain-db"; : > "$SPACED_LOGIN"
: > "$S/id.p12"
cat > "$S/keychain.env" <<EOF
PULP_SIGN_KEYCHAIN="$SPACED_KC"
PULP_SIGN_KEYCHAIN_PW="kcpw"
PULP_SIGN_P12="$S/id.p12"
PULP_SIGN_P12_PW="p12pw"
PULP_SIGN_IDENTITY_HASH="DEADBEEF"
EOF
ARGV="$TMP/argv10"; : > "$ARGV"
# existing search list ALSO has a spaced path, to prove the parser preserves it.
SHIM_EXISTING_KEYCHAINS="    \"$SPACED_LOGIN\"" \
SHIM_ARGV_LOG="$ARGV" \
  PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$S" SHIM_IDENTITY_IN_DEDICATED=1 \
  env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH bash "$DOCTOR" --quiet >/dev/null 2>&1 || true
# the dedicated spaced path must appear as exactly one argv line…
{ grep -Fxq "$SPACED_KC" "$ARGV" \
  && grep -Fxq "$SPACED_LOGIN" "$ARGV"; } \
  && ok "spaced keychain paths survive search-list rebuild as single tokens" \
  || bad "spaced-path search-list rebuild corrupted tokenization"$'\n'"argv:"$'\n'"$(cat "$ARGV")"

# ── search-list CONVERGENCE (dedup + prune + idempotence) ─────────────────────
# These drive the real search-list rebuild through the shimmed `security`
# read/write seam. Existence is a real `[ -f ]` on disk, so a "surviving" entry
# is an actual file under $TMP and a "dangling" entry is a path that does not
# exist. A search list must CONVERGE — never grow, never keep deleted keychains.
render_existing() {  # emit a `security list-keychains`-style block for $@ paths
  local p; for p in "$@"; do printf '    "%s"\n' "$p"; done
}
render_from_file() { while IFS= read -r p; do [ -n "$p" ] && printf '    "%s"\n' "$p"; done < "$1"; }
kc_paths_of() { grep -E '\.keychain-db$' "$1" 2>/dev/null || true; }  # written keychains
converge_run() {  # $1 secretsdir  $2 existing-list block  $3 argv-log
  SHIM_EXISTING_KEYCHAINS="$2" SHIM_ARGV_LOG="$3" \
    PATH="$SHIMBIN:$PATH" PULP_SECRETS_DIR="$1" SHIM_IDENTITY_IN_DEDICATED=1 \
    env -u PULP_SIGN_KEYCHAIN -u PULP_NOTARY_KEY_PATH \
    bash "$DOCTOR" --quiet >/dev/null 2>&1 || true
}

# 11. PRUNE: a search list carrying deleted (dangling) keychains must be rewritten
#     WITHOUT them — securityd should never be handed a keychain file that is gone.
S="$TMP/s11"; make_secrets "$S"
KC11="$S/pulp-signing.keychain-db"
LOGIN11="$TMP/login11.keychain-db"; : > "$LOGIN11"        # exists on disk
GONE11A="$TMP/gone-11a.keychain-db"                        # never created
GONE11B="$TMP/gone-11b.keychain-db"                        # never created
ARGV="$TMP/argv11"; : > "$ARGV"
converge_run "$S" "$(render_existing "$LOGIN11" "$GONE11A" "$GONE11B")" "$ARGV"
kc_paths_of "$ARGV" > "$TMP/paths11"
{ grep -Fxq "$KC11" "$TMP/paths11" && grep -Fxq "$LOGIN11" "$TMP/paths11" \
  && ! grep -Fxq "$GONE11A" "$TMP/paths11" && ! grep -Fxq "$GONE11B" "$TMP/paths11"; } \
  && ok "search list prunes dangling (deleted) keychains" \
  || bad "dangling keychains were NOT pruned from the search list"$'\n'"written:"$'\n'"$(cat "$TMP/paths11")"

# 12. DEDUP: duplicate entries (including the dedicated keychain itself) must
#     collapse to a single occurrence each.
S="$TMP/s12"; make_secrets "$S"
KC12="$S/pulp-signing.keychain-db"; : > "$KC12"
LOGIN12="$TMP/login12.keychain-db"; : > "$LOGIN12"
ARGV="$TMP/argv12"; : > "$ARGV"
converge_run "$S" "$(render_existing "$KC12" "$LOGIN12" "$KC12" "$LOGIN12")" "$ARGV"
kc_paths_of "$ARGV" > "$TMP/paths12"
{ grep -Fxq "$KC12" "$TMP/paths12" && grep -Fxq "$LOGIN12" "$TMP/paths12" \
  && [ -z "$(sort "$TMP/paths12" | uniq -d)" ]; } \
  && ok "search list deduplicates repeated keychains" \
  || bad "duplicate keychains survived the search-list rebuild"$'\n'"written:"$'\n'"$(cat "$TMP/paths12")"

# 13. IDEMPOTENCE: running twice must equal running once. Thread run-1's written
#     list back into run-2 and assert the stable list never accumulates — no
#     dangling, no duplicates, login + dedicated keychain still present.
S="$TMP/s13"; make_secrets "$S"
KC13="$S/pulp-signing.keychain-db"
LOGIN13="$TMP/login13.keychain-db"; : > "$LOGIN13"
GONE13="$TMP/gone-13.keychain-db"
A1="$TMP/argv13a"; : > "$A1"; A2="$TMP/argv13b"; : > "$A2"
converge_run "$S" "$(render_existing "$LOGIN13" "$GONE13" "$GONE13")" "$A1"
kc_paths_of "$A1" > "$TMP/stable13a"
converge_run "$S" "$(render_from_file "$TMP/stable13a")" "$A2"
kc_paths_of "$A2" > "$TMP/stable13b"
# run-2's write if it wrote, else exactly what run-2 was handed (a true no-op).
if [ -s "$TMP/stable13b" ]; then STABLE="$TMP/stable13b"; else STABLE="$TMP/stable13a"; fi
{ grep -Fxq "$KC13" "$STABLE" && grep -Fxq "$LOGIN13" "$STABLE" \
  && ! grep -Fxq "$GONE13" "$STABLE" && [ -z "$(sort "$STABLE" | uniq -d)" ]; } \
  && ok "search-list convergence is idempotent (N runs == 1 run)" \
  || bad "search list grew / retained dangling across repeated runs"$'\n'"stable:"$'\n'"$(cat "$STABLE")"

echo ""
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]
