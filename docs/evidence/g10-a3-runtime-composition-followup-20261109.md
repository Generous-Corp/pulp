# G-10/A3 runtime composition follow-up

**Disposition: HOLD / NOT CHECKED.** This packet records the smallest evidence
boundary for a possible two-asset spectral morph/resynthesis workflow with
bounded granular controls. It does not change G-10 or A3 status and it authorizes
no source implementation.

**Source authority:** Pulp `origin/main` at
`a3df0dc6e3dca64b601e67f0cd30e3d0dfd51f67` (2026-11-09). The current checkout
was clean at that head before this documentation-only change. The companion
planning contract is `G10-A3-FOLLOWUP-2026-11-09` at planning commit
`e3c198662b3cf914ac18ec38a2e421a01a839703` on the planning follow-up branch;
that contract is design input, not an admission.

## Current evidence boundary

The existing G-10 status remains **PARTIAL**. The routed implementation and
focused controls cover a versioned frame contract, closed identity/mask stages,
partition controls, bounded synthesis failure behavior, and output-clock
protection. The merged row-16 receipt is separately bounded to output-clock,
fatal-prefix, zero-output, and callback-allocation controls; it is not a full
G-10 or A3 receipt.

The source audit on this head establishes these facts:

| Surface | Current evidence | Boundary that remains open |
|---|---|---|
| Audio graph adapter | `format::SpectralFrameProcessor` is a `format::Processor` hosting `SpectralFrameEngineT<float>` (`core/format/include/pulp/format/spectral_frame_processor.hpp:61-222`). | It is one audio node with borrowed frame views and equal-hop WOLA. There is no public two-asset graph adapter, asset schema, or frame chronology join. |
| Frame math | `SpectralFrameEngineT` reports fixed `fft_size + analysis_hop` latency (`core/signal/include/pulp/signal/spectral_frame_engine.hpp:225-299`). | Alternate synthesis hops, analyzed-asset loading, cross-frame phase policy, and end-of-stream composition are not admitted. |
| Morph | `SpectralMorphT` operates on two coherent complex frames (`core/signal/include/pulp/signal/spectral_morph.hpp:3-7,32-94`). | The standalone type does not define asset ownership, chronology, missing-frame behavior, persistence, or graph binding. |
| Granular | `GranularEngineT` is a bounded standalone grain engine with no dynamic voice allocation and zero reported codec latency (`core/signal/include/pulp/signal/granular.hpp:86-150,455`). | Graph binding, prepared asset lifetime, control/event rates, sample-indexed modulation, and EOF behavior remain unqualified. |
| Graph latency | `SignalGraph` accepts `format::Processor` and `ProcessorNodeInstance` (`core/host/include/pulp/host/signal_graph_runtime.hpp:262-264`) and records node/PDC latency. | Existing latency metadata does not prove spectral resynthesis tail length, granular lookahead, or a two-asset adapter contract. |
| A3 status | The flagship readiness audit keeps A3 **PARTIAL** because notation, analyzed two-asset morph/resynthesis, live modulation/time-index, and the callback matrix remain open (`docs/evidence/g12-flagship-readiness-audit-20261106.md:32`). | No status upgrade is justified by the existing routed receipts. |
| GPU path | The resident spectral-chain evidence is **PARTIAL / NO CHANGE**; no `SignalGraph` GPU adapter is present and `GpuSpectralStack::render()` is blocking (`docs/evidence/advanced-graph-gpu-resident-spectral-chain-2026-11-06.md:3,55-56`). | The first follow-up remains CPU and no-wait; GPU promotion is not inferred. |

## Smallest contract before implementation

A future implementation proposal must first demonstrate a bounded A3-MVP
fixture with one prepared graph and one serial execution plan:

1. Two immutable analyzed assets share one canonical profile: sample rate, FFT
   size, equal analysis/synthesis hop, window, channel-group shape, positive-bin
   count, frame sequence/origin, finite bins, and explicit end marker. Geometry
   mismatch is rejected before activation; no callback decode, file I/O, or
   implicit resampling is allowed.
2. Morph is a deterministic `m` in `[0,1]` joining corresponding frames, with
   named magnitude and phase policies and an explicit missing-frame rule. The
   policies are part of semantic identity; hidden crossfades and phase resets
   are not acceptable evidence.
3. Granular controls are bounded `position`, `density`, `size`, `jitter`, and
   `spread` fields with declared rate labels. The first fixture may admit
   control-rate or two-endpoint block interpolation; graph-owned multirate and
   arbitrary per-sample modulation stay out of scope.
4. Resynthesis reports fixed analysis latency plus any declared lookahead. End
   of stream is explicit: `Drain` reaches `Ended` after a finite declared tail,
   while `Abort` produces bounded silence and `Aborted`. Infinite tails and
   hidden lookahead are refusals.
5. Reset, seek, and prepared replacement start a new generation and sequence.
   Late/future/overflow controls, malformed frames, missing assets, and failed
   stages have named outcomes; pending mailbox data is preserved or refused,
   never silently dropped or migrated.
6. The public acceptance target is a clean installed-SDK consumer using only
   public headers. It must reproduce deterministic output across 32/64/128
   partitions, prove `m=0`/`m=1` asset identity, record asset/control/event and
   output hashes, and carry positive plus planted-negative controls.

## Evidence disposition

### Existing bounded positives (carried forward)

- Routed G-10 controls and the row-16 output-clock receipt remain valid only for
  their named predicates and source heads; they do not establish this A3-MVP.
- `SpectralMorphT`, `GranularEngineT`, and `SpectralFrameEngineT` are real public
  signal surfaces, but source presence is not graph composability evidence.

### Required planted negatives (not checked in this packet)

The following controls must be added to any admitted fixture and must fail for
the named reason before passing after restoration:

- mismatched asset geometry, frame order/origin, non-finite bins, missing frames,
  unequal lengths without a declared policy, and EOF without a finite tail;
- out-of-range/NaN/Inf morph or granular controls, unsupported rate labels,
  late/future events, and bounded queue overflow;
- arbitrary stage registration, callback allocation/lock/wait/syscall/file I/O,
  implicit resampling, hidden lookahead, and a second scheduler;
- wrong asset, event order, notation, replay, or one-sample-latency mutations in
  the installed consumer.

These are **NOT CHECKED** here. No runtime/API change, generated fixture, or
speculative adapter is justified until the contract and owner boundaries are
reviewed.

## Gates and next action

- Governed build/test: **N/A** for this documentation-only HOLD packet.
- Dedicated-builder build/source-selftest: **NOT CHECKED**.
- Diff coverage: **NOT CHECKED**.
- Realtime callback matrix, clean installed consumer, notation/replay, and
  cross-platform receipts: **NOT CHECKED**.

The next safe action is a design review of the A3-MVP contract, followed by a
fixture-only model with immutable assets and refusal receipts. If two focused
public-consumer attempts cannot express real value without a second scheduler,
open-ended tail, callback blocking/allocation, or ambiguous phase semantics,
record **NO CHANGE** and retain the current PARTIAL/HOLD status.

This packet is subordinate to `docs/graph-program-status/G-10.md` and
`docs/graph-program-status/G-10-routed-acceptance.md`; it does not duplicate or
reopen the merged routed spectral work.
