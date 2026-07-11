#!/usr/bin/env bash
# test_install_platform_detect.sh — pin the deployed installer's OS/arch →
# platform mapping, including Intel Mac support (darwin-x64).
#
# Why this test exists
# --------------------
# Before Intel Mac support landed, tools/install/install.sh mapped an
# x86_64 Mac to the darwin-arm64 tarball "via Rosetta" — physically
# impossible (Rosetta translates x86_64 → arm64, never the reverse), so
# Intel users downloaded an arm64 binary that could not launch. This test
# drives the real installer with a mocked `uname` and asserts each
# supported host resolves to the correct release-artifact platform,
# so the mapping can never silently regress again.
#
# Runtime check (executes install.sh with PULP_PRINT_PLATFORM=1, which
# prints the resolved platform and exits before any network access).
#
# Tag [issue-intel-cli] for coverage attribution.

set -euo pipefail

PULP_SRC="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="$PULP_SRC/tools/install/install.sh"
[[ -f "$INSTALL_SH" ]] || { echo "FAIL: $INSTALL_SH missing"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Mocked `uname`: answers -s (OS) and -m (arch) from env, so the same
# install.sh runs as if on any host without touching the real kernel.
make_uname() {
  local os="$1" arch="$2"
  cat > "$WORK/uname" <<EOF
#!/usr/bin/env bash
case "\$1" in
  -s) echo "$os" ;;
  -m) echo "$arch" ;;
  *)  echo "$os" ;;
esac
EOF
  chmod +x "$WORK/uname"
}

PASS=0
FAIL=0
assert_platform() {
  local label="$1" os="$2" arch="$3" expected="$4"
  make_uname "$os" "$arch"
  local got
  got="$(PATH="$WORK:$PATH" PULP_PRINT_PLATFORM=1 bash "$INSTALL_SH" 2>/dev/null || true)"
  if [[ "$got" == "$expected" ]]; then
    echo "  PASS — $label ($os/$arch → $got)"
    PASS=$((PASS+1))
  else
    echo "  FAIL — $label ($os/$arch): expected '$expected', got '$got'"
    FAIL=$((FAIL+1))
  fi
}

# Apple Silicon
assert_platform "macOS arm64 → darwin-arm64" Darwin arm64  darwin-arm64
assert_platform "macOS aarch64 → darwin-arm64" Darwin aarch64 darwin-arm64
# Intel Mac — the regression this test guards
assert_platform "macOS x86_64 → darwin-x64" Darwin x86_64 darwin-x64
# Linux
assert_platform "Linux x86_64 → linux-x64" Linux x86_64 linux-x64
assert_platform "Linux amd64 → linux-x64"  Linux amd64  linux-x64
assert_platform "Linux aarch64 → linux-arm64" Linux aarch64 linux-arm64
assert_platform "Linux arm64 → linux-arm64"   Linux arm64   linux-arm64

echo ""
echo "Result: $PASS pass, $FAIL fail"
[[ "$FAIL" == "0" ]]
