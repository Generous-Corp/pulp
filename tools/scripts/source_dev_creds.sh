#!/usr/bin/env bash
# source_dev_creds.sh — load Pulp Apple signing / notary creds into the
# current shell.
#
# Designed to be SOURCED, not executed. Examples:
#
#   . tools/scripts/source_dev_creds.sh
#   cmake -S . -B build-ios-device -G Xcode \
#     -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=$PULP_TEAM_ID \
#     -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development" \
#     -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE="Automatic"
#
# Lookup priority for the creds file:
#   1. $PULP_SECRETS_FILE                        (env override)
#   2. ./secrets/notary.env                      (project-local; useful in CI caches)
#   3. ~/.config/pulp/secrets/notary.env         (per-user default)
#
# Schema:  templates/secrets/notary.env.example
# Guide:   docs/guides/ios-dev-signing.md
#
# Errors out with a clear message when:
#   - no creds file is found in any of the lookup locations
#   - the file is found but a required key is missing
#   - the file is found but a required key still contains a "<...>" placeholder
#
# Idempotent: sourcing twice is a no-op (re-exports the same values).

# Detect sourced vs. executed — only sourced makes sense.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  printf 'source_dev_creds.sh: this script must be SOURCED, not executed.\n' >&2
  printf '  usage: . tools/scripts/source_dev_creds.sh\n' >&2
  exit 64
fi

_pulp_creds_err() {
  printf 'source_dev_creds.sh: %s\n' "$1" >&2
  printf '  see docs/guides/ios-dev-signing.md and templates/secrets/notary.env.example\n' >&2
  return 1
}

# Resolve creds file path.
_pulp_creds_file=""
if [ -n "${PULP_SECRETS_FILE:-}" ]; then
  if [ -f "$PULP_SECRETS_FILE" ]; then
    _pulp_creds_file="$PULP_SECRETS_FILE"
  else
    _pulp_creds_err "PULP_SECRETS_FILE='$PULP_SECRETS_FILE' set but file does not exist" || return 1
  fi
elif [ -f "./secrets/notary.env" ]; then
  _pulp_creds_file="./secrets/notary.env"
elif [ -f "$HOME/.config/pulp/secrets/notary.env" ]; then
  _pulp_creds_file="$HOME/.config/pulp/secrets/notary.env"
else
  _pulp_creds_err "no creds file found. Tried: \$PULP_SECRETS_FILE, ./secrets/notary.env, ~/.config/pulp/secrets/notary.env" || return 1
fi

# Source it. set -a exports every assignment.
set -a
# shellcheck disable=SC1090
. "$_pulp_creds_file"
set +a

# Validate required keys are set and don't still contain placeholders.
_pulp_required_keys="PULP_TEAM_ID PULP_SIGN_IDENTITY PULP_NOTARY_KEY_ID PULP_NOTARY_ISSUER_ID PULP_NOTARY_KEY_PATH"

_pulp_missing=""
_pulp_placeholder=""
for _k in $_pulp_required_keys; do
  _v=$(eval "printf '%s' \"\${${_k}:-}\"")
  if [ -z "$_v" ]; then
    _pulp_missing="$_pulp_missing $_k"
  elif printf '%s' "$_v" | grep -q '<.*>'; then
    _pulp_placeholder="$_pulp_placeholder $_k"
  fi
done

if [ -n "$_pulp_missing" ] || [ -n "$_pulp_placeholder" ]; then
  printf 'source_dev_creds.sh: creds file %s is incomplete.\n' "$_pulp_creds_file" >&2
  [ -n "$_pulp_missing" ] && printf '  missing keys:    %s\n' "${_pulp_missing# }" >&2
  [ -n "$_pulp_placeholder" ] && printf '  placeholder values still present in: %s\n' "${_pulp_placeholder# }" >&2
  printf '  fill in real values from your Apple Developer account, then re-source.\n' >&2
  printf '  schema: templates/secrets/notary.env.example\n' >&2
  printf '  guide:  docs/guides/ios-dev-signing.md\n' >&2
  unset _pulp_creds_file _pulp_required_keys _pulp_missing _pulp_placeholder _k _v
  return 1
fi

# Success — quiet by default. Set PULP_SECRETS_VERBOSE=1 to confirm.
if [ "${PULP_SECRETS_VERBOSE:-0}" = "1" ]; then
  printf 'source_dev_creds.sh: loaded creds from %s\n' "$_pulp_creds_file" >&2
fi

unset _pulp_creds_file _pulp_required_keys _pulp_missing _pulp_placeholder _k _v
return 0
