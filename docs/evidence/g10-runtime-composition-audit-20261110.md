# G-10/A3 runtime composition audit — 2026-11-10

**Disposition: HOLD / NO-CHANGE.** This packet records the strongest bounded
follow-up that can be made from the current public surfaces. It does not add a
graph adapter, change G-10/A3 status, or promote standalone DSP/GPU code to
graph evidence.

## Provenance and commands

| Item | Value |
|---|---|
| Source | `origin/main` at `448057ec9ea569c6b75809251dbd356074c0201a` |
| Worktree | Fresh classified `codex/g10-runtime-granular-20261110` |
| Configure | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=ON` |
| Build | `tools/ci/governed-build.sh cmake --build build --target pulp-test-spectral-morph pulp-test-signal-granular` |
| Build mode | CMake `Release`; governed compile commands contain `-O3 -DNDEBUG` |
| GPU | `PULP_ENABLE_GPU=OFF`; GPU graph/Metal evidence is **NOT CHECKED** |
| Morph binary | `build/test/pulp-test-spectral-morph`, SHA-256 `c225d2c90bb1ca2ab4c3b82d5b425b88be98f82b788898140af6d5389438fb97` |
| Granular binary | `build/test/pulp-test-signal-granular`, SHA-256 `1ad4414fd5faec1465626c19908b9d68b3a42b729637e6491d3a2fe88b60ea0f` |

The exact focused runs were:

```text
./build/test/pulp-test-spectral-morph --reporter compact
RNG seed: 1318621487
All tests passed (78 assertions in 9 test cases)

./build/test/pulp-test-signal-granular --reporter compact
RNG seed: 4220535322
All tests passed (2426 assertions in 28 test cases)
```

The random seeds are reported for reproducibility; the tests use fixed analytic
and deterministic controls where their assertions depend on output values.

## What the current public APIs prove

The CPU frame morph is a real, allocation-free primitive. `SpectralMorphT` is
prepared for a fixed coherent frame geometry and accepts two borrowed complex
frame groups (`core/signal/include/pulp/signal/spectral_morph.hpp:31-106`). The
focused suite covers phase-wrap behavior, magnitude and phase policy oracles,
exact endpoints and aliasing, partition equivalence, non-finite input handling,
invalid lifecycle/configuration, and zero allocation during processing.

The granular engine is also a real bounded standalone primitive. Its public
contract declares a fixed grain pool, live-ring or borrowed-buffer source,
bounded controls, and an allocation-free prepared/process path
(`core/signal/include/pulp/signal/granular.hpp:1-103,150-245`). The focused
suite covers causality bounds, maximum legal live-ring settings, freeze
stability, zero allocations after prepare, parameter clamping, and non-finite
automation recovery.

The GPU surfaces are separate worker/offline primitives. `GpuSpectralMorph`
captures two frames and renders a blend, but its header explicitly says that
render blocks on readback and is not real-time safe
(`core/gpu_audio/include/pulp/gpu_audio/gpu_spectral_morph.hpp:11-42`). The
resident `GpuSpectralStack` has the same boundary: it reads back a synthesized
frame and must run off the audio thread (`core/gpu_audio/include/pulp/gpu_audio/spectral_stack.hpp:98-109,155-185`).
The existing Spectral Lab node correctly drives that path on a transport worker,
but it is an example `GpuAudioNode`, not a `SignalGraph` adapter
(`examples/spectral-lab/gpu_spectral_cloud_node.hpp:3-18,79-160`).

## Why this does not close G-10/A3

The graph's public spectral surface currently exposes only typed analysis,
stage, and synthesis port records (`core/host/include/pulp/host/spectral_graph.hpp:13-33`).
There is no public graph node for `SpectralMorphT`, `GranularEngineT`,
`GpuSpectralMorph`, or `GpuSpectralStack`; there is no immutable analyzed-asset
owner, frame chronology join, granular EOF policy, or graph-owned modulation
rate contract. Connecting these standalone types would require inventing a
second scheduler or an adapter API, which this evidence lane is not authorized
to do.

The current routed graph contract is intentionally narrower: its status and
acceptance documents already identify missing two-asset morph/resynthesis,
granular graph binding, finite tail semantics, notation/catalog authoring,
callback timing/resource matrices, and a no-wait GPU adapter as open. The tests
above strengthen the distinction between “standalone DSP works” and “the graph
can compose it,” but they do not provide the missing graph evidence.

The GPU path has an additional hard boundary. The implementation's own public
contract says readback can block, while the graph callback contract requires a
no-wait path. The existing GPU evidence therefore remains **PARTIAL / NO-CHANGE**;
CPU fallback and worker scheduling must be specified before any graph promotion.

## Required next contract before implementation

An implementation proposal should first define a bounded, installed-consumer
contract for two immutable analyzed assets: canonical geometry and frame
identity, explicit missing-frame/EOF behavior, named morph policies, bounded
granular controls with rate labels, finite `Drain`/`Abort` tails, generation and
seek semantics, and positive plus planted-negative controls. The consumer must
use one prepared graph and one execution plan, prove `m=0`/`m=1` identity and
deterministic output across supported partitions, and record exact source/SDK/
executable hashes. GPU admission additionally requires measured transfer/copy
behavior and callback no-wait fallback; worker readback alone is insufficient.

## Evidence disposition

| Surface | Result | Boundary |
|---|---|---|
| CPU `SpectralMorphT` | 78 assertions / 9 cases pass | no asset ownership, chronology, graph binding, or tail |
| CPU `GranularEngineT` | 2426 assertions / 28 cases pass | no graph node, event-rate contract, EOF, or installed graph consumer |
| GPU morph/stack | NOT CHECKED in this CPU-only configured run | existing headers explicitly mark worker/offline blocking readback |
| G-10 routed graph | existing focused evidence remains valid only for named rows | morph/granular runtime composition and A3 remain open |

The safe disposition is to retain **G-10/A3 PARTIAL/HOLD**. A future lane may
implement the contract above after design ownership is reviewed; this packet
does not authorize that runtime/API work.

## Current-head revalidation — 2026-11-10

This addendum was inspected from the clean classified worktree at
`origin/main=d33f5ea7d8e9f7ef2fd54cd3f90fbe44e0e6eb43`. It is a source-surface
revalidation, not a replay of the historical binaries above. The current tree
still has the same boundaries: the public spectral graph surface exposes only
typed analysis, stage, and synthesis records; `SpectralMorphT` and
`GranularEngineT` remain standalone borrowed/prepared primitives; GPU morph
and stack rendering explicitly allow blocking readback; and generic
`format::Processor` nodes do not adapt any of these types.

The current source therefore supplies no newly authorized graph morph/granular
or GPU runtime seam. The previous focused results remain historical evidence,
and this revalidation does not promote them to current runtime qualification.
G-10/A3 remains **PARTIAL / HOLD** with the same next contract: immutable
analyzed assets, frame chronology and EOF/tail policy, graph-owned rate labels,
positive and planted-negative controls, and a callback-safe GPU
transfer/fallback design before implementation is considered.
