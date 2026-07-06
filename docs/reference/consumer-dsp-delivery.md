# Consumer DSP delivery policy

How a *shipped* Pulp plugin may accept new DSP from downloadable content, and —
just as importantly — how it may NOT. (Live-swap plan item 3.4 / decision D4.)
This is a security + portability contract, not a roadmap: each rule below is
enforced by code cited inline.

## The rule, one line
**Consumer DSP is data-driven graphs today; novel kernels arrive as sandboxed
WASM (Phase 4); native code packs are deferred behind a trust design; a shipping
plugin never `dlopen`s an arbitrary downloaded dylib.**

## 1. Consumer DSP = data-driven signal graphs (now)
A content pack delivers DSP as a **`.pulpgraph`** — a description of nodes +
connections the plugin's own `SignalGraph` compiles and runs. No third-party
machine code executes: the graph is data, interpreted by first-party code.

- **Universal + iOS-safe.** No loader entanglement, no Apple code-signing/JIT
  restriction — the same graph runs on every platform, so Wave-1 consumer
  hot-swap ships everywhere including iOS.
- **Measured, not assumed, performance** (derisk P0.4): the graph path carries no
  perf penalty worth a native-kernel escape hatch for the node vocabulary that
  exists. Engine: `core/host/` `SignalGraph`.
- The graph swaps through the same Phase-2 machinery as everything else — a
  verified pack's graph becomes a `SwapUnit` handed to `apply_live_swap`
  (`core/format/include/pulp/format/reload/live_swap_transaction.hpp`).

## 2. Novel kernels → sandboxed WASM (Phase 4), never native
When a pack needs DSP the graph vocabulary can't express (a genuinely new
kernel), the answer is a **WASM** module, not native code. The swap-pack format
already **reserves** the `wasm-dsp` kind for exactly this
(`SwapPackKind::WasmDsp`, `core/format/include/pulp/format/reload/swap_pack.hpp`);
the runtime + RT execution contract are Phase 4. A WASM kernel is a
node-pack-shaped citizen so it swaps via the same transaction.

## 3. Native node packs are DEFERRED behind a trust design
Native node packs (real machine code) are NOT a consumer path today. The signed
`NodePackManifest` scheme exists
(`core/host/include/pulp/host/node_pack.hpp` — per-file SHA-256 + Ed25519 +
trusted-signer + fail-closed load), but pulling *third-party* native packs onto a
user's machine is gated on an unresolved trust decision, recorded in the plan's
Open items: **Team-ID re-sign** (a curator signs each pack — key management +
revocation duties) **vs library-validation-off** (a weakened OS security
posture). Until that is decided, native packs are first-party/curated only.

## 4. A shipping plugin NEVER dlopens a downloaded dylib
The developer hot-reload lane (`ReloadableShell`) has a filesystem watcher that
`dlopen`s a rebuilt logic library — a *development* affordance. It is **compiled
out of shipping builds** by the ship-safety gate (item 1.12):
`PULP_RELOAD_DEV_WATCHER=0` removes the watcher thread, its poll loop, and the
raw-path `ReloadController`
(`core/format/include/pulp/format/reload/reloadable_shell.hpp`), enforced by the
`nm` symbol-absence test (`pulp-reload-ship-symbol-absence`,
`test/cmake/reload_ship_symbol_check.cmake`). So a shipped binary has no
filesystem-watch / arbitrary-`dlopen` entry point at all. The only sanctioned
"is this content trustworthy?" path is `verify_swap_pack` (signature-then-hash,
fail-closed).

## Why this shape
Data-graphs + WASM are portable and sandboxable; native code is neither without a
signing/trust apparatus Pulp has deliberately not committed to. The policy keeps
the consumer surface **portable (iOS included), sandboxed, and fail-closed** —
and keeps the dangerous developer affordance out of the shipped artifact.
