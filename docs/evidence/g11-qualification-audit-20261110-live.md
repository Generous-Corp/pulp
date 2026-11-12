# G-11 live qualification audit

Date: 2026-11-10T10:13:38Z
Audited source: `origin/main` at `fb0ca2601cab3f082d72e4bba9f2aaf5675eb3a8`
Disposition: **PARTIAL / HOLD / NOT CHECKED**

This is an evidence-only refresh of the G-11 remainder. It records what the
current public tree proves and what it still cannot prove. It does not add a
runtime API, change the roadmap, or promote an existing offline receipt into a
realtime or device claim.

## Verified current primitives

- `core/audio/include/pulp/audio/realtime_sample_recorder.hpp:80-132` exposes
  fixed-capacity command/event queues, block-offset commands, callback-side
  `process()`, and explicit off-realtime materialization. `prepare()` owns
  storage capacity; reset/release and materialization are documented as
  non-concurrent with the callback.
- `core/audio/include/pulp/audio/sample_slot_bank.hpp:31-108` exposes a
  generation-aware publication bank. Publication and lifecycle mutation are
  single-owner control operations; audio readers consume immutable views and
  acknowledge generations.
- `core/audio/include/pulp/audio/loop_playback_cursor.hpp:26-49` provides
  allocation-free loop traversal and a bounded playback-rate setter. It is
  storage-independent and does not define graph event timing or persistence.
- `examples/g11-recording-publication/main.cpp` composes the public graph,
  recorder, slot bank, and playback path. Its regular/irregular block oracle
  and planted wrong-buffer/timing/stale-generation controls are deterministic
  offline evidence. Its README explicitly excludes device capture and
  realtime qualification.
- `examples/g11-tuning-cookbook` proves one fixed-note tuning lookup and its
  invalid/default-A4 controls. It does not expose sample-timed graph retuning.
- `examples/PulpSampler` has a richer format-`Processor` sampler, but it is
  not a graph-authored installed consumer and does not establish graph
  ingress, graph-owned persistence, or cross-platform graph parity.

## Remaining gates

| Requirement | Current evidence | Disposition |
| --- | --- | --- |
| Device or authenticated host live-input capture | Existing graph fixtures use generated buffers; no device/host receipt or provider-authenticated capture exists. | **NOT CHECKED** |
| Callback deadline/allocation/lock/wait/I/O campaign | Thread-boundary comments and fixed queues exist, but no paced 44.1/48/96 kHz × 32/64/128/256 campaign with p99 receipt exists. | **HOLD** |
| Sample-timed graph retuning | Tuning is prepare/authoring-side; no graph event/MIDI/MPE retune path is public and measured. | **HOLD** |
| Graph serialization/state migration | Recorder, slot generation, cursor, and publication state are runtime-owned; no prepared-graph round trip binds them. | **NOT CHECKED** |
| Non-Darwin installed parity | Retained receipts are Darwin arm64 source-install evidence; Linux/Windows/device providers are not exercised. | **NOT CHECKED** |
| Complete graph-authored sampler | No public graph consumer combines live publication, retuning, persistence, and independent negative controls as one product. | **PARTIAL / HOLD** |

The existing G-11 receipts remain valid only for their stated static,
tuning, sample-region, and recording/publication controls. They do not close
any row above. No current public seam authorizes implementing a new sampler
node or promoting `PulpSampler` internals into the graph ABI; doing so would
duplicate ownership before event, persistence, and platform contracts exist.

## Smallest defensible next experiment

Before implementation, specify one public graph-controlled publication and
retuning contract. Its installed consumer must provide: a host/device-neutral
input adapter without callback file I/O; sample-accurate start/stop/retune
events with refusal controls; a prepared-plan serialization round trip that
binds generation, cursor, and tuning identity; a paced callback matrix with
allocation/lock/wait counters and p99 deadline/jitter limits; independent
wrong-generation and wrong-retune negatives; and either a non-Darwin consumer
or an explicit provider/platform HOLD.

## Adversarial conclusion

- **DSP architect:** the callback/control split is plausible, but deadline and
  event-ordering behavior under a real host remain unmeasured.
- **Instrument developer:** the existing pieces are useful, but a developer
  still cannot build a graph-defined live sampler with persistent publication
  and retuning from the installed graph surface alone.
- **Framework maintainer:** promoting the format sampler into graph APIs now
  would create duplicate ownership and an ABI before the missing contracts are
  settled.

The correct current state is **PARTIAL / HOLD / NOT CHECKED**. This packet is
not implementation authorization and does not alter the roadmap disposition.
