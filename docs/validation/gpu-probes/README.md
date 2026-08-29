# A2 GPU-probe structural evidence

`tools/scripts/gpu_probe_acceptance.py` is the A2 exact-head structural-evidence
recorder. It refreshes
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
regular evidence files, and publishes `receipt.json` last. The durable receipt
is structural evidence, not a self-authenticating terminal certificate. The
recorder rechecks the exact live `HEAD`, every retained descriptor/inode/digest,
the published hard-link identities, and the structural verifier while those
claims remain live, then emits only `result: structural-evidence-written` after
closing them. No importable Python function, stdout string, saved marker, or
offline verifier result from this repository constitutes terminal authority.

The recorder must run only after the final integration SHA is fixed. It does
not create or promote a receipt during an implementation turn.

## Prerequisites

- A fresh, classified, completely clean Pulp worktree at the final integration
  SHA, with the accepted plan object available in its `planning` checkout.
- The pinned macOS Skia/Dawn GPU build and milestone-matched sealed V8 release
  inputs, each with the asset stamp written by Pulp's canonical fetch tool. Do
  not mix headers and libraries from different provider generations.
- A dedicated Pulp build directory and a not-yet-created install-prefix path
  outside the worktree. The recorder atomically claims the empty prefix before
  installing by no-replace renaming an unpredictable staging directory and
  binds its no-follow created-inode identity through the final proof. Before
  reconfiguration it retains every tracked Pulp regular-file/symlink inode and
  expected Git blob. It then reconfigures the external build, retains the
  regenerated CMake/Ninja/configure-time graph, resolves and retains the
  complete selected Skia/Dawn and V8 provider trees (including both GPU
  archives, V8 headers, and the V8 runtime). When FindSkia activates the
  adjacent `SKIA_DIR/../skia-src` include layout, that complete source-provider
  tree is retained too. Each binary provider must be the exact CMake/Ninja
  consumed package root, match a fixed supported directory layout, carry a
  release-asset generation stamp pinned by `tools/deps/manifest.json`, and
  contain no unrelated top-level organization/monorepo data. Escaping provider
  symlinks fail closed.
  The recorder then forces a CMake clean, removes
  the Rust Cargo target cache, and requires all three measured outputs absent
  before rebuilding. Those exact source and build-input descriptors remain
  live through install, all CLI/MCP recipes, Forge proof, self-verification,
  and publication. Immediately after the exact-head build it retains the CMake
  cache and all three build-output inodes before installation, then retains the installed
  build stamp, CLI, delegate, MCP, and Forge executable inodes through final
  publication. Every claim is rehashed and sealed with macOS vnode mutation
  monitoring across its complete path-ancestor chain, so a replace-and-restore
  race is still detected.
  Receipt publication likewise
  uses retained directory descriptors,
  source/destination inode comparisons, relative no-replace links, and a
  receipt-last identity and final byte check, so neither path, staged-file, or
  in-place content substitution nor stale SDK files can be accepted.
- A fresh Forge worktree detached at
  `0750a88dea3af7fca927a8c02887e071109407ae`. The only allowed difference is
  the plan-required `PULP_SDK_REF` overlay containing the final Pulp SHA; it
  must be unstaged, with no untracked or other modified files. The recorder
  retains the complete tracked Forge tree with that one exact overlay, rejects
  Git links, retains every file in the installed Pulp SDK before Forge
  configure, atomically claims a fresh Forge build inode, parses the retained
  CMake cache descriptor, retains the regenerated Forge CMake/Ninja input graph,
  and retains the build-info stamp plus every regular
  file/symlink in the completed app bundle through codesign, screenshot, and
  Forge-cwd doctor execution.
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
  -DV8_DIR=/absolute/path/to/pinned/v8-build \
  -DPULP_BUILD_RUST_CLI=ON \
  -DPULP_RUST_CLI_PROFILE=release
cmake --build "$PULP_BUILD_DIR" --target pulp-rust-cli pulp-cli pulp-mcp --parallel

git worktree add --detach "$FORGE_ROOT" \
  0750a88dea3af7fca927a8c02887e071109407ae
printf '%s\n' "$PULP_FINAL_SHA" > "$FORGE_ROOT/PULP_SDK_REF"
```

The V8 provider must be one of the platform roots materialized by
`tools/scripts/fetch_v8_for_release.py`, including its `.v8-asset-sha256`
stamp. Pulp configure must leave `PULP_HAS_THREEJS=TRUE`. Do not pre-create `$PULP_PREFIX` or
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

The receipt directory contains 12 CLI JSON results, a nine-response MCP JSONL
transcript, the Forge Modular PNG, Forge-cwd doctor JSON, and `receipt.json`.
The standalone verifier checks structural integrity only. It rejects missing
recipe parity, direct C++ substitutions for the
Rust front, stale source blobs, forged plan/build stamps, non-authentic hardware,
blank screenshots, missing negative controls, Forge drift, and non-passing
structural fields, but it cannot prove that a saved directory came from a live
recorder. `--terminal` therefore fails closed; rerunning this recorder still
does not create terminal authority, and no JSON marker or recomputed hash can
upgrade an offline directory. Final A2 acceptance requires a separate protected
cross-system acceptance package that independently binds the recorder run,
retained receipt bytes, and exact protected integration authority. That package
is not implemented or minted by these local scripts.

Publication never replaces an output directory that appears during the run.
Before any build or recipe execution the recorder retains the exact existing
output-parent inode; it keeps that descriptor and its vnode mutation monitor
live through self-verification and publication. The recorder claims the final
directory with a no-replace `mkdir`, publishes regular files by no-replace hard
links, fsyncs them, and links `receipt.json` only after the other evidence is
durable. A raced parent/destination or partial interrupted directory is
nonterminal and must not be promoted. A copied or hand-built directory may pass
structural checks when its internal bytes are coherent; the same is true of a
fresh recorder-produced directory. Both remain structural inputs to the
separate cross-system acceptance package.

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
