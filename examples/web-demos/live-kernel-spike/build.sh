#!/usr/bin/env bash
# Pulp Live Kernel — S0 spike — build the two standalone wasm modules.
#
# Produces (into ./dist):
#   lk_kernel.wasm — the resident graph interpreter (lk_* ABI)
#   aot_twin.wasm  — the hand-fused AOT baseline / null-test oracle (aot_* ABI)
#
# Both are STANDALONE_WASM: raw .wasm with only wasi_snapshot_preview1 imports and
# an internal (exported) memory, so the worklet can transfer the BYTES in and
# compile them SYNCHRONOUSLY with `new WebAssembly.Module(bytes)` — never posting
# a WebAssembly.Module (silently dropped in Chrome, DECISION.md §3). No JS glue,
# no ES-module factory, no SINGLE_FILE base64.
#
# Requires emsdk 6.0.2 active (source ~/Code/emsdk/emsdk_env.sh).
set -euo pipefail

SPIKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SPIKE_DIR/../../.." && pwd)"
KERNEL_DIR="$REPO_ROOT/experimental/live_kernel"
SIGNAL_INC="$REPO_ROOT/core/signal/include"
DIST="$SPIKE_DIR/dist"
mkdir -p "$DIST"

if ! command -v emcc >/dev/null 2>&1; then
  echo "emcc not found — run: source ~/Code/emsdk/emsdk_env.sh" >&2
  exit 1
fi
echo "emcc: $(emcc --version | head -1)"

COMMON=( -std=c++20 -O3 -fno-exceptions -fno-rtti
         -I"$SIGNAL_INC" -I"$KERNEL_DIR"
         -sSTANDALONE_WASM=1 -sPURE_WASI=0
         -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=0
         -sTOTAL_STACK=1048576
         --no-entry )

echo "building lk_kernel.wasm ..."
emcc "${COMMON[@]}" \
  -sEXPORTED_FUNCTIONS="['_lk_init','_lk_load_plan','_lk_swap','_lk_set_param','_lk_process','_lk_is_fading','_lk_active_valid','_lk_sample_rate','_lk_alloc_count','_lk_node_levels','_lk_set_meter','_f2_tanhf','_f2_ladder_tanhf','_f2_sinf','_f2_cosf','_f2_expf','_f2_tanf','_f2_powf','_f2_fmodf','_malloc','_free']" \
  "$KERNEL_DIR/lk_entry.cpp" \
  -o "$DIST/lk_kernel.wasm"

echo "building aot_twin.wasm ..."
emcc "${COMMON[@]}" \
  -sEXPORTED_FUNCTIONS="['_aot_init','_aot_process','_aot_chain_setup','_aot_chain_setparam','_aot_chain_process','_malloc','_free']" \
  "$KERNEL_DIR/aot_twin.cpp" \
  -o "$DIST/aot_twin.wasm"

echo "done:"
ls -la "$DIST"/*.wasm
