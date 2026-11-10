# Pulp audio graph after the current roadmap

**Status:** research and planning only. This document does not alter the graph
roadmap and does not authorize runtime or API implementation.

**Review date:** 2026-11-10

**Verified source head:** `bd695daca6cdd298ba95b738b6c4055ac5810eb0`
(`origin/main` at the time of this review).

**Question:** once the current graph program is complete, what important things
will developers still be unable to build elegantly with Pulp, why, and what is
the smallest defensible architectural response?

The terms below keep claims honest:

- **Verified** means checked against source, a committed test/evidence document,
  or a live PR head at the source head above.
- **Reasoned** means a design conclusion drawn from verified behavior and a
  representative use case. It still needs a focused experiment.
- **Speculative** means an attractive opportunity without a named Pulp consumer
  or acceptance evidence. It is not a roadmap commitment.

## Executive summary

Pulp is already a capable prepared audio graph host. `SignalGraph` has stable
node identities, built-in audio and MIDI nodes, hosted `PluginSlot` nodes,
stateful custom nodes, explicit feedback delay, sparse automation, dense
audio-rate modulation, sidechain edges, latency compensation, serial and
parallel executors, anticipation, graph limits, serialization, signed bake
artifacts, nested built-in subgraphs, and bounded spectral ports. These are
verified in `docs/reference/signal-graph.md`,
`docs/reference/signal-graph-capabilities.md`, the graph lifecycle receipts,
and the current graph status files.

The remaining problem is not a shortage of DSP nodes. The central limitation is
that Pulp has several good adjacent primitives without one small, public contract
for time, control events, voices, and inspection. A developer can make a
sophisticated instrument by combining `Processor`, `Synthesiser`, `MpeVoiceAllocator`,
`SignalGraph`, timeline, spectral engines, and GPU primitives, but must write the
binding and lifecycle glue themselves. That glue is where sample-accurate timing,
state migration, realtime safety, and reproducibility become fragile.

The smallest defensible follow-up is three connected contracts:

1. **A bounded control and event plane** with one explicit clock, absolute sample
   positions, cross-block scheduling, deterministic ordering, reset/epoch
   semantics, and a declared relationship to the audio graph. It should be a
   separate typed plane that can feed audio nodes, parameter lanes, and voice
   allocators. It should not become a second unconstrained scheduler.
2. **A graph-owned polyphonic binding** that maps a value-owned voice template to
   fixed voice instances, event delivery, tails, and transactional state. It
   should reuse `Synthesiser` and `MpeVoiceAllocator` rather than replace them.
3. **A deterministic inspection and replay contract** that emits semantic graph,
   prepared-plan, event, and rendered-output identities as separate records. A
   CLI/MCP workflow can then discover, validate, prepare, render, diagnose, and
   diff one graph without treating a hash as a proof of audio equivalence.

These three contracts unlock polyphonic modular instruments, event-driven
sequencers, agent-generated fixtures, and reliable spectral/control composition.
They also expose where Pulp should stop. Graph-level multirate execution,
first-class arbitrary conditional control flow, universal GPU scheduling,
precompiled third-party Processor ABI compatibility, and graph-native hosted
plugin sandboxing should remain gated by named consumers and measured contracts.

## Verified post-roadmap baseline

The table assumes all currently planned graph slices land with their documented
scope. It distinguishes an existing capability from a complete product workflow.

| Area | Verified implementation or adjacent primitive | Post-roadmap boundary |
| --- | --- | --- |
| Audio topology | `SignalGraph` connects input, output, gain, plugin, custom, and processor nodes. Stable `NodeId` values survive connection edits. `core/host/include/pulp/host/signal_graph_runtime.hpp` and `docs/reference/signal-graph.md` | A graph is an audio topology, not a general event or state-machine language. |
| Prepared execution | `prepare()` compiles an immutable snapshot; `process()` reads the published snapshot. Prepared stats expose nodes, ports, work, delay bytes, spectral bytes, and generation. `signal_graph_prepare.cpp` and `signal_graph_runtime.hpp` | Mutation is control-thread work. A failed preparation can leave silence or require eager preparation. |
| Live edits | Prepared topology edits and live swap paths validate, prepare, and publish atomically for an admitted subset. `docs/guides/signal-graph-workflow.md` | Latency changes, feedback, queue egress, and some hosted lifecycles still require an interruption or eager path. |
| Feedback | `connect_feedback` represents an explicit one-block delay and keeps the execution graph acyclic. `signal_graph_connection.hpp` and `signal_graph.md` | Feedback has fixed one-block semantics. Arbitrary zero-delay algebraic loops are intentionally refused. |
| Automation and modulation | Sparse block-relative automation and dense audio-rate modulation are separate edge kinds. Dense modulation is bounded by lane and block limits and differs for hosted plugins and native Processor nodes. | There is no general control graph with branching, state machines, or future-event scheduling. |
| Events and MIDI | MIDI ingress and egress are fixed-capacity, ordered, and preserved over admitted swaps. Host event projection supports MIDI, UMP, and optional MPE sidecars. | The typed trigger runtime is bounded and current-sample oriented; graph-authored callback scheduling and cross-block event transport remain narrower than a sequencer model. |
| Polyphony | `Synthesiser`, `MpeVoiceAllocator`, `UnisonVoiceStack`, sample voice renderers, and a value-owned `voice_template` schema exist. | The graph binding does not yet own voice allocation, per-voice graph snapshots, event routing, tails, or graph state migration as one contract. |
| Subgraphs | `SubgraphTemplate` recursively flattens bounded built-in Input, Output, and Gain templates with deterministic child identity and exposed ports. | Plugin, custom-node, event, and polyphony children are outside the admitted subgraph runtime. Flattening is not first-class runtime nesting. |
| Serialization and bake | `.pulpgraph` canonical serialization, limits, unresolved custom-node preservation, and signed lowerable bake artifacts are implemented. | Semantic graph identity, resolved execution-plan identity, and rendered-output identity are separate but not yet exposed through one inspection/replay workflow. |
| Custom nodes | Source-level custom nodes support lifecycle, state, transport callbacks, event callbacks, latency, realtime declarations, and experimental `pulp_node_v1` C ABI plus signed node packs. `docs/reference/node-abi.md` | The ABI is experimental and source/rebuild oriented. It is not a stable Processor binary plug-in contract. |
| Spectral processing | Offline spectral frame codec and bounded routed frame ports exist. `core/host/src/spectral_graph.cpp`, `core/signal`, and G-10 status documents cover canonical frames and identity checks. | Runtime spectral modulation, resynthesis tails, granular fields, and a complete graph-authored spectral workflow remain partial. |
| Granular and sampling | Scalar sample-region kernels, streaming sources, sample voice renderers, and timeline/offline sample examples exist. | A graph does not yet provide a general live sampler with recording, publication, granular scheduling, and realtime-safe asset lifecycle. |
| Multirate | Node-local resampling and `AudioStreamHandoff` support source/host rate conversion. The graph prepares one host rate. `docs/reports/graph-multirate-capability-audit-2026-11-02.md` | There is no graph-owned rate-domain scheduler or conversion-edge/PDC contract. This is a deliberate no-change decision until a named consumer exists. |
| GPU and neural DSP | GPU spectral, convolution, and neural primitives exist with explicit CPU fallback and device-generation checks. `docs/examples/gpu-nam.md` and `docs/examples/super-convolver.md` document their boundaries. | GPU work is not a graph-wide realtime domain. Device loss, readback, scheduling, and fallback remain primitive-specific. |
| Isolation | Scanner containment and POSIX process-group cleanup exist. G-9 evidence explicitly records `isolation_implemented=false`. | Hosted-plugin graph isolation, callback IPC allocation, crash recovery, state restore, sibling continuity, and Windows process-tree semantics remain unproven. |
| Tooling | `pulp graph inspect`, `validate`, and `canonicalize`, kit validation, graph limits, receipts, traces, and audio-quality tools exist in adjacent paths. | There is no single public graph inspect/diff/render/diagnose command or MCP operation that carries all artifact lineage. |

The current graph program status files are the source of truth for the in-flight
slices. At this review, they classify G-3, G-5, G-6, G-9, G-10, G-11, A9, and
A10 as partial, in progress, or hold even when individual evidence lanes are
strong. A green PR or a local fixture does not promote a capability beyond its
stated boundary.

## What developers can build today

A developer can construct a deterministic feed-forward rack, add feedback delay,
route MIDI and sidechain audio, automate plugin parameters, use dense modulation
on eligible parameters, save/load a graph, lower a trusted graph to a signed
artifact, and render it offline. They can use a `Synthesiser` or MPE allocator
inside a `Processor`; they can use spectral and granular primitives outside the
main graph; and they can use nested built-in templates for bounded composition.

The important boundary is the seam between these facts. A polyphonic graph
requires a voice allocator to create or retire per-voice node state, a graph
binding to deliver events at sample offsets, a tail policy, and a state identity
that survives allocation reuse. A spectral graph requires frame geometry,
window/hop chronology, latency, and a stream-tail policy. A GPU graph requires
submission, readback, device-loss, and CPU fallback semantics. These are not
solved merely by adding another node type.

## Competitive and architectural gap analysis

This comparison describes useful architectural ideas, not claims that Pulp
should clone another system.

| System | Capability that matters | Architectural lesson for Pulp | Cost or objection |
| --- | --- | --- | --- |
| Max/MSP, gen~, RNBO | A patch combines audio signals, messages, scheduling, state, and reusable abstractions; gen lowers a bounded patch to DSP. [Max](https://cycling74.com/products/max), [gen](https://docs.cycling74.com/legacy/max8/vignettes/gen), [RNBO](https://rnbo.cycling74.com/) | A typed event/control plane and explicit lowering boundary are more valuable than an unlimited node catalog. | Max's dynamic message semantics and host assumptions are difficult to make hard realtime and portable. |
| Pure Data | Message boxes, control flow, and signal graphs coexist with explicit block/sample semantics. [Pd](https://puredata.info/) | Keep event/control edges explicit and test ordering independently from audio. | Pd's permissive dynamic patching and global state are poor defaults for a bounded SDK. |
| Kyma | Sound objects, time, and hierarchical composition are first-class; scheduling is part of the musical model. [Kyma](https://kyma.symbolicsound.com/) | A value-owned composition identity and clock contract would make Pulp's prepared snapshots more expressive. | Kyma's environment is a complete musical computer; importing its scope would overbuild Pulp. |
| SuperCollider | Server-side node tree, buses, groups, patterns, and demand-rate sequencing separate control from realtime synthesis. [SuperCollider](https://supercollider.github.io/) | Separate orchestration from the audio server; make graph publication and event delivery explicit. | OSC/server scheduling adds latency and lifecycle complexity that a plugin cannot assume. |
| Csound | Score/event scheduling and a rich DSP language are explicit, with offline and realtime modes. [Csound](https://csound.com/) | Give generated graphs a bounded event timeline and deterministic offline replay. | A score language is larger than a plugin SDK and would duplicate timeline functionality. |
| Faust | A functional DSP language compiles signal expressions and exposes predictable static structure. [Faust](https://faust.grame.fr/) | Preserve a static lowering target: finite graph, declared resources, deterministic execution, and explicit state. | General conditional/stateful control and dynamic topology require different analysis than Faust's pure signal model. |
| Reaktor, VCV Rack, Bitwig Grid | User-facing modular systems need reusable modules, polyphony, modulation, and safe patching. [VCV Rack](https://vcvrack.com/), [Bitwig Grid](https://www.bitwig.com/the-grid/), [Reaktor](https://www.native-instruments.com/en/catalog/komplete/reaktor/) | Reusable subgraphs, voice domains, and modulation metadata are product multipliers. | Their UI and content ecosystems are separate products; Pulp should expose contracts, not a patch browser. |
| JUCE | `AudioProcessorGraph`, `Synthesiser`, buses, parameters, and format adapters are deliberately separate C++ primitives. [AudioProcessorGraph](https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html), [Synthesiser](https://docs.juce.com/master/classjuce_1_1Synthesiser.html) | Pulp already has analogous pieces. Reuse and bind them instead of inventing a second synth or parameter stack. | JUCE's broad compatibility and dynamic ownership model do not provide Pulp's immutable snapshot guarantees by default. |
| Modern GPU audio | GPU compute can win for large parallel workloads, but queueing, readback, device loss, and fallback dominate small blocks. [WebGPU](https://www.w3.org/TR/webgpu/), [Dawn](https://dawn.googlesource.com/dawn/) | Keep GPU as an explicit optional execution primitive with a measured lead and fallback, not an implicit graph rate. | A unified CPU/GPU scheduler risks nondeterminism and deadline misses in plugin callbacks. |
| Neural/differentiable DSP | Models enable new timbres and inverse design, but model identity, memory, warm-up, and deterministic inference matter. [RTNeural](https://github.com/jatinchowdhury18/RTNeural), [DDSP](https://github.com/magenta/ddsp) | Treat model packs as signed, versioned resources with explicit latency and CPU/GPU policy. | Training, model serving, and graph execution are different lifecycles; Pulp should not become a training framework. |

The strongest common lesson is separation of concerns: systems that scale keep
composition identity, event scheduling, realtime DSP, and offline rendering
related but independently testable. Pulp's current prepared snapshot is a good
realtime boundary; the missing piece is a durable control/event binding around
it.

## Missing capability inventory

### Graph expressiveness

**Audio-rate modulation and arbitrary signal connections are verified but
bounded.** `Connection` distinguishes audio, MIDI, automation, dense modulation,
sidechain, feedback, and spectral edges. Native Processor nodes can receive
borrowed dense modulation views; hosted plugin delivery uses a bounded parameter
queue. Arbitrary signal typing, dynamic port shape, and multi-output semantic
contracts remain awkward.

**Sample-accurate events are partial.** Host event projection preserves absolute
positions and sidecars, but the graph's current typed trigger path is bounded and
current-sample oriented. A future event queue needs a single ordering rule for
same-sample events, late events, cross-block carry, epoch reset, and overflow.
The existing `ParameterEventQueue`, MIDI buffers, and trigger queues should be
adapted rather than merged into an untyped universal queue.

**Signal combinators, conditionals, and state machines are missing as a public
control language.** Custom nodes can implement these behaviors, but doing so
hides resource bounds, state identity, and event semantics from graph validation.
A general-purpose branch node would also make static work and latency analysis
harder. The defensible opportunity is a small event/control vocabulary with
bounded switch, gate, latch, counter, and state-transition nodes whose state and
cost are explicit.

**Nested graphs are bounded, not runtime-hierarchical.** Subgraphs flatten built-in
templates and retain deterministic child identity. Flattening is a good default
for realtime scheduling. First-class runtime nesting should be considered only
if it enables a named use case that requires encapsulated lifecycle or independent
resource budgets.

**Graph polyphony is outside the admitted runtime.** Pulp has established voice pools and
MPE primitives, but the graph has no admitted per-voice expansion, event-to-voice
routing, tail retirement, or voice state migration contract. This prevents a
fully graph-defined modular synth without custom glue.

**Feedback semantics are explicit and useful.** One-block feedback makes loops
safe and deterministic. Audio-rate zero-delay feedback, algebraic loop solving,
and arbitrary dynamic topology are intentionally absent. They are specialized
features with steep realtime and stability costs.

**Multirate execution is genuinely missing at graph level and intentionally
parked.** Node-local resampling is available; graph-owned domains would require
conversion nodes, clock mapping, PDC, modulation timing, buffer budgets, and
offline parity. Do not add per-node rates before an explicit mixed-rate consumer
exists.

**External phase, time, and signal indexing are incomplete.** Custom transport
callbacks receive a block context, timeline provides richer transport, and
sample streams expose rates. There is no common graph clock that every event,
voice, spectral frame, and automation lane can query with the same epoch and
sample origin.

### Advanced audio processing

**Spectral graph processing is partial.** Canonical frames and routed frame ports
exist, but runtime modulation, resynthesis tails, granular frame fields, and a
complete external consumer remain unqualified. A typed frame domain with explicit
geometry and tail policy is more valuable than exposing every spectral algorithm
as a node.

**Granular synthesis and live sampling are adjacent, not graph-native.** Pulp has
streaming sources, sample regions, timeline rendering, and granular kernels. A
live instrument still needs a bounded recorder, asset publication, page residency,
voice scheduling, and underrun policy. Those should be a sample/asset service
feeding graph nodes, rather than callback file I/O in graph processing.

**Physical modeling and waveguides are algorithm opportunities, not graph
primitives.** Delay lines, feedback edges, and custom nodes can host them. A
framework-level waveguide type is justified only by a repeatable resource or
portability problem that custom nodes cannot solve.

**Spatial audio and ambisonics are missing as a unified graph contract.** Existing
multi-channel buses and sidechains are not a channel-layout or coordinate-space
model. A useful future slice would define typed layouts, order/normalization,
rotation, and latency, then prove one renderer. It should not start as dozens of
spatial nodes.

**Microtonal tuning is available around MIDI but not graph-owned.** Pulp has MPE,
tuning packages, and synth primitives. A graph needs only a stable pitch/frequency
mapping event contract if a named graph instrument requires it.

**Neural DSP and hybrid CPU/GPU execution are primitives, not graph guarantees.**
GPU NAM and spectral examples prove valuable kernels and fallback patterns. They
do not prove model loading, graph scheduling, deadline behavior, or cross-platform
parity. A model resource contract and a measured asynchronous boundary should
precede any graph integration.

### Dynamic and interactive systems

Generative composition, sensor input, graph-defined sequencing, and adaptive
instruments are not one missing node. They need a control/event plane, an external
clock/input adapter, deterministic random seeds, and an offline replay model.
Live graph modification exists for prepared topology edits; state migration for
an active voice bank or a time-varying control state is still a separate contract.

### Developer and agent experience

Pulp has the pieces for an agent workflow: bounded graph limits, serializer,
canonicalization, kit validation, bake verification, diagnostics, traces, and
quality tools. The missing practical surface is a single receipt envelope
that binds:

- semantic authored graph bytes and schema;
- resolved node/type/resource identities;
- prepared snapshot generation and execution-plan facts;
- event/control input stream and clock epoch;
- partition schedule and transport contract;
- rendered output digest or audio-quality artifact; and
- negative-control results.

A fingerprint alone is insufficient. Two plans can have different memory
addresses and equal semantics; two equal semantic graphs can resolve different
resources; and equal output hashes do not explain a scheduling change. Keep the
three identities separate and make their relationships explicit.

## Proposals with the greatest practical value

### P1: bounded control and event plane

**Problem.** Developers currently combine MIDI, parameter queues, trigger lanes,
timeline events, and custom callbacks with different capacities and clocks.
Cross-block scheduling and deterministic replay require application-specific glue.

**Direction.** Introduce a value-owned `GraphEventStream` contract with:

- a graph epoch and absolute sample origin;
- fixed-capacity block projection with explicit overflow and late policies;
- stable ordering `(absolute_sample, source_order, sequence)`;
- typed payload families for MIDI/UMP/MPE, parameter, trigger, and voice intent;
- reset and cancellation semantics;
- a control-thread producer and an audio-thread consumer;
- optional offline event source replay; and
- no graph mutation or allocation on the audio thread.

Keep it as a separate plane. Audio edges consume projected values; event edges
carry events; the scheduler does not inspect arbitrary plugin internals.

**Alternatives.** Reusing `MidiBuffer` for every event loses typed identity and
would force parameter and voice semantics into MIDI. A full SuperCollider-like
server is too large. A per-node queue duplicates capacity and ordering rules.

**Acceptance experiment.** Build one installed-SDK fixture with a MIDI note,
parameter change, trigger, and voice-intent event crossing two block boundaries.
Require exact ordering, epoch reset, overflow refusal, late-event policy,
partition-invariant output, and a planted mutation that changes one sequence
number and fails the receipt.

**Strongest objection.** A second event abstraction could become another
incompatible queue. The proposal is defensible only if it adapts existing queues,
keeps payload families explicit, and removes duplicate ordering logic.

**Priority:** foundational, after current G-5 evidence stabilizes.

### P2: graph-owned polyphonic binding

**Problem.** `Synthesiser`, `MpeVoiceAllocator`, and voice templates are useful
individually, but graph authors cannot express a fixed voice bank whose audio,
control, event, tail, and state identities travel together.

**Direction.** Add a control-side `VoiceBankBinding` that references a versioned
voice template and expands a bounded number of graph instances at prepare time.
The runtime should own fixed voice slots and generation-tagged allocations. A
voice intent event selects or steals a slot; the graph routes per-voice events and
mixes declared outputs. Release tails remain attached to the generation that
created them. State save/restore is transactional and records slot identity,
voice generation, and declared state only.

Reuse existing `Synthesiser` and `MpeVoiceAllocator` policy hooks. Do not make a
voice a dynamic graph node on the audio thread. Use prepared expansion or a
fixed slot runtime with preallocated scratch buffers.

**Alternatives.** Duplicating the entire graph per note is simple but expensive
and makes publication/state migration hard. A single monolithic Processor hides
voice topology and defeats graph inspection. Dynamic node insertion during
processing violates the prepared snapshot model.

**Acceptance experiment.** Eight-slot installed consumer: note-on/off, sustain,
sostenuto, MPE per-note pressure, oldest and quietest stealing, release tails,
slot reuse, graph swap while voices sound, and state save/restore. Compare
partitioned and offline renders. Mutate a stale generation and require refusal.

**Strongest objection.** Polyphony is a product-level synth feature, not a
universal graph primitive. Keep the binding optional, fixed-capacity, and
reusable for samplers and modular instruments; do not require every graph to
carry voice state.

**Priority:** foundational for a graph-defined instrument, after P1 and G-4/G-6
contracts.

### P3: deterministic graph inspection, replay, and diff

**Problem.** Current diagnostics are strong but distributed. Agents and humans
must manually correlate serializer output, prepared stats, receipts, traces, and
rendered artifacts.

**Direction.** Define a versioned, JSON receipt envelope with four linked IDs:
semantic graph, resolved plan, event replay, and rendered output. Add read-only
`pulp graph inspect`, `validate`, `prepare-report`, `render`, `diff`, and MCP
operations that reuse existing graph/control authority. `diff` must state whether
the difference is authored topology, resource identity, plan assignment, event
stream, or output artifact.

Do not expose raw memory pointers, worker thread IDs, or unstable scheduler
addresses as identity. Include limits, capacities, latency, fallback decisions,
negative controls, and source/SDK provenance.

**Alternatives.** A single SHA of canonical JSON is easy but cannot identify
resolved resources or runtime execution. A binary trace is rich but not a stable
agent-facing contract. A CLI-only flow misses applications that already use the
control registry.

**Acceptance experiment.** Run the same `.pulpgraph` through two block partitions
and one serial/parallel executor selection. Semantic and resolved IDs must remain
stable; plan IDs may differ only where the contract allows; output oracle must
pass. Mutate one node version, one resource hash, one event sequence, and one
output sample and require the corresponding layer to fail.

**Strongest objection.** More receipts increase maintenance and can create false
confidence. Keep the envelope compact, make each claim scoped, and require
planted-negative controls.

**Priority:** high and relatively low risk; can proceed in parallel with P1.

### P4: explicit conversion and typed domains

**Problem.** Spectral frames, audio samples, control values, and possible future
spatial or multirate data have different clocks and lifetimes. Treating them as
plain audio ports loses contracts.

**Direction.** Add explicit conversion nodes and domain metadata only for a named
workflow: for example, `AudioToSpectralFrame` and `SpectralFrameToAudio` with
FFT size, hop, window, channel layout, latency, and tail policy. For mixed rates,
make the converter own resampling and PDC. Keep graph scheduling at the host
rate until a second scheduler is proven necessary.

**Alternatives.** Add a `sample_rate` field to every node. This creates implicit
converters, ambiguous clocks, and hidden latency. Make every node rate-agnostic.
That pushes aliasing and buffering into application glue.

**Acceptance experiment.** One spectral/granular consumer with explicit frame
geometry, host-rate event projection, partition-invariant offline render,
latency receipt, EOF tail, and CPU fallback. A mixed-rate consumer must prove
conversion quality, PDC, event phase, and memory bounds before any graph-wide
rate domain is admitted.

**Strongest objection.** The existing spectral engines and node-local resampler
may be enough. Require a real consumer and measured failure before expanding the
core graph.

**Priority:** conditional and specialized.

### P5: model/resource contract for neural and GPU nodes

**Problem.** GPU and neural primitives need model identity, residency, warm-up,
device loss, deadline, fallback, and output parity. A generic graph node does not
supply those.

**Direction.** Define a signed, versioned `GraphResource` manifest that names model
or kernel assets, memory and latency budgets, provider requirements, and CPU
fallback. Preparation verifies resources; processing sees only a prepared handle.
Readback and device-loss behavior are explicit state transitions. Keep training
outside Pulp.

**Acceptance experiment.** One conditioned-WaveNet or spectral node with a
small deterministic model, CPU fallback, device-loss simulation, warm-up budget,
realtime allocation probe, and output tolerance. A model hash mutation must fail
before code execution.

**Strongest objection.** Existing GPU examples already solve their narrow needs.
Do not promote this to a graph feature until two independent graph consumers need
shared resource admission.

**Priority:** speculative until a second consumer appears.

## Architectural alternatives and tradeoffs

### Unified graph versus split audio and control/event graphs

A unified graph makes authoring visually simple but forces audio scheduling,
message ordering, and state-machine semantics into one type system. A split
model keeps audio edges statically analyzable and lets event/control streams be
bounded, ordered, and replayable. The recommended design is a split typed plane
with explicit bindings, presented as one authoring document. This preserves a
single saved artifact without requiring one scheduler to execute every domain.

### First-class nested runtime versus flattening

Flattening templates gives deterministic node order, straightforward PDC, and
simple resource accounting. First-class nested runtime would support encapsulated
state and independent budgets but introduces hierarchy-aware latency, debugging,
and publication. Keep flattening as the default. Add runtime nesting only after a
consumer proves that namespace and lifecycle isolation cannot be represented by
value-owned templates.

### Immutable prepared snapshots versus incremental live mutation

Immutable snapshots make audio-thread behavior understandable and support reader
lifetime, stale-edit rejection, and deterministic receipts. Incremental mutation
could reduce prepare cost but would require lock-free ownership of every node,
edge, delay ring, and voice state. Retain immutable snapshots. Optimize compile
and reuse off-thread only when measurements show preparation latency is a user
problem.

### Per-node sample rates versus explicit conversion nodes

A per-node rate field looks convenient and hides a scheduler. Explicit conversion
nodes put resampling, latency, buffer size, and clock crossing where they can be
validated and inspected. Start with explicit converters; introduce graph-level
rate domains only if conversion nodes cannot meet a measured workload.

### In-process hosted plugins versus process isolation

In-process hosting is lower latency and already supported by `PluginSlot`.
Process isolation can contain crashes but needs callback IPC, shared buffers,
restart, state restoration, sibling continuity, and platform-specific process
semantics. Keep isolation as an optional host policy with a separate lifecycle
contract. Do not imply it from scanner containment or a hostile fixture.

## Assumptions challenged or invalidated

1. **More graph node types will solve expressiveness.** False for polyphony,
   scheduling, and live sampling. The missing contracts are identity, time,
   ownership, and lifecycle.
2. **A canonical graph hash is a reproducibility proof.** False. Semantic,
   resolved-plan, event, and output identities answer different questions.
3. **Graph serialization implies portability.** False. Processor nodes are runtime
   only; custom node packs and resources need version, trust, and platform policy.
4. **A prepared snapshot makes dynamic graphs easy.** False. It makes publication
   safe, but state migration and latency changes still need explicit transactions.
5. **MIDI is a universal event bus.** False. MIDI, parameter, trigger, and voice
   intent have different payload and overflow semantics.
6. **GPU execution is just another node implementation.** False. GPU queue lead,
   readback, device loss, and CPU fallback are observable scheduling contracts.
7. **Per-node multirate is a harmless extension.** False. It changes clocks,
   latency, event phase, memory, offline rendering, and scheduler design.
8. **Process fixtures prove hosted-plugin isolation.** False. G-9 explicitly
   records `isolation_implemented=false`; production callback and recovery paths
   remain open.
9. **A separate universal framework ABI is necessary.** Unproven. Pulp's
   experimental `pulp_node_v1` is useful for custom nodes; Processor binaries are
   already covered by VST3, AU, CLAP, and source-compatible Pulp contracts.
10. **Every missing capability belongs in graph core.** False. Recording,
    training, device discovery, plugin catalog policy, and product UI should stay
    in adjacent services unless a graph boundary is the demonstrated bottleneck.

## Recommended priorities and sequencing

| Order | Slice | Gate before implementation | Evidence needed |
| --- | --- | --- | --- |
| 0 | Close current roadmap statuses and preserve explicit HOLDs | Exact-head status refresh and adversarial review | Current main, receipts, PR/review state |
| 1 | P3 inspection/replay receipt envelope | One repeated manual correlation cost | Layered identity mutation controls and partition replay |
| 2 | P1 control/event plane | One cross-block installed consumer | Ordering, epoch, overflow, late, replay, realtime allocation |
| 3 | P2 voice-bank binding | P1 event semantics and G-4 identity/state | Eight-slot MPE/tail/state consumer with stale-generation negative |
| 4 | P4 typed frame/conversion node | Named spectral or mixed-rate consumer | Geometry, PDC, EOF/tail, fallback, offline parity |
| 5 | Spatial layout contract | A concrete ambisonic or object renderer | Layout/order/orientation/latency and CPU fallback |
| 6 | P5 neural/GPU resource contract | Two independent graph consumers | Signed resources, device loss, fallback, deadline and output oracle |
| 7 | Dynamic topology beyond prepared edits | Measured preparation or authoring bottleneck | Bounded incremental publication experiment |

The inspection/replay slice should be small enough to proceed without changing
runtime behavior. The event and voice slices should be separate PRs so their
contracts can be reviewed independently. Specialized spectral, spatial, and GPU
work should depend on a concrete consumer and not block core graph stability.

## Ambitious examples as architectural tests

These examples are probes for weaknesses, not feature commitments.

### Graph-defined polyphonic modular synthesizer

An eight-voice template contains oscillator, envelope, filter, modulation, and
output subgraphs. MIDI 2.0 per-note events allocate a voice, route pitch and
pressure, and release a tail. The current pieces cover custom nodes, MPE, voice
identity, prepared edits, and audio mixing. The failure point is the missing
single binding that owns event-to-voice scheduling, generation-safe tails, and
state migration. P1 plus P2 should make this possible without dynamic audio-thread
node insertion.

### Live spectral/granular instrument

A recorder publishes bounded sample pages; a granular voice bank schedules grains;
a spectral stage applies a frame-domain mask and resynthesizes with an EOF tail.
Current sample-region, streaming, spectral, and timeline primitives can each do
part of this. The failure points are asset publication, frame clock alignment,
voice scheduling, and an explicit frame-to-audio latency/tail contract. P1 plus
P4 is the smallest path; a graph-wide multirate scheduler is not required for
the first consumer.

### Generative instrument with MIDI, audio, and sensors

MIDI notes, envelope features, and a sensor stream drive a deterministic pattern
state machine. Offline replay must produce the same events and output. Current
MIDI, parameter, analysis, and timeline APIs are adjacent but have no common
clock/epoch and random-seed receipt. P1 and P3 are required; a general control
language remains optional until the fixture shows which bounded nodes are useful.

### Hybrid CPU/GPU/neural graph

A CPU preprocessor feeds a conditioned-WaveNet GPU node with a CPU fallback, then
an audio limiter. Device loss must transition to fallback without stale output;
offline rendering must match within a declared tolerance. Current GPU NAM and
neural primitives demonstrate the kernels, but not graph admission, model resource
identity, or deadline behavior. P5 is justified only after two such consumers.

### Agent-generated instrument

An agent emits a `.pulpgraph`, event stream, and resource manifest; Pulp validates
limits, prepares, renders a fixed partition schedule, and emits a layered receipt.
A planted mutation to a node version, resource hash, event sequence, or output
sample fails at the corresponding layer. Current serializer, limits, bake, and
quality tools make this close, but a unified receipt/diff workflow is missing.
P3 is the direct enabler and should be measured before adding more automation.

## Explicit recommendations for what not to build

- Do not add a second general-purpose DSP graph beside `SignalGraph`.
- Do not expose arbitrary C++ `Processor` binaries as a stable cross-version ABI.
- Do not make every graph node dynamically allocate, load files, or call a GPU
  queue from the audio callback.
- Do not add implicit sample-rate conversion or a hidden graph scheduler.
- Do not turn MIDI into a universal untyped event transport.
- Do not treat scanner process cleanup as hosted-plugin sandboxing.
- Do not make runtime nesting mandatory when flattening gives better analysis.
- Do not add a node for every physical-model, spatial, or neural algorithm before
  a reusable contract is demonstrated.
- Do not promise output determinism across different GPU providers without a
  declared tolerance and fallback policy.
- Do not make the graph own training, device discovery, asset databases, or a
  user-facing modular editor.

## Open questions and experiments before commitment

1. What one Pulp consumer would fail without P1 but remain simple enough for an
   installed-SDK receipt?
2. Can one event stream adapt MIDI, parameter, trigger, and voice intent without
   reintroducing per-family ordering bugs?
3. What is the minimum voice-bank state needed to migrate an active graph swap
   without copying opaque DSP state?
4. Should event streams be persisted in `.pulpgraph`, a timeline document, or a
   separate replay sidecar?
5. Which spectral frame geometry and tail policy cover the first real consumer?
6. What latency and jitter budget makes a GPU node useful at 64 or 128 samples?
7. Can resource admission reuse signed node-pack manifests, or does model
   provenance require a distinct envelope?
8. Which spatial layout representation is portable across AU, VST3, CLAP, and
   standalone hosts?
9. How much preparation time is acceptable before optimizing snapshot compilation?
10. Can a bounded state-machine node be expressed as serialized data with static
    work and memory limits?

## Adversarial review of this proposal

### Audio DSP architect

The proposal preserves the immutable prepared snapshot and puts event ordering,
voice allocation, conversion, and GPU/resource admission at explicit boundaries.
It avoids zero-delay feedback and implicit multirate scheduling. The main risk is
that P1 grows into an unbounded scheduler; the acceptance fixture must enforce
fixed capacity, epoch semantics, and a bounded projection cost. The spectral and
GPU proposals deliberately require frame/timing and fallback receipts before
runtime integration.

### Experienced instrument developer

The proposal addresses the highest-friction workflow: a voice bank that can be
patched, automated, saved, swapped, rendered, and replayed without hand-written
lifecycle glue. It keeps ordinary `Synthesiser` users simple and makes the graph
binding optional. The main risk is an API that exposes graph internals instead of
musical concepts. Use stable authored names and voice intents, and keep allocator
policy reusable.

### Skeptical framework maintainer

The proposal limits new core concepts to three contracts, reuses existing queues,
voice allocators, serializers, and signed resource boundaries, and gives every
specialized feature a named-consumer gate. The main risk is documentation and
receipt burden. Keep receipts machine-readable, scoped, and generated from
existing diagnostics; do not add a framework service or a second graph engine.

The strongest conclusion from all three reviews is to implement no speculative
feature until P3 or P1 has a concrete consumer and a planted-negative fixture.
Several attractive items, especially graph-wide multirate scheduling, universal
GPU execution, and general conditional language, should remain research topics.

## Sources and code references

### Pulp source and documentation reviewed

- `core/host/include/pulp/host/signal_graph_runtime.hpp` : graph limits,
  prepared stats, spectral ports, processor nodes, event ingress, prepared edits,
  executor routing, anticipation, and process contracts.
- `core/host/include/pulp/host/signal_graph_connection.hpp` : typed connection
  flags for MIDI, feedback, automation, audio-rate modulation, sidechain, and
  spectral edges.
- `core/host/include/pulp/host/custom_node_type.hpp` : custom-node lifecycle,
  event callbacks, state, latency, realtime declarations, and bake hooks.
- `core/host/include/pulp/host/voice_template.hpp` and
  `core/host/src/voice_template.cpp` : value-owned voice binding schema and
  persistence boundary.
- `core/host/src/signal_graph_prepare.cpp` : immutable compilation, validation,
  lifecycle, latency, and publication behavior.
- `core/host/src/signal_graph_subgraph.cpp` : bounded recursive template
  flattening and exposed-port composition.
- `core/host/src/spectral_graph.cpp` : spectral identity, frame geometry, and
  runtime admission checks.
- `core/midi/include/pulp/midi/synthesiser.hpp` and
  `core/midi/include/pulp/midi/mpe_synth_voice.hpp` : existing polyphony and MPE
  primitives.
- `core/audio/include/pulp/audio/audio_stream_handoff.hpp` : prepared ring and
  source/host resampling boundary.
- `core/gpu_audio/include/pulp/gpu_audio/gpu_spectral_morph.hpp` : explicit GPU
  spectral morph lifecycle and non-realtime readback boundary.
- `docs/reference/signal-graph.md` : public graph behavior and latency/PDC
  semantics.
- `docs/reference/signal-graph-capabilities.md` : runtime, authoring, tooling,
  and evidence matrix.
- `docs/guides/signal-graph-workflow.md` : load, validate, prepare, edit, render,
  inspect, bake, and agent acceptance workflow.
- `docs/reference/node-abi.md` : experimental `pulp_node_v1` and signed node-pack
  boundary.
- `docs/reports/graph-multirate-capability-audit-2026-11-02.md` : explicit
  no-change multirate decision.
- `docs/reports/g9-hostile-containment-verdict-20261102.md` : explicit containment
  limitation.
- `docs/graph-program-status/G-2.md` through `G-11.md`, `A9-agent-design-loop.md`,
  and `A10-routing-hosting.md` : current program status and evidence boundaries.

### External architectural references

- Max and gen: <https://cycling74.com/products/max> and
  <https://docs.cycling74.com/legacy/max8/vignettes/gen>
- RNBO: <https://rnbo.cycling74.com/>
- Pure Data: <https://puredata.info/>
- Kyma: <https://kyma.symbolicsound.com/>
- SuperCollider: <https://supercollider.github.io/>
- Csound: <https://csound.com/>
- Faust: <https://faust.grame.fr/>
- Reaktor: <https://www.native-instruments.com/en/catalog/komplete/reaktor/>
- VCV Rack: <https://vcvrack.com/>
- Bitwig Grid: <https://www.bitwig.com/the-grid/>
- JUCE `AudioProcessorGraph`: <https://docs.juce.com/master/classjuce_1_1AudioProcessorGraph.html>
- JUCE `Synthesiser`: <https://docs.juce.com/master/classjuce_1_1Synthesiser.html>
- WebGPU: <https://www.w3.org/TR/webgpu/>
- Dawn: <https://dawn.googlesource.com/dawn/>
- RTNeural: <https://github.com/jatinchowdhury18/RTNeural>
- DDSP: <https://github.com/magenta/ddsp>

## Final disposition

The current graph roadmap is directionally correct where it hardens identity,
prepared publication, event transport, reusable composition, evidence, and
realtime boundaries. It is incomplete as a developer experience because those
contracts are still distributed across graph, MIDI, timeline, spectral, GPU, and
voice APIs.

The defensible next decision is to finish the current roadmap with explicit
partial/HOLD statuses, then run P3 and P1 experiments. If those experiments do
not show repeated integration cost, stop. If they do, add the smallest adapter
that binds existing primitives and preserves the immutable prepared snapshot.
The evidence should be allowed to invalidate this proposal before any broader
feature roadmap is accepted.
