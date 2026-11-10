# G-11 remainder audit: live sampling, retuning, and parity

Date: 2026-11-10  
Audited source: `origin/main` at `ba04406f050258968f64d497fd6e80937618da68`  
Disposition: **PARTIAL / HOLD / NOT CHECKED**

This packet is an evidence-only audit of the G-11 remainder. It does not add a
runtime API, upgrade the roadmap, or claim a complete sampler. The source SHA
was recorded before this document was written; if `origin/main` advances, this
packet is historical until refreshed.

## Verified current capabilities

The current tree contains the following public, reusable primitives:

- `core/audio/include/pulp/audio/realtime_sample_recorder.hpp:80-132`
  exposes a bounded recorder with fixed command/event queues, callback-side
  `process()`, block-offset commands, and off-real-time materialization helpers.
  `prepare()` owns capacity setup; `process()` is `noexcept` and the header
  explicitly forbids calling reset/release concurrently with the callback.
- `core/audio/include/pulp/audio/sample_slot_bank.hpp:31-108` defines a fixed
  capacity publication bank. Publication and lifecycle mutation are serialized
  on a control/background thread; audio readers consume generation-aware views.
  `core/audio/include/pulp/audio/sample_slot_materializer.hpp:11-25` keeps the
  completed-recording publication helper off the realtime path.
- `core/audio/include/pulp/audio/loop_playback_cursor.hpp:26-60` provides
  allocation-free loop traversal and a playback-rate setter. It is a storage
  independent cursor, not a graph event or state-serialization contract.
- `examples/g11-recording-publication/main.cpp:48-205` composes a real
  `SignalGraph` with `RealtimeSampleRecorder`, `SampleSlotBank`, and a playback
  processor. Its positive path compares every captured/played sample for regular
  and irregular blocks; its planted controls at lines 286-325 exercise timing,
  wrong-buffer, and stale-publication defects. The README explicitly bounds this
  as offline evidence without device capture or realtime qualification.
- `core/midi/include/pulp/midi/tuning.hpp:15-80` and
  `examples/g11-tuning-cookbook/main.cpp:109-138` support a static
  `EqualTemperamentTuningProvider` lookup. The cookbook proves A4=442 Hz and
  32/17-frame partition parity for one rendered note, with invalid-note and
  planted-default-A4 controls.
- `examples/PulpSampler/pulp_sampler.hpp:49-60` and its streaming/heritage
  tests contain a richer format-`Processor` sampler with voices, streaming,
  clock automation, and state tests. Those tests are valuable existing
  primitives, but this example is not a graph-authored public consumer and does
  not establish graph ingress or graph serialization.

These findings agree with the existing G-11 status and installed-SDK receipts;
no source claim was inferred from a planning statement alone.

## Remaining gates

| Capability | Current evidence | Disposition |
| --- | --- | --- |
| Device or host live-input capture | The G-11 graph fixture uses generated `Buffer` input in its own harness. No device/host capture receipt is present. | **NOT CHECKED** |
| Callback deadline, allocation, lock, wait, and I/O matrix | Recorder and bank comments define thread boundaries, but no 44.1/48/96 kHz × 32/64/128/256 paced campaign or p99 receipt exists. | **HOLD** |
| Sample-timed graph retuning | The tuning fixture performs an authoring/prepare-side lookup and renders one fixed note. No graph event/MIDI/MPE retune path is exposed or measured. | **HOLD** |
| Graph serialization and state migration | Sample publication and recorder state are owned by runtime objects; the fixture does not serialize or restore them through a prepared graph. | **NOT CHECKED** |
| Cross-platform installed-consumer parity | Current G-11 receipt is a Darwin arm64 source install. Linux/Windows and device backends are not exercised. | **NOT CHECKED** |
| Complete sampler product | The separate `PulpSampler` format processor has broader tests, but no graph-authored polyphonic sampler with live publication, retuning, persistence, and negative controls exists. | **PARTIAL / HOLD** |

The existing exact-head receipts remain valid for their bounded claims: static
stereo/tuning and deterministic sample-region or recording/publication behavior.
They must not be promoted to any row above.

## Smallest defensible follow-up

Before implementation, design one named public contract for graph-controlled
sample publication and retuning. A future evidence slice should use a real
installed consumer and require all of the following before admission:

1. a host/device-independent input adapter or an explicit authenticated host
   fixture, with no callback file I/O;
2. sample-accurate start/stop/retune events and a documented refusal for
   unsupported event forms;
3. a serialized prepared-plan state round trip that binds sample generation,
   cursor position, and tuning identity;
4. a paced callback matrix with allocation/lock/wait counters, p99 deadline
   and jitter limits, and a planted positive plus independent wrong-generation
   and wrong-retune negatives; and
5. at least one non-Darwin installed consumer, or an explicit platform HOLD
   with the missing provider and next experiment named.

The existing recorder, slot-bank generation protocol, loop cursor, and tuning
lookup should be reused if those contracts are approved. Adding a second
publication bank, a speculative graph sampler node, or a broad tuning system
without these acceptance gates would duplicate existing primitives and would
not close G-11.

## Adversarial conclusion

- **DSP architect:** the callback/control split is technically plausible, but
  the current receipts do not measure deadline behavior or sample-timed event
  ordering under a real host.
- **Instrument developer:** the recipes demonstrate useful pieces, yet a
  developer still cannot build a graph-defined live sampler with persistent
  publication and retuning from the installed graph surface alone.
- **Framework maintainer:** promoting the existing `PulpSampler` internals into
  graph APIs now would duplicate ownership and create an ABI before the event,
  serialization, and platform contracts are settled.

No implementation or roadmap change is authorized by this audit. The correct
current state is **PARTIAL / HOLD / NOT CHECKED** until the named experiments
produce current exact-head evidence.
