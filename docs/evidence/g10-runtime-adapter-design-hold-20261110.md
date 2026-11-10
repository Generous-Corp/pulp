# G-10/A3 runtime adapter design hold — 2026-11-10

**Disposition: HOLD / DESIGN REQUIRED.** This packet records the smallest
defensible next contract for runtime composition of spectral morphing,
granular processing, and GPU work. It intentionally adds no graph node, no
second scheduler, and no speculative public API. The current standalone
primitives are useful evidence, but they are not graph adapters.

## Scope and provenance

This review is bound to `origin/main` at
`910501973960060a19d056ec9fb190a283eacf08`. The source tree and public headers
were inspected at that exact revision. The preceding bounded CPU audit is
`docs/evidence/g10-runtime-composition-audit-20261110.md`; this document
refreshes its source binding and turns its findings into an implementation
gate. Existing G-10 status remains **PARTIAL**, and A3 remains unqualified.

The safe conclusion is a design hold: an adapter cannot be implemented against
the currently authorized graph contract without inventing ownership,
chronology, scheduling, tail, and GPU deadline semantics. A future
implementation may proceed only after those semantics have an owner-approved
contract and an installed-consumer acceptance fixture.

## Verified post-roadmap baseline

The following are source-backed facts, not hypotheses:

* `core/host/include/pulp/host/spectral_graph.hpp:13-33` exposes typed records
  for analysis, stage, and synthesis ports. It does not expose morph,
  granular, or GPU graph nodes.
* `core/graph/include/pulp/graph/graph_runtime_plan.hpp:12-23,84-169`
  contains spectral node and connection kinds alongside audio, event,
  automation, and feedback lanes. There is no morph/granular/GPU kind and no
  graph-owned asset, EOF, drain, or callback deadline contract.
* `core/graph/include/pulp/graph/spectral_frame.hpp:48-228` provides a fixed,
  versioned frame contract, identity, the closed `IdentityV1` and `RealMaskV1`
  stages, and stamped frame views. It does not own two analyzed assets or join
  independent frame streams.
* `core/signal/include/pulp/signal/spectral_morph.hpp:6-106` provides a
  prepared, allocation-free CPU morph over two borrowed coherent frame groups.
  It is a DSP primitive, not a graph-owned lifetime or chronology policy.
* `core/signal/include/pulp/signal/granular.hpp:75-103,150-245,543-558`
  provides a prepared bounded grain engine. Its live-ring and interleaved
  overloads have explicit sample-clock behavior, but the graph has no event
  rate, source EOF, finite tail, or generation/seek policy for this engine.
* `core/gpu_audio/include/pulp/gpu_audio/gpu_spectral_morph.hpp:11-42` and
  `core/gpu_audio/include/pulp/gpu_audio/spectral_stack.hpp:98-109,155-185`
  explicitly document blocking readback and a non-real-time-safe execution
  boundary. They cannot be called from the graph callback without a measured
  no-wait fallback.
* `examples/spectral-lab/gpu_spectral_cloud_node.hpp:3-18,79-164` is a useful
  worker-only example. It routes misses to dry passthrough and keeps GPU work
  off the audio thread; it is not a `SignalGraph` adapter or an installed
  consumer.

The existing focused CPU evidence remains valuable and bounded: the morph
fixture passes 78 assertions in 9 cases, and the granular fixture passes 2,426
assertions in 28 cases. Those controls cover deterministic policies,
partitioning, invalid lifecycle, finite recovery, clamping, and zero
allocations after preparation. They do **not** prove graph composition,
asset ownership, chronology, callback timing, or GPU safety. GPU evidence in
the preceding audit was configured OFF and is therefore **NOT CHECKED**.

## Why implementation is held

An adapter would have to answer all of these questions before it could be
correctly scheduled by the existing prepared plan:

1. Who owns the two analyzed inputs and keeps them immutable while a prepared
   graph runs? Borrowing `SpectralMorphT`'s pointers is insufficient because a
   graph plan may outlive an authoring buffer.
2. How are frame stamps, dropped frames, replacement frames, and differing
   source generations joined? The existing spectral frame contract validates a
   frame; it does not define a two-input join or a missing-input result.
3. At what rate do morph controls, grain creation, and source writes arrive?
   The graph has no bounded modulation/event-rate contract for these primitives.
4. What is the finite behavior at source EOF, seek, reset, `Drain`, and
   `Abort`? A live-ring engine can continue to produce grains while a graph
   tail must eventually settle.
5. How does a GPU miss, device loss, or readback delay affect callback output,
   latency, telemetry, and recovery? The worker example's dry fallback is a
   starting policy, not an admitted graph contract.
6. How are all of these resources represented by one prepared graph and one
   execution plan without adding a second scheduler or callback-time
   allocation?

Choosing defaults for these questions inside an adapter would silently create
   a new public architecture. That is outside this bounded evidence lane and
   would make the result impossible to compare with the existing G-10
   acceptance rows.

## Required contract before a runtime PR

The next design review should specify one bounded contract with these fields:

### Immutable assets and frame identity

* Two immutable analyzed-asset handles with explicit lifetime and memory caps.
* Canonical FFT, hop, channel, bin, and sample-rate geometry shared by both
  inputs, plus a frame identity and monotonically checked chronology.
* A defined result for missing, late, duplicate, or mismatched frames. The
  policy must say whether a frame is replaced, muted, held, or fails the branch.
* Generation and seek semantics that cannot mix old and new assets.

### Morph and granular controls

* Named magnitude/phase morph policies and exact endpoint semantics (`m=0`
  and `m=1`) across fixed and irregular partitions.
* Bounded controls with explicit rate labels: block, frame, sample, and event.
  A control that is not admitted at a rate must be refused at graph prepare
  time.
* Grain source and EOF behavior, fixed pool/resource limits, and a finite
  `Drain`/`Abort` tail contract.

### Execution and GPU boundary

* One prepared graph and one execution plan; no adapter-owned scheduler.
* Callback paths that allocate nothing, wait for no worker, and perform no
  readback. GPU work must have a measured transfer budget and a deterministic
  CPU or dry fallback for a missed block or device generation.
* Resource and callback timing receipts covering activation, reset, steady
  state, tail, and recovery, not only a C++ allocation probe.

### Consumer and controls

* A clean installed SDK consumer that connects the public graph API, runs both
  positive and planted-negative controls, and records exact source, SDK,
  executable, and fixture hashes.
* Deterministic endpoint and partition controls, chronology/EOF controls,
  generation/seek controls, callback-resource controls, and GPU miss/device
  loss controls. Every production guard needs a passing baseline, a failing
  mutation, and a restored-green run.

## Acceptance sketch for the eventual implementation

The first implementation PR should be admitted only if it can demonstrate:

| Area | Positive control | Planted-negative control |
| --- | --- | --- |
| Asset identity | Two canonical assets render independently and join by stamp | Remove identity comparison; the consumer must fail closed |
| Endpoints | `m=0` and `m=1` reproduce each source over fixed/irregular blocks | Bypass endpoint branch; oracle detects non-identity |
| Chronology | Missing, duplicate, late, and generation changes follow the declared policy | Remove replacement/duplicate guard; oracle detects wrong frame |
| Granular lifecycle | Bounded pool, source EOF, `Drain`, and `Abort` settle deterministically | Suppress tail cutoff or exceed the pool; control fails |
| Callback safety | Activation/reset/steady/tail paths remain no-wait and allocation-free | Plant allocation, lock, I/O, or readback; resource gate fails |
| GPU recovery | Device loss and missed worker block select the declared fallback | Remove fallback or return stale data; output/telemetry oracle fails |
| Plan ownership | One prepared graph and one runtime plan execute the fixture | Add a second executor or callback allocation; build/control gate fails |

These are acceptance predicates, not authorization to add the implementation in
this packet.

## Explicit non-goals and alternatives rejected

This lane does not add a generic “DSP node” wrapper, a callback-time asset
cache, a second event scheduler, dynamic topology mutation, or a graph-visible
GPU readback API. Existing CPU primitives should remain reusable; duplicating
their policies in a graph adapter would create divergent semantics.

The worker-only GPU design is retained as an implementation alternative for a
future asynchronous node. It is not promoted to callback execution merely
because it has a dry fallback. A separate control/event graph may eventually
be appropriate, but selecting one here would preempt the ownership and rate
decisions that the current graph contract intentionally leaves open.

## Disposition and next safe action

Keep G-10/A3 at **PARTIAL/HOLD**. Do not claim morph, granular, or GPU graph
composition from the standalone test counts or the worker example. The next
safe action is an owner-reviewed contract decision followed by one installed
consumer and one narrowly scoped adapter PR. Until then, this packet is
evidence/design only and introduces no runtime or public API change.

