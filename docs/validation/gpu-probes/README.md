# Final A2 GPU-probe acceptance

`tools/scripts/gpu_probe_acceptance.py` is the terminal A2 recorder. It refreshes
one exact-head Release Pulp build/install, then runs all four canonical recipes
twice plus their seeded negative controls through installed Rust `pulp`. It
replays every positive and negative through installed `pulp-mcp`, rebuilds the
exact Forge Modular standalone against that same SDK, captures its real GPU
shell, and runs the installed GPU doctor from the Forge checkout. Forge Modular
is the Forge downstream proof. The plan separately permits maintained Pulp
examples for paths that Forge Modular does not exercise, so the executed STFT
and Three.js rows are recorded as additional Pulp path canaries rather than
Forge evidence. The recorder writes to a new directory outside all checkouts,
self-verifies, atomically claims the destination without replacement, links all
regular evidence files, and publishes `receipt.json` last as the terminal
marker.

The recorder must run only after the final integration SHA is fixed. It does
not create or promote a receipt during an implementation turn.

## Prerequisites

- A fresh, classified, completely clean Pulp worktree at the final integration
  SHA, with the accepted plan object available in its `planning` checkout.
- The pinned macOS Skia/Dawn GPU build and Homebrew `node@24` V8 inputs. Do not
  mix headers and libraries from different Skia builds.
- A dedicated Pulp build directory and a not-yet-created install-prefix path
  outside the worktree. The recorder atomically claims the empty prefix before
  installing and binds its no-follow device/inode identity through the final
  proof. It also retains and rehashes the exact installed CLI, delegate, MCP,
  and Forge executable inodes around every launch. Receipt publication likewise
  uses retained directory descriptors,
  source/destination inode comparisons, relative no-replace links, and a
  receipt-last identity check, so neither path or staged-file substitution nor
  stale SDK files can be accepted.
- A fresh Forge worktree detached at
  `0750a88dea3af7fca927a8c02887e071109407ae`. The only allowed difference is
  the plan-required `PULP_SDK_REF` overlay containing the final Pulp SHA; it
  must be unstaged, with no untracked or other modified files.
- A not-yet-created Forge build path outside every checkout. The recorder
  configures its single-config Release build only after the fresh Pulp install
  exists. Pulp build, install, Forge build, and output paths must be mutually
  non-overlapping.

Example preparation (replace the three dependency paths with the host's
verified pinned inputs):

```bash
export PULP_FINAL_SHA="$(git rev-parse HEAD)"
export PULP_BUILD_DIR=/absolute/outside/path/pulp-a2-release
export PULP_PREFIX=/absolute/outside/path/pulp-a2-prefix
export FORGE_ROOT=/absolute/path/forge-a2-proof
export FORGE_BUILD_DIR=/absolute/outside/path/forge-a2-release

cmake -S . -B "$PULP_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PULP_PREFIX" \
  -DSKIA_DIR=/absolute/path/to/pinned/skia-build \
  -DPULP_ENABLE_GPU=ON \
  -DPULP_ENABLE_SCENE3D=ON \
  -DPULP_ENABLE_THREEJS_RUNTIME=ON \
  -DPULP_ENABLE_JS=ON \
  -DPULP_JS_ENGINE=v8 \
  -DV8_INCLUDE_DIR=/opt/homebrew/opt/node@24/include/node \
  -DV8_LIB_DIR=/opt/homebrew/opt/node@24/lib \
  -DV8_LIBRARY_PATH=/opt/homebrew/opt/node@24/lib/libnode.137.dylib \
  -DPULP_BUILD_RUST_CLI=ON \
  -DPULP_RUST_CLI_PROFILE=release
cmake --build "$PULP_BUILD_DIR" --target pulp-rust-cli pulp-cli pulp-mcp --parallel

git worktree add --detach "$FORGE_ROOT" \
  0750a88dea3af7fca927a8c02887e071109407ae
printf '%s\n' "$PULP_FINAL_SHA" > "$FORGE_ROOT/PULP_SDK_REF"
```

The exact V8 dylib filename is part of the local node@24 installation; confirm
it rather than copying the example blindly. Pulp configure must leave
`PULP_HAS_THREEJS=TRUE`. Do not pre-create `$PULP_PREFIX` or
`$FORGE_BUILD_DIR`; the recorder claims the prefix, installs Pulp, then
configures Forge with `Pulp_DIR` resolved to
`$PULP_PREFIX/lib/cmake/Pulp`.

## Record and verify

Use the immutable accepted plan identities and a new output path outside every
checkout:

```bash
PLAN_REVISION=641649b7e7fece6baae34380b6e719904506af22
PLAN_SHA256=00bdb8bd55fb90fb42d98a09442d2b168505a23a4208cb5b9edb67b01de69f07
A2_RECEIPT=/tmp/m3-a2-final-$PULP_FINAL_SHA

python3 tools/scripts/gpu_probe_acceptance.py \
  --repository "$PWD" \
  --build-dir "$PULP_BUILD_DIR" \
  --install-prefix "$PULP_PREFIX" \
  --planning-repository "$PWD/planning" \
  --plan-revision "$PLAN_REVISION" \
  --plan-sha256 "$PLAN_SHA256" \
  --forge-repository "$FORGE_ROOT" \
  --forge-build-dir "$FORGE_BUILD_DIR" \
  --output-dir "$A2_RECEIPT"

python3 tools/scripts/verify_gpu_probe_acceptance.py "$A2_RECEIPT"
```

The terminal directory contains 12 CLI JSON results, a nine-response MCP JSONL
transcript, the Forge Modular PNG, Forge-cwd doctor JSON, and `receipt.json`.
The verifier rejects missing recipe parity, direct C++ substitutions for the
Rust front, stale source blobs, forged plan/build stamps, non-authentic hardware,
blank screenshots, missing negative controls, Forge drift, and non-passing
terminal fields.

Publication never replaces an output directory that appears during the run.
The recorder claims the directory with a no-replace `mkdir`, publishes regular
files by no-replace hard links, fsyncs them, and links `receipt.json` only after
the other evidence is durable. A raced destination or partial interrupted
directory is nonterminal and must not be promoted.

Expected recorder runtime is roughly 10–30 minutes with warm build caches and
45–90 minutes from cold dependencies. The bounded subprocess timeouts are one
hour each for Pulp and Forge builds, five minutes per GPU operation, and 30
minutes for install. A missing pinned Skia/V8 input, `PULP_HAS_THREEJS!=TRUE`,
non-Release cache, dirty Pulp checkout, Forge drift beyond `PULP_SDK_REF`,
wrong installed build stamp, or unavailable authentic Metal adapter is a hard
blocker—not a skippable result.

After preserving the receipt, restore or remove the detached Forge proof
worktree through the normal worktree protocol. The one-file overlay is not a
Forge change to land.
