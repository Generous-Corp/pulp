# Pulp audio graph after the current roadmap: current-main refresh

**Status:** research and planning only. This document does not change the graph
roadmap and does not authorize runtime or API implementation.

**Review date:** 2026-11-11

**Verified source head:** `3633377fdd47c086850a22ef8d87130cd17f4e6d`
(`origin/main` in the clean worktree used for this review).

**Prior context:** `docs/reference/2026-11-10-post-roadmap-audio-graph-opportunities.md`
is the broader proposal. This refresh records what changed on current `main`
and narrows the next decision to the smallest defensible experiments. It does
not relabel an in-flight roadmap slice as a post-roadmap feature.

## How to read the claims

* **Verified** is present in current source, a committed status/evidence file,
  or a live GitHub PR record read at this review head.
* **Reasoned** is an architectural conclusion from those facts and still needs
  a focused consumer experiment.
* **Speculative** has no named consumer and must not become a roadmap item.

A merged PR proves that its change reached `main`; it does not prove every
qualification sentence in its status document. An open PR is evidence of a
candidate only. Runtime, realtime, platform, and installed-SDK claims stay
bounded by their receipts.

## Current-main delta

The previous proposal correctly identified three seams: a typed event plane,
graph-owned voice lifecycle, and layered inspection/replay. Current `main` now
contains useful slices of the first two, which changes their priority and their
wording:

| Live record | What is verified | What remains outside the evidence |
| --- | --- | --- |
| PR [#12020](https://github.com/Generous-Corp/pulp/pull/12020), merged `ce9a634e2d6f6f320f9560dc10105b90a61d39f7`, source head `1c77e7d59343bfee691dfe7f0ed1295ea2b34294` | Sidechain feedback and dense control edges are observed in the graph probes. | This does not establish a general control/event scheduler or hosted-plugin parity. |
| PR [#11984](https://github.com/Generous-Corp/pulp/pull/11984), merged `f2664397c643807f01a4a885687ef6ebd4d30efa`, source head `59491c267dfb23654bdbf159e4a7294849b1b6b2` | Graph-authored event transport is hardened with bounded contracts. | `docs/graph-program-status/G-5.md` still records queued cross-block semantics, arbitrary callback routing, plugin parity, hard-realtime, and non-Darwin lanes as open or hold. |
| PR [#12000](https://github.com/Generous-Corp/pulp/pull/12000), merged `67ec6593452a4f94e813f44f9b533edb9e2c4e9e`, source head `1e7d8e0c431038d9049bcb2d19f1e20cdbb4e273` | A bounded eight-voice graph binding and SDK audio oracle now exist. | `G-6` remains in progress: voice scheduling, tails, graph state migration, and independent graph audio qualification are not promoted by the merge. |
| PR [#11998](https://github.com/Generous-Corp/pulp/pull/11998), merged `aea67edf573c2b0d310b4e69434cab8c99942afd`, source head `0673ffadd14b7f0f6909c117adde1a991e4969a8` | Finite two-asset spectral morph with live hop control is present. | A complete graph-authored spectral/frame workflow, frame-to-audio tail policy, and broad cross-platform evidence remain partial. |
| PR [#11999](https://github.com/Generous-Corp/pulp/pull/11999), merged `fa5a9f0205b38d255820505ae185d01ab9b4a47c`, source head `77a9777d50a9b8a46bff293efd9118f31c47ca9d` | Catalog-derived notation and strict graph diagnostics are available. | A single receipt that correlates authored graph, resolved resources, prepared plan, event replay, and output is still missing. |
| PR [#12008](https://github.com/Generous-Corp/pulp/pull/12008), **open**, head `245d55910dc1f847d14d3b214ea741eac357627d` | Candidate bounded resolved-graph identity bridge. | It is not merged or a public contract; do not treat it as shipped. |
| PR [#12017](https://github.com/Generous-Corp/pulp/pull/12017), **open**, head `1ea1eabbdb411f277bf989f25e67796df0a85642` | Candidate G6 pedal/A1 voice evidence. | It is not merged; the graph voice lifecycle remains an evidence question. |
| PR [#11733](https://github.com/Generous-Corp/pulp/pull/11733), closed without merge, head `d90324a2749fa6c6ab6acd90943fab6330be8425` | Earlier bounded control/event probe proposal. | Its candidate code is not current `main` and must not be copied as an implementation dependency. |

The source status files preserve the limits: `G-5` is **PARTIAL**, `G-6` is
**IN PROGRESS**, `G-10` remains **PARTIAL**, and the capability matrix labels
inspection/fingerprints and the unified control workflow **PARTIAL/AWKWARD**.
Those labels outrank an attractive example or a green check.

## Verified baseline and the remaining boundary

Current `SignalGraph` is a prepared, bounded audio topology. The public
reference and implementation provide stable node IDs, input/output/gain/plugin/
custom/processor nodes, explicit one-block feedback, sparse automation,
dense audio-rate modulation, sidechain ports, MIDI/UMP/MPE ingress, latency and
PDC, serial and parallel execution, prepared topology edits, canonical
serialization, signed bake artifacts, and bounded built-in subgraph flattening.
The relevant authority is `docs/reference/signal-graph.md`,
`docs/reference/signal-graph-capabilities.md`,
`core/host/include/pulp/host/signal_graph_runtime.hpp`, and
`core/host/src/signal_graph_prepare.cpp`.

The remaining boundary is a binding problem. MIDI, parameter queues, trigger
lanes, timeline events, voice allocators, spectral frames, and GPU resources
have different clocks, capacities, state, and failure policies. They are useful
individually, but a developer still writes the glue that must preserve sample
position, generation, reset, tail, resource identity, and realtime ownership.
Adding another DSP node does not remove that glue.

| Domain | Verified primitive | Missing product contract |
| --- | --- | --- |
| Event/control | `graph_event_contract.hpp`, trigger runtime, MIDI/UMP/MPE mailbox, sparse/dense parameter edges | One bounded cross-block event projection with absolute sample origin, tie order, late/overflow policy, reset epoch, and partition-invariant replay. |
| Voice | `voice_template.hpp`, `Synthesiser`, `MpeVoiceAllocator`, merged bounded eight-voice binding | A graph-owned lifecycle for allocation/steal, generation-safe tails, per-voice events, graph swaps, and declared state migration. |
| Inspection | Graph notation/diagnostics, serializer, prepared stats, probes, traces, audio-quality tools | Layered IDs and a replay/diff receipt that says whether a difference is authored topology, resource resolution, plan, event stream, or output. |
| Spectral | Canonical frames, routed spectral ports, finite two-asset morph, hop control | A frame-domain conversion contract with geometry, clock, latency, EOF/tail, and host-rate event policy. |
| Sampling | Recorder, sample-slot generation, loop cursor, sample-region and timeline primitives | Asset publication, residency/underrun policy, and graph voice scheduling as one bounded workflow. |
| GPU/neural | GPU spectral/convolution/neural primitives, CPU shadows/fallbacks, provider and device-generation checks | Shared graph admission for model identity, residency, warm-up, device loss, deadline, and fallback; this needs at least two consumers. |
| Isolation | Scanner/process-group utilities and explicit G-9 isolation limitation | Hosted-plugin callback IPC, crash recovery, state restore, sibling continuity, and platform process-tree semantics. |

These are **post-roadmap boundaries**, not claims that every row should become
core graph API. The current roadmap's partial/hold status remains the gate for
any implementation decision.

## Competitive architecture lessons

The useful comparison is architectural, not a feature-count contest:

| Reference | Observable idea | Pulp lesson | Deliberate limit |
| --- | --- | --- | --- |
| [Max/gen/RNBO](https://cycling74.com/products/max) | Messages, signals, reusable abstractions, and a bounded lowering target coexist. | Keep event/control edges explicit and lower only a finite prepared graph. | Dynamic message semantics are not a hard-realtime contract. |
| [Pure Data](https://puredata.info/) | Message and signal graphs share explicit block/sample behavior. | Test ordering and sample boundaries independently of audio topology. | Global mutable state and permissive patching are poor SDK defaults. |
| [SuperCollider](https://supercollider.github.io/) | Server node tree separates orchestration from realtime synthesis. | Keep orchestration/control publication separate from the immutable audio snapshot. | A server/OSC scheduler adds latency and lifecycle assumptions a plugin cannot require. |
| [Faust](https://faust.grame.fr/) | Static signal expressions expose predictable structure and resources. | Preserve a finite lowering boundary and declared state/resource costs. | Faust purity does not solve dynamic voices, event clocks, or asset lifecycles. |
| [VCV Rack](https://vcvrack.com/) and [Bitwig Grid](https://www.bitwig.com/the-grid/) | Reusable modules, modulation, and polyphonic patching are product multipliers. | Value-owned templates and bounded voice domains are more useful than a large node catalog. | Their editor/content ecosystem is outside Pulp's framework scope. |
| [JUCE AudioProcessorGraph](https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html) and [Synthesiser](https://docs.juce.com/master/classjuce_1_1Synthesiser.html) | Graph, parameter, format, and voice primitives stay separate and composable. | Bind Pulp's existing graph and MIDI primitives; do not fork a second synth or parameter stack. | Broad compatibility does not imply immutable snapshot or hard-realtime guarantees. |
| [WebGPU](https://www.w3.org/TR/webgpu/) and [RTNeural](https://github.com/jatinchowdhury18/RTNeural) | Device/provider and model execution expose availability and fallback concerns. | Make model/resource identity and CPU/GPU policy explicit at preparation. | Training, device discovery, and universal scheduling are separate lifecycles. |

The shared lesson is separation with explicit bindings: authoring identity,
event scheduling, realtime execution, and offline evidence can be related while
remaining independently testable. Pulp's immutable prepared snapshot is a good
execution boundary; the missing value is a small, durable binding around it.

## Prioritized smallest defensible primitives

### P0: layered inspection and replay receipt

**Reasoned priority.** This is the lowest-risk path because it can reuse current
serializer, prepared stats, notation, probes, traces, and audio-quality tools
without changing callback behavior. The open resolved-identity candidate is
useful evidence, but it is neither approval nor a dependency.

Define a compact versioned envelope with separate IDs for:

1. authored semantic graph bytes/schema;
2. resolved node/type/resource identities;
3. prepared execution-plan facts and generation;
4. event/control input stream plus clock epoch; and
5. rendered output digest or declared audio-quality artifact.

The receipt must include capacities, latency/fallback decisions, source/SDK
provenance, and positive plus planted-negative controls. A single canonical
graph hash is insufficient: equal authored graphs can resolve different assets,
and equal output hashes do not explain a scheduling change.

**Gate before implementation:** one installed-SDK or independent public
consumer must replay two block partitions and mutate one layer at a time. The
corresponding layer must fail while the others remain explainable.

### P1: bounded cross-block event projection

**Reasoned priority.** G-5 now has hardened bounded transport, but its status
still leaves queued cross-block scheduling and broad adapter parity open. Adapt
the existing MIDI/UMP/MPE, trigger, and parameter queues rather than introducing
an untyped universal queue.

The contract should own an epoch and absolute sample origin, fixed-capacity
block projection, deterministic `(absolute_sample, producer_order, sequence)`
ordering, explicit late/overflow behavior, reset/cancellation, and an offline
replay source. Producers stay control-side; the audio consumer performs no
allocation or graph mutation.

**Gate before implementation:** an installed consumer sends MIDI, a parameter,
a trigger, and a voice-intent record across two block boundaries. It proves
ordering, reset, overflow, late policy, partition invariance, and a sequence
mutation that fails the receipt. If this cannot be kept small, stop rather than
create a second scheduler.

### P2: graph-owned voice lifecycle binding

**Reasoned priority.** The merged eight-voice binding reduces the unknowns but
does not establish a general graph lifecycle. Reuse `Synthesiser`,
`MpeVoiceAllocator`, and `voice_template`; do not dynamically add graph nodes
from the callback.

The smallest contract has a fixed voice count, generation-tagged allocation and
steal, per-voice event projection, release-tail ownership, graph-swap behavior,
and declared state migration. A voice template expands at prepare time or into
a fixed preallocated slot runtime. Retired generations must reject stale events.

**Gate before implementation:** an installed eight-slot consumer covers note
on/off, sustain policy, MPE pressure, steal order, tails, slot reuse, swap while
voices sound, and state save/restore. A stale-generation mutation must fail.
The open G6 evidence candidate can inform the experiment, but its state does
not promote a capability.

### P3: explicit spectral/frame conversion (conditional)

The finite spectral morph is a useful bounded consumer, not a universal spectral
graph. Add conversion nodes only for a named workflow and make FFT size, hop,
window, channel layout, latency, EOF/tail, and host-rate event mapping explicit.
Keep graph scheduling at one host rate until conversion plus PDC evidence proves
that a second scheduler is necessary.

**Gate:** one consumer with frame geometry, partition-invariant offline render,
latency receipt, EOF/tail, CPU fallback, and a planted geometry mutation. Without
that consumer, keep this research-only.

### P4: shared model/resource admission (speculative)

GPU and neural primitives should remain independent until two graph consumers
need the same resource policy. A future signed manifest could name model/kernel
identity, residency, warm-up, provider, memory/latency budget, device-loss
transition, and CPU fallback. Training and device discovery remain external.

**Gate:** two independent consumers and a measured 64/128-frame workload with
device-loss, fallback, allocation, and output-tolerance controls. No gate means
no graph-wide GPU or neural scheduler.

## Assumptions challenged

1. **“The merged eight-voice binding completes graph polyphony.”** False. It is
   bounded implementation/evidence; G-6 still leaves scheduling, tails, state,
   and broader parity open.
2. **“A current event contract is a sequencer.”** False. Current G-5 evidence
   does not admit unrestricted cross-block scheduling, plugin parity, or hard
   realtime.
3. **“A canonical graph hash proves reproducibility.”** False. Keep semantic,
   resolved, plan, event, and output identities separate.
4. **“A spectral node is a spectral graph.”** False. Geometry, clock, latency,
   tail, and conversion ownership are separate obligations.
5. **“More node types solve authoring.”** False for voices, assets, timing, and
   resources; the missing unit is a lifecycle binding.
6. **“A prepared snapshot makes state migration automatic.”** False. Snapshot
   publication is safe; active voice/resource state still needs generation and
   transaction semantics.
7. **“MIDI is a universal event bus.”** False. MIDI, parameter, trigger, and
   voice-intent payloads have different validation and overflow policies.
8. **“GPU/neural execution is just another graph node.”** False. Provider,
   queue lead, readback, device loss, warm-up, and fallback are observable
   scheduling contracts.
9. **“Scanner cleanup proves hosted-plugin sandboxing.”** False. G-9 records
   isolation as unimplemented; callback IPC and crash/state recovery remain.
10. **“Every useful capability belongs in graph core.”** False. Recording,
    training, device discovery, catalog policy, and editor UI stay adjacent until
    a measured graph boundary is the actual bottleneck.

## Explicit non-goals

* A second general-purpose DSP graph beside `SignalGraph`.
* A stable cross-version C++ `Processor` binary ABI.
* Audio-thread allocation, file I/O, device discovery, or GPU queue submission.
* Implicit sample-rate conversion or a hidden graph-wide scheduler.
* A universal untyped MIDI/event transport.
* Mandatory runtime-nested graphs when bounded flattening suffices.
* A node for every physical-model, spatial, neural, or GPU algorithm before a
  reusable contract and named consumer exist.
* Hosted-plugin isolation claims inferred from scanner/process-group utilities.
* Graph ownership of model training, asset databases, device discovery, or a
  user-facing modular editor.

## Three-perspective adversarial self-review

**Audio DSP architect.** P0 and P1 risk becoming an unbounded scheduler. The
fixed capacities, one host clock, epoch/reset rules, and partition-invariant
fixture are mandatory; any proposal that needs arbitrary callback execution is
rejected. P3 must state frame latency and tail instead of hiding them in a node.

**Instrument developer.** P2 is useful only if the voice binding feels like a
voice template with stable musical identity, not a graph-internals API. Sustain,
steal, tails, MPE, and swap behavior must be observable from a public consumer.
The binding remains optional so ordinary `Synthesiser` users do not pay graph
complexity.

**Framework maintainer.** P0 should be generated from existing diagnostics and
remain read-only. P1/P2 must adapt existing queues and allocators, with separate
PRs and planted negatives. P3/P4 stay conditional; no graph-wide multirate,
GPU, ABI, or sandbox commitment is justified by current evidence.

All three reviews agree on the stop rule: if P0 or P1 cannot demonstrate a
repeated integration cost with a small public consumer, implement nothing and
retain the research note.

## Decision and next evidence

Finish and refresh the current roadmap's exact partial/HOLD statuses first. Then
run P0 as a docs/tooling experiment and P1 as one bounded installed consumer.
Only a repeated failure at the binding seam should justify P2. P3 requires a
named spectral consumer; P4 requires two independent GPU/neural consumers.
The evidence may invalidate this proposal. Until it does not, the smallest
defensible architecture is a layered receipt plus a bounded event projection
that reuses existing graph, MIDI, trigger, and voice primitives.

## Sources

### Pulp source and documentation

* `core/host/include/pulp/host/signal_graph_runtime.hpp`
* `core/host/include/pulp/host/graph_event_contract.hpp`
* `core/host/include/pulp/host/voice_template.hpp`
* `core/host/src/signal_graph_prepare.cpp`
* `core/host/src/signal_graph_subgraph.cpp`
* `core/host/src/spectral_graph.cpp`
* `core/midi/include/pulp/midi/synthesiser.hpp`
* `core/midi/include/pulp/midi/mpe_synth_voice.hpp`
* `core/audio/include/pulp/audio/audio_stream_handoff.hpp`
* `core/gpu_audio/include/pulp/gpu_audio/gpu_neural_realtime_node.hpp`
* `docs/reference/signal-graph.md`
* `docs/reference/signal-graph-capabilities.md`
* `docs/reference/node-abi.md`
* `docs/graph-program-status/G-5.md`
* `docs/graph-program-status/G-6.md`
* `docs/graph-program-status/G-7.md`
* `docs/graph-program-status/G-10.md`
* `docs/graph-program-status/G-11.md`
* `docs/graph-program-status/A10-routing-hosting.md`
* `docs/reference/2026-11-10-post-roadmap-audio-graph-opportunities.md`

### External architecture references

* <https://cycling74.com/products/max>
* <https://puredata.info/>
* <https://supercollider.github.io/>
* <https://faust.grame.fr/>
* <https://vcvrack.com/>
* <https://www.bitwig.com/the-grid/>
* <https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html>
* <https://docs.juce.com/master/classjuce_1_1Synthesiser.html>
* <https://www.w3.org/TR/webgpu/>
* <https://github.com/jatinchowdhury18/RTNeural>
