#!/usr/bin/env bash
# Build the signed, notarized Forge Modular-only PKG from an exact Forge build.
set -euo pipefail

PULP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
FORGE_ROOT=""
BUILD_DIR=""
OUT_DIR=""
VERSION=""
FORGE_REF=""
ARCHITECTURE="$(uname -m)"
APP_ID=""
INSTALLER_ID=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --forge-root) FORGE_ROOT="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --forge-ref) FORGE_REF="$2"; shift 2 ;;
        --architecture) ARCHITECTURE="$2"; shift 2 ;;
        --sign-identity) APP_ID="$2"; shift 2 ;;
        --installer-identity) INSTALLER_ID="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$ARCHITECTURE" in
    arm64|aarch64) ARCHITECTURE="arm64" ;;
    x86_64|amd64) ARCHITECTURE="x86_64" ;;
    *) echo "unsupported release architecture: $ARCHITECTURE" >&2; exit 2 ;;
esac
[[ -n "$FORGE_ROOT" && -n "$BUILD_DIR" && -n "$OUT_DIR" && \
   -n "$VERSION" && -n "$FORGE_REF" ]] || {
    echo "usage: release-package.sh --forge-root DIR --forge-ref SHA --build-dir DIR --out DIR --version X.Y.Z [--architecture arm64|x86_64]" >&2
    exit 2
}
FORGE_ROOT="$(cd "$FORGE_ROOT" 2>/dev/null && pwd -P)" || {
    echo "cannot resolve --forge-root" >&2; exit 2; }
BUILD_DIR="$(cd "$BUILD_DIR" 2>/dev/null && pwd -P)" || {
    echo "cannot resolve --build-dir" >&2; exit 2; }

PULP_REF_FILE="$FORGE_ROOT/PULP_SDK_REF"
[[ -f "$PULP_REF_FILE" ]] || { echo "Forge PULP_SDK_REF is missing" >&2; exit 2; }
PULP_REF="$(tr -d '[:space:]' < "$PULP_REF_FILE")"
[[ "$PULP_REF" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Forge PULP_SDK_REF is not an exact lowercase Git SHA" >&2; exit 2; }
[[ "$(git -C "$PULP_ROOT" rev-parse HEAD)" == "$PULP_REF" ]] || {
    echo "Pulp release checkout does not match Forge PULP_SDK_REF" >&2; exit 2; }
[[ -z "$(git -C "$PULP_ROOT" status --porcelain --untracked-files=all)" ]] || {
    echo "Pulp release checkout must be completely clean" >&2; exit 2; }
if git -C "$PULP_ROOT" symbolic-ref -q HEAD >/dev/null 2>&1; then
    echo "Pulp release checkout must be detached at Forge PULP_SDK_REF" >&2
    exit 2
fi

VALIDATOR="$FORGE_ROOT/tools/validate-release-sdk.sh"
[[ -x "$VALIDATOR" ]] || { echo "Forge release SDK validator is missing" >&2; exit 2; }
"$VALIDATOR" "$BUILD_DIR" "$PULP_REF_FILE" "$PULP_ROOT"

INPUTS="$PULP_ROOT/examples/forge-modular/release_inputs.py"
SOURCE_REPORT="$("$INPUTS" source --forge-root "$FORGE_ROOT" --forge-ref "$FORGE_REF" \
    --build-dir "$BUILD_DIR" --version "$VERSION" --pulp-ref "$PULP_REF" \
    --architecture "$ARCHITECTURE")"
echo "$SOURCE_REPORT"
SOURCE_SNAPSHOT="$(python3 -c \
    'import json,sys; print(json.load(sys.stdin)["source_snapshot_sha256"])' \
    <<< "$SOURCE_REPORT")"

AU="$BUILD_DIR/AU/Forge Modular.component"
VST3="$BUILD_DIR/VST3/Forge Modular.vst3"
CLAP="$BUILD_DIR/CLAP/Forge Modular.clap"
APP="$BUILD_DIR/modular/Forge Modular.app"
ARCH_CHECK="${FORGE_MODULAR_ARCH_CHECKER:-$PULP_ROOT/tools/scripts/check_bundle_architectures.py}"
for artifact in "$AU" "$VST3" "$CLAP" "$APP"; do
    "$ARCH_CHECK" "$artifact" --archs "$ARCHITECTURE" --strict --no-verify-signature
done
"$ARCH_CHECK" "$APP/Contents/Resources/build/shape_text" \
    --archs "$ARCHITECTURE" --strict --no-verify-signature
"$ARCH_CHECK" "$APP/Contents/Resources/build/rack_patch_decode" \
    --archs "$ARCHITECTURE" --strict --no-verify-signature

if [[ -f "$HOME/.config/pulp/secrets/keychain.env" ]]; then
    # shellcheck disable=SC1091
    source "$HOME/.config/pulp/secrets/keychain.env"
fi
APP_ID="${APP_ID:-${PULP_SIGN_IDENTITY_HASH:-}}"
INSTALLER_ID="${INSTALLER_ID:-${PULP_SIGN_INSTALLER_HASH:-}}"
[[ -n "$APP_ID" && -n "$INSTALLER_ID" ]] || {
    echo "Developer ID Application and Installer identities are required; run 'pulp ship doctor'" >&2
    exit 2
}

RECIPE="$PULP_ROOT/tools/scripts/build_combined_installer.sh"
[[ -x "$RECIPE" ]] || { echo "canonical combined-installer recipe is missing" >&2; exit 2; }
ARGS=(--name "Forge Modular" --version "$VERSION" --out "$OUT_DIR"
      --architectures "$ARCHITECTURE"
      --sign-identity "$APP_ID" --installer-identity "$INSTALLER_ID"
      --app-for "Forge Modular" "Forge Modular" "$APP"
      --plugin au "$AU" --plugin vst3 "$VST3" --plugin clap "$CLAP")
echo "Forge Modular $VERSION ($FORGE_REF, Pulp $PULP_REF) -> $OUT_DIR"
echo "  exactly 1 AU, 1 VST3, 1 CLAP, and 1 standalone; notarization is mandatory"
"$RECIPE" "${ARGS[@]}"

PKG="$OUT_DIR/Forge Modular-$VERSION.pkg"
[[ -f "$PKG" ]] || { echo "canonical recipe did not produce $PKG" >&2; exit 1; }
EXPANDED="$(mktemp -d)"
trap 'rm -rf "$EXPANDED"' EXIT
PKGUTIL="${FORGE_MODULAR_PKGUTIL:-/usr/sbin/pkgutil}"
"$PKGUTIL" --expand-full "$PKG" "$EXPANDED/package"
"$INPUTS" package --expanded-root "$EXPANDED/package" \
    --forge-ref "$FORGE_REF" --version "$VERSION" --pulp-ref "$PULP_REF" \
    --architecture "$ARCHITECTURE" --source-snapshot "$SOURCE_SNAPSHOT"
echo "verified signed/notarized Modular-only package: $PKG"
