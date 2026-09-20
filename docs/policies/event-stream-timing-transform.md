# Event-stream timing transform

Status: **RESOLVED for the stages Pulp owns. The reserved external tick-domain
stage has no seam, and two declared deviations are recorded below.**

A sequenced note reaches a sample position through more than one thing that may
move it. Authored groove displaces it in ticks. An external transform is
reserved a place to displace it in ticks. The tempo map converts it. A
humaniser may displace it again in samples. Delay compensation shifts the
window it is read from. Each of those is defensible alone; together they are a
pipeline, and a pipeline nobody wrote down is how a note gets swung twice.

This document is that pipeline. It answers which coordinate each stage owns,
which stage runs on which thread, what is deterministic and what the
determinism is keyed to, and what a zero-setting transform is actually allowed
to promise. It does not restate delay compensation, which is owned elsewhere
and named below.

## The contract

### The pipeline is two halves, not one list

**Stages 1 through 3 are compile-time. They run on the control thread, in
document ticks, and their output is the immutable `PlaybackProgram`. Stages 4
and 5 are render-time. They run on the audio thread, per `TransportRange`, and
they never rewrite program data.**

Reading the five as one flat list is the error this split exists to prevent. A
compile-time stage may move a note's authored position, because the program has
not been published yet and a moved note is simply what the document compiled
to. A render-time stage may not: `PlaybackProgram` is immutable and shared by
the offline and realtime paths, and a lowered event's sample position is
authoritative against the compiled tempo map. A render-time stage therefore
either emits a different event downstream of the program (stage 4) or changes
which window of the program is read (stage 5). Neither edits it.

The consequence worth stating plainly: **a compile-time displacement is a
property of the document and survives being saved, bounced, and re-opened. A
render-time displacement is a property of this playback and does not.**

### Stage 1 — authored groove and swing, in document ticks

The shipping stage is `timeline::GrooveTemplate`:
`apply_timing` (`core/timeline/src/sequence_context.cpp:217`) displaces a tick
position by swing plus the authored step table, and `velocity_scale_at`
(`:235`) scales the note's velocity. Both are invoked from the compiler at
`core/playback/src/program_compiler.cpp:791` and `:809`. The table is indexed
by the **authored** position rather than the swung one, so changing a swing
setting never re-assigns material to a different step.

**The contract binds `GrooveTemplate`, and `GrooveTemplate` does not validate
order.** `GrooveTemplate::create` bounds each step offset to the open interval
`(-step, +step)` (`sequence_context.cpp:188`) — a per-step bound, checked one
step at a time. It performs no joint validation across adjacent steps, so a
table whose neighbouring steps carry offsets near `+step` and `-step` can emit
two positions in the opposite order to the order they were authored in.

A stricter kernel exists and is not wired in. `timebase::OrderPreservingGrooveKernel`
(`core/timebase/include/pulp/timebase/groove_kernel.hpp`) walks every phase
boundary in `validate_order()` (`:187-212`) and rejects a table that reorders
with `GrooveKernelError::ReordersEvents`. Its own header states what it is: "not
the canonical, named, sequence-owned `GrooveTemplate`" but "a fixed-capacity
projection kernel for callers that require the stricter non-reordering subset"
(`:53-55`). **It has zero non-test consumers** — every occurrence under `core/`
is inside its own header.

So the honest statement of stage 1 is: **order preservation is not currently
guaranteed at stage 1.** A groove table that reorders events is accepted by the
authored model, compiles, and plays. Nothing downstream repairs it, and the
same-sample ordering rules the renderer applies are about events that already
landed together, not about two events that swapped. Wiring `GrooveTemplate`
through the order-preserving kernel, or giving it an equivalent joint check, is
what would make the stronger claim true; until then the weaker claim is the one
this document makes.

### Stage 2 — reserved, for an external tick-domain transform

A tick-domain, deterministic, bounded-displacement transform owned outside Pulp
is reserved a place here: after stage 1, before the clip clamp and the tempo-map
conversion. **No seam exists.** There is no interface, no registration point,
and no adapter, and this document reserves the position rather than describing
an implementation.

The insertion point is concrete: `program_compiler.cpp:791-809`, between
`groove.apply_timing(...)` and the tick-to-sample conversion at `:835-836`.
Anything landing there must satisfy two constraints that are not obvious from
the call site:

- **Its displacement bound must join `groove_timing_reach`.** That function
  (`core/playback/src/sequence_content_lowerer.cpp:84`) computes how far groove
  can move a note and is consumed at `:967` to widen the lowering window of a
  trimmed clip by exactly that reach. A transform whose displacement exceeds
  the groove reach has no widened window to live in, and its notes are **dropped
  at trim edges** rather than clamped or reported. A second displacement source
  that does not join this total is a silent truncation, not a refusal.
- **Dirty-tracking needs a new `CompileContextKind`.** The enum
  (`core/timeline/include/pulp/timeline/compile_context.hpp:46-57`) lists
  `Groove` at `:48`; a subscription is what makes a reader recompile when the
  thing it reads changes, and the registry resolves subscribers through
  `compile_context_registry.hpp:233` and `:238`. A transform that reads
  something no kind names is a transform whose output goes stale without
  anything noticing.

The enum already carries the shape such a kind would take: `CrossTrackRhythm`
(`:53-54`) is reserved as a peer-edge kind, and the note above the enum
(`:36-45`) states the constraint a peer edge inherits — the compile order is a
DAG, the DAG must stay acyclic, and whoever gives one of these kinds a producer
owns proving that acyclicity, because the subscription bitset records *what* a
reader reads and never *when* it may be compiled. A future tick-domain kind
should be designed against that reservation and that proof obligation. This is
the precedent for the shape, not a claim that `CrossTrackRhythm` is the kind.

### Stage 3 — tick to sample

`CompiledTempoMap::ticks_to_samples` converts the lowered tick position to a
sample position; for the authored-note path that is
`program_compiler.cpp:835-836`, and the compiler converts at several other
sites for controller lanes and registered content.

**This is the coordinate boundary. Everything before it is ticks and may move a
note musically. Everything after it is samples and must not.** A displacement
expressed musically after this point would shrink as tempo rises and would make
a tempo ramp change a transform that did not change — the same reason delay
compensation is counted in samples and never in ticks.

### Stage 4 — the optional humaniser, in samples

Pulp owns one executable event-to-event device: a MIDI humaniser, published as
`pulp.device.event.humanise`
(`core/host/include/pulp/host/timeline_device_resolver.hpp:18`), implemented at
`core/host/src/timeline_device_resolver.cpp:135-259` (`EventHumaniserSlot`).
It is an optional device in a track chain, not part of the compiler.

It operates in **samples**. `HumanizeSpec::timing_samples`
(`core/midi/include/pulp/midi/humanize.hpp:27`) is a sample count, and
`process()` takes a `timebase::SamplePosition` block start. Placing it after
stage 3 is therefore correct rather than incidental: it could not be placed
before stage 3 without being given a coordinate it does not speak.

Its draw is a pure hash, not a stream of random numbers. `draw_value`
(`humanize.hpp:237-244`) builds a `timebase::RandomCoordinate` from the event's
absolute position, its channel/note key, and a stream selector, and calls
`timebase::coordinate_random(seed, coordinate)`. There is no callback RNG and
no per-block state in the draw, so **the same authored note receives the same
jitter no matter how the host partitions its blocks.** That is proven rather
than asserted: *"humanize is invariant under block partition"* in
`test/test_midi_performance_kernels.cpp:375-396` renders the same input over a
whole block, small blocks, and ragged blocks and requires all three to be
identical. Timing and velocity use separate streams (`kTimingStream`,
`kVelocityStream`) so changing one depth cannot shift the other's draw.

> **Declared deviation — the device is keyed to stream position, not document
> position. Do not "fix" it here.**
>
> The kernel's partition invariance does not extend to the device wrapper. The
> wrapper's seed is the constant `kSeed` (`timeline_device_resolver.cpp:230`,
> used at `:243`) and the coordinate it hands the kernel is `position_`
> (`:181-182`), a **device-local frame counter** incremented by the block size
> on every `process()` call and reset only in `prepare()` and `release()`. The
> slot's `process()` carries no transport context, so a seek or a loop wrap does
> not reset it.
>
> **The consequence is that the same authored note draws a different
> displacement depending on where playback started and how many times it
> looped.** A note at bar 9 draws one value when the transport rolled from bar 1
> and a different one when it started at bar 8.
>
> **That is the contract as declared: the humaniser's draw is keyed to stream
> position, not to document position.** It is written down rather than smoothed
> over, because the alternative reading — that the humaniser is reproducible per
> document — is the one a later alignment proof would certify and it is false.
> Re-keying the draw to the event's document sample position would make the
> stronger promise true and would change what every existing render sounds
> like; it is a deliberate change, not a repair, and it is not made here.

### Stage 5 — event-domain delay compensation, owned elsewhere

**Stage 5 is owned entirely by [`event-stream-pdc.md`](event-stream-pdc.md).
This document adds nothing to it and deliberately does not restate it.**

That contract owns: the unit (samples at the compiled sample rate, never
ticks); the ceiling shared with `CustomNodeType::kMaxLatencySamples` = 65 535
samples, with an out-of-range device failing admission rather than being
clamped; control-thread discovery once at admission, cached into the immutable
prepared binding; hold-until-stop relatch when a latency changes mid-stream;
shift the query window and never the event data; application per
`TransportRange` and never per block; the loop-end read-ahead fold; and the
refusal of live input into a compensated event chain.

**Two overlapping latency policies is the exact failure shape that document
warns against** — a second total computed beside the first does not error, it
produces a plausible-looking wrong offset. So there is one total, it lives
there, and stage 5 here reads only: *event-domain device latency in samples,
per `event-stream-pdc.md`.*

Two wording traps are worth recording, because both names exist in this
codebase meaning something else:

- **`ProductionDeclaration::lookahead_ms` is wall-clock and is barred from any
  latency or delay-compensation computation.** Its own declaration says so:
  "Wall-clock milliseconds a producer is given to work ahead of the playhead.
  Zero for synchronous content. Never a latency figure."
  (`core/timeline/include/pulp/timeline/production_mode.hpp:91-93`). The
  compensation header states the reason from the other side — expressing a
  shift in ticks would shrink it as tempo rises, "which is the reason
  `timeline::ProductionDeclaration`'s lookahead is barred from reaching a
  delay-compensation computation in the first place"
  (`core/playback/include/pulp/playback/event_compensation.hpp:30-32`). A
  lookahead is time a producer is given to work, not time a signal is delayed
  by. Never describe a timing stage as declaring "causal lookahead".
- **"One-block delay" is a `SignalGraph` feedback-edge term, not an event-stream
  concept.** It names how a feedback connection reads its source's previous-block
  output slot (`core/format/include/pulp/format/graph_runtime_executor.hpp:644`).
  Nothing in the event pipeline delays by a block; the event shift is a sample
  count.

### One transform stage per coordinate

**Each coordinate gets exactly one timing-transform stage: one in ticks, one in
samples.** Stage 1 (with stage 2 reserved beside it as the external tick-domain
owner) is the tick-domain stage; stage 4 is the sample-domain stage. Stage 3 is
the conversion between them and displaces nothing.

This is the rule that keeps two engines from becoming two competing global
timing engines. An external tick-domain transform and the humaniser can
coexist precisely because they are not the same kind of thing operating in the
same coordinate: one is authored musical feel baked into the program, the other
is bounded per-performance variation applied to a stream. Adding a second
sample-domain displacement stage, or a second tick-domain one that does not
resolve into stage 2's reserved position, breaks the rule — and the failure is
the quiet kind, because two half-strength displacements look exactly like one
correct one until a table is changed and only one of them follows.

### Zero-transform identity

**"Byte identical" is the wrong claim for an event stream.** `MidiEvent` has no
`operator==`, and two buffers holding the same music can differ in capacity and
in how a sysex payload is stored. The claim would be untestable as stated.

The convention this repository actually uses is **exact equality of a projected
tuple, in emission order**. The projection is the event's absolute sample
position plus its decoded message fields, and it is exact because those are all
integers — no tolerance is involved. Both existing projections follow that
shape: `identity()` in `test/test_midi_performance_kernels.cpp:48-51` projects
`{sample, attack, release, channel, note, velocity}`, and `Emitted` in
`test/test_playback_event_pdc.cpp:89-94` projects `{sample, status, data1}`
with a defaulted `operator<=>`. A claim about velocity or about ordering needs
a projection that carries them; `Emitted` carries neither velocity nor buffer
order, so it cannot be used for those without being extended.

**Identity holds at the kernel spec, not at the device's depth-0 setting.**
`Humanize<>::is_identity` (`core/midi/include/pulp/midi/humanize.hpp:59-61`) is
true only when `timing_samples == 0` and `velocity_amount == 0` — that is the
kernel's declared bypass identity, and at it the kernel cannot change any
event.

The device never reaches that spec. `spec()`
(`timeline_device_resolver.cpp:233-246`) always reports
`timing_samples = kEventHumaniserWindowSamples` = **512**
(`timeline_device_resolver.hpp:27`) and expresses depth by moving
`minimum_timing_samples` inside that window, and `latency_samples()` (`:223`)
reports the same 512. **So the device at depth 0 is a fixed 512-sample delay,
not an identity.** Delay compensation cancels it audibly — the events land on
the authored sample — but the stream a depth-0 chain emits is not
tuple-identical to the stream a chain with no humaniser emits unless the
compensating shift is accounted for. A test asserting depth-0 identity must
either account for the 512 samples or assert the offset as the declared
contract.

**Audio identity is within-run only.** Where a render is compared, it is
compared against another render produced in the same run, through a test-local
`sample_hash` (`test/test_timeline_event_device_chain.cpp:66`). There is no
stored golden audio for this pipeline and none should be introduced: a stored
golden would pin the humaniser's stream-position keying, which this document
declares as a deviation rather than as a promise.

### Preserved refusals

Two compile-time refusals bound what a timing transform may assume about
structure. Both stay refused, and neither is relaxed by anything above.

- **`CompileErrorCode::NestedDeviceChainUnsupported`**
  (`core/playback/include/pulp/playback/program_compiler.hpp:193`), guarded at
  `core/playback/src/sequence_content_lowerer.cpp:750-752`: a nested sequence
  whose track carries a device chain is refused rather than flattened.
- **`CompileErrorCode::NestedAbsoluteChildUnsupported`** (`program_compiler.hpp:235`),
  guarded at `sequence_content_lowerer.cpp:853-855`: a nested child that is not
  musically anchored is refused, because flattening an absolute-anchored child
  onto a musical owner has no hybrid time domain to write into.

Each refusal names its own construct so the error says which one is missing.
**"Sub-bus" is not a code identifier** — it appears only as prose describing the
condition under which the first refusal could be lifted, and no symbol, error
code, or field carries that name.

## What remains open

- **Order preservation at stage 1.** `GrooveTemplate` bounds each step offset
  independently and validates no joint ordering; the kernel that does has no
  non-test consumer. Until one of those changes, a reordering table is
  accepted.
- **The stage 2 seam.** Reserved, unimplemented. Its two constraints — joining
  `groove_timing_reach` and owning a `CompileContextKind` — are stated so a
  later implementation does not discover them from a dropped note.
- **The humaniser's keying.** Stream position, declared above. Re-keying to
  document position is a behaviour change, not a fix.

## Acceptance

### The stages that exist are covered

Stage 1's compile-time application is covered by the program-compiler and
sequence-lowering suites, including the groove reach that widens a trimmed
clip's lowering window and the boundary case where groove pushes a note
entirely outside its clip so it contributes no audible span.

Stage 4's kernel is covered by `test/test_midi_performance_kernels.cpp`:
partition invariance over whole, small, and ragged block partitions with a
positive control that a differing stream is detected; the kernel's zero-amount
identity; and the window bound that keeps a jittered attack from outrunning its
own release.

Stage 5 is covered where it is owned, by the suites named in
[`event-stream-pdc.md`](event-stream-pdc.md); this document asserts nothing
about it.

### The two contract claims this document adds

Two claims above were not covered by an existing test. Each now has a dedicated
case in `test/test_timeline_event_device_chain.cpp`, tagged `[parity]` in the
unlabeled `pulp-test-timeline-graph-binding` suite, so both run on the required
lane.

**1. Zero-depth humaniser chain versus no-humaniser chain** — *"timeline
zero-depth humaniser chain lands every attack exactly where a chain without it
does"*.

The measured answer is neither of the two readings this section anticipated. The
two event kinds disagree, and the asymmetry is the finding:

| | no humaniser | zero-depth humaniser | delta |
|---|---|---|---|
| note-on | 600, 2600, 4600 (velocity 127) | 600, 2600, 4600 (velocity 127) | **0 — tuple-identical, velocity included** |
| note-off | 1800, 3800, 5800 | 1288, 3288, 5288 | **−512, exactly `kEventHumaniserWindowSamples`** |

**The kernel is attack-only.** It delays each attack by `minimum_timing_samples`,
which at depth 0 is the entire 512-sample window — exactly cancelling the
compensated early read, which is why attacks land tuple-identically. A release
is forwarded unchanged (`core/midi/include/pulp/midi/humanize.hpp:134`), so
nothing delays it back and it lands one full window early.

**The consequence: inserting the humaniser at depth 0 shortens every note by 512
samples** (10.7 ms at 48 kHz), because the note-on holds still while the note-off
moves earlier. Zero depth is a deterministic control setting and **not** render
identity — the stronger form of what the exposure ledger already records. A
caller wanting true bypass must omit the device, not zero its depths.

Causal control: removing the compensating addend from the scheduler
(`core/playback/src/note_renderer.cpp`, `process_shifted(block, transport,
latched_shift_)` → an empty `EventCompensationShift{}`). `confirm_failure.sh`
verdict **CONFIRMED (exit 0)**, recompile observed at baseline, broken and
restored.

**2. Exact-oracle single application** — *"timeline humaniser chain applies its
timing draw exactly once"*.

The one-stage-per-coordinate rule had no detector: the separation between stage 1
and stage 4 is structural only. The kernel deliberately exposes its pure
per-event draw (`jittered_position`, `humanize.hpp:78`), so the case predicts the
schedule without pushing events through it and asserts the landed position equals
the groove-lowered position displaced by exactly one draw. It carries a
seed-sensitivity control — a perturbed seed must disagree — so the oracle is
proven to read the seed rather than to match a value handed to it.

Causal control: duplicating the `humanise_.process(...)` statement in
`core/host/src/timeline_device_resolver.cpp` so the draw applies twice.
`confirm_failure.sh` verdict **CONFIRMED (exit 0)**, recompile observed in all
three phases.

Both controls were run against the `.cpp` call sites rather than the header
declarations. `confirm_failure.sh` deletes every object in the build directory
for a header edit, and the two `.cpp` breaks exercise the same behaviours at a
fraction of the rebuild cost. A header-path invocation remains valid but needs
`--object` pointing at a translation unit that includes the header, or the
recompile is never observed and the verdict is `INCONCLUSIVE` rather than a
result.
