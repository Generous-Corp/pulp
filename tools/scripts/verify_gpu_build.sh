#!/usr/bin/env bash
# verify_gpu_build.sh — assert a Pulp SDK build + downstream consumer are
# Release-mode, Skia-linked, native-bridge (not WebView-fallback), and
# GPU-capable. Exits nonzero on any regression.
#
# Usage:
#   tools/scripts/verify_gpu_build.sh <sdk-build-or-install-dir> [<consumer-binary>]
#
# Examples:
#   tools/scripts/verify_gpu_build.sh build
#   tools/scripts/verify_gpu_build.sh ~/.pulp/sdk/0.91.0-local
#   tools/scripts/verify_gpu_build.sh build ~/Code/spectr/build/Spectr.app/Contents/MacOS/Spectr
#
# The script is read by CI (.github/workflows/build.yml) and by
# pulp standalone test loops; rules below are the contract.

set -euo pipefail

SDK_PATH="${1:-}"
CONSUMER_BIN="${2:-}"

if [[ -z "$SDK_PATH" ]]; then
  echo "usage: $0 <sdk-build-or-install-dir> [<consumer-binary>]" >&2
  exit 2
fi

fail=0
warn=0

red()  { printf '\033[31m%s\033[0m\n' "$*"; }
grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
yel()  { printf '\033[33m%s\033[0m\n' "$*"; }

# --- 1. SDK build/install layout -------------------------------------------
echo "▸ SDK: $SDK_PATH"

# Find CMakeCache.txt (build tree) or report install layout
cache=""
if [[ -f "$SDK_PATH/CMakeCache.txt" ]]; then
  cache="$SDK_PATH/CMakeCache.txt"
elif [[ -d "$SDK_PATH/lib/cmake/Pulp" ]]; then
  # Install tree: read PulpConfig.cmake
  cache=""
else
  red "  FAIL: not a Pulp build or install directory"; fail=1
fi

# --- 2. Build type must be Release (or RelWithDebInfo) ---------------------
if [[ -n "$cache" ]]; then
  bt="$(grep -E '^CMAKE_BUILD_TYPE:STRING=' "$cache" | cut -d= -f2)"
  case "$bt" in
    Release|RelWithDebInfo)
      grn "  ✓ Build type: $bt"
      ;;
    Debug|"")
      red "  FAIL: Build type is '${bt:-(empty)}'; must be Release or RelWithDebInfo for shipped/perf-tested builds"
      red "        (Debug SDK is 50–100× slower and produces user-reported sluggishness — see #1817 lesson)"
      fail=1
      ;;
    *)
      yel "  WARN: Build type is '$bt'; expected Release/RelWithDebInfo"
      warn=1
      ;;
  esac
fi

# --- 3. pulp-view-core static library must contain Skia / Graphite symbols ---
# The library lives at either:
#   <build>/core/view/libpulp-view-core.a   (build tree)
#   <install>/lib/libpulp-view-core.a       (install tree)
pulp_view_core_a=""
for c in \
  "$SDK_PATH/core/view/libpulp-view-core.a" \
  "$SDK_PATH/lib/libpulp-view-core.a"
do
  if [[ -f "$c" ]]; then pulp_view_core_a="$c"; break; fi
done

if [[ -z "$pulp_view_core_a" ]]; then
  red "  FAIL: libpulp-view-core.a not found under $SDK_PATH"
  fail=1
else
  echo "  pulp-view-core: $pulp_view_core_a"
  # Skia presence: look for SkSurface symbols
  if nm "$pulp_view_core_a" 2>/dev/null | grep -qE 'SkSurface'; then
    grn "  ✓ Skia symbols present (SkSurface)"
  else
    red "  FAIL: libpulp-view-core.a has no Skia symbols (SDK shipped without GPU — #1817)"
    fail=1
  fi
  # Graphite presence: look for skgpu::graphite symbols
  if nm "$pulp_view_core_a" 2>/dev/null | grep -qE 'skgpu.*graphite|graphite.*Resource'; then
    grn "  ✓ Graphite symbols present (skgpu::graphite)"
  else
    yel "  WARN: libpulp-view-core.a has no Graphite symbols — GPU path may be unavailable on this lane"
    warn=1
  fi
fi

# --- 4. Consumer binary linkage (optional) ---------------------------------
if [[ -n "$CONSUMER_BIN" ]]; then
  echo "▸ Consumer: $CONSUMER_BIN"
  if [[ ! -f "$CONSUMER_BIN" ]]; then
    red "  FAIL: consumer binary not found"; fail=1
  else
    # arm64 check (we only ship arm64 native bridge for macOS at the moment)
    arch="$(file "$CONSUMER_BIN" | grep -oE 'arm64|x86_64' | head -1)"
    if [[ "$arch" == "arm64" ]]; then
      grn "  ✓ arch: arm64"
    else
      yel "  WARN: arch=$arch — Pulp native bridge tested primarily on arm64"
      warn=1
    fi
    # Skia/Graphite linked through (static link from pulp-view)
    if nm "$CONSUMER_BIN" 2>/dev/null | grep -qE 'graphite|SkGraphic'; then
      grn "  ✓ Graphite linked through into consumer"
    else
      red "  FAIL: consumer does not contain Graphite symbols (would mean WebView/CG-only fallback)"
      fail=1
    fi
    # Native-bridge marker — projects opting into native bridge expose this
    # by defining a NativeEditorView/createNativeEditor symbol. The check
    # is opt-in via PULP_VERIFY_NATIVE_SYMBOL env var so non-bridge consumers
    # don't fail.
    if [[ -n "${PULP_VERIFY_NATIVE_SYMBOL:-}" ]]; then
      if nm "$CONSUMER_BIN" 2>/dev/null | grep -qE "$PULP_VERIFY_NATIVE_SYMBOL"; then
        grn "  ✓ Native-bridge symbol '$PULP_VERIFY_NATIVE_SYMBOL' present"
      else
        red "  FAIL: PULP_VERIFY_NATIVE_SYMBOL='$PULP_VERIFY_NATIVE_SYMBOL' not found in consumer"
        red "        Consumer was built without the native bridge — would fall back to WebView at runtime"
        fail=1
      fi
    fi
  fi
fi

# --- 5. Summary -------------------------------------------------------------
echo
if [[ $fail -ne 0 ]]; then
  red "✗ verify_gpu_build: $fail fatal issue(s), $warn warning(s)"
  exit 1
fi
if [[ $warn -ne 0 ]]; then
  yel "⚠ verify_gpu_build: $warn warning(s) — review above"
fi
grn "✓ verify_gpu_build: SDK is Release + Skia + native, consumer linkage OK"
exit 0
