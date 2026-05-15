#!/usr/bin/env bash
# test_install_sh_runs_sdk_install.sh — pin install.sh's CLI+SDK contract
# (pulp #2087, slice 1).
#
# Why this test exists
# --------------------
# The original install.sh installed only the CLI binary, leaving the SDK
# at whatever version had been installed previously (often years out of
# date or absent entirely). Real users then ran `pulp build` against an
# ancient SDK, hit cryptic errors, and never realized CLI vs SDK were
# separate things.
#
# This test pins the contract that install.sh now ALSO installs the SDK
# unless explicitly opted out, AND that the closing message tells the
# user how to manage SDKs going forward (upgrade, install, project bump).
#
# Static checks only — does not actually curl install or download
# anything. Looks at install.sh source for the right invocations and
# strings.
#
# Tag [issue-2087] for coverage attribution.

set -euo pipefail

PULP_SRC="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="$PULP_SRC/tools/install/install.sh"
[[ -f "$INSTALL_SH" ]] || { echo "FAIL: $INSTALL_SH missing"; exit 2; }

PASS=0
FAIL=0
assert_grep() {
  local label="$1" pattern="$2"
  if grep -qE "$pattern" "$INSTALL_SH"; then
    echo "  PASS — $label"
    PASS=$((PASS+1))
  else
    echo "  FAIL — $label (pattern: $pattern)"
    FAIL=$((FAIL+1))
  fi
}

# Contract 1: install.sh runs `pulp sdk install` after CLI extraction
assert_grep \
  "install.sh runs pulp sdk install" \
  '"\$INSTALL_DIR/pulp" sdk install'

# Contract 2: opt-out env var documented + honored
assert_grep \
  "PULP_SKIP_SDK_INSTALL opt-out is honored" \
  'PULP_SKIP_SDK_INSTALL'

# Contract 3: closing message mentions matching SDK
assert_grep \
  "closing message says 'CLI + matching SDK'" \
  '(CLI \+ matching SDK|CLI \+ SDK)'

# Contract 4: closing message tells user about pulp sdk status
assert_grep \
  "closing message documents pulp sdk status" \
  'pulp sdk status'

# Contract 5: closing message documents pulp project bump for pinning
assert_grep \
  "closing message documents pulp project bump (pinning path)" \
  'pulp project bump'

# Contract 6: closing message documents pulp upgrade
assert_grep \
  "closing message documents pulp upgrade" \
  'pulp upgrade'

# Contract 7: header comment still says CLI installer (sanity — file
# wasn't truncated/regressed)
assert_grep \
  "header comment still describes the installer" \
  '^#.*Pulp CLI installer'

echo ""
echo "Result: $PASS pass, $FAIL fail"
[[ "$FAIL" == "0" ]]
