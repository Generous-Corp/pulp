# Sampler Suite Hardening Plan

**Status:** active implementation plan

**Baseline:** Pulp `2fa9948ba` (v0.675.0)

**Scope:** CPU audio, storage, import, and verification paths

## Implementation ledger

Current branch evidence, without implying completion of later gates:

- S0 partial: bit-exact nonvacuous resident/sequential parity covers unaligned
  preload handoff, callback partitions, reset, channel mapping, and 10,000
  one-frame allocation-probed callbacks. Prepared-page coordinate parity covers
  canonical boundaries, forward/reverse integer steps, seeks, stale generations,
  and deterministic missing-page recovery.
- S1 partial: WAV and uncompressed AIFF/AIFF-C `NONE` retain mapped ranged
  readers. Codec capability is explicit, ranged reads are bounded, and preload
  admission uses the checked latency/rate/guard formula. Synchronized release
  and destruction tests prove that entered reader work is joined before its
  captured owner is destroyed. Ranged mapped bindings cooperatively observe a
  stop token between bounded chunks, while arbitrary and decode-once callbacks
  remain explicitly join-only rather than claiming prompt interruption.
- S2 partial: requester-aware scheduling, checked page-memory admission, shared
  page coalescing, explicit pressure, and generation-gated FIFO retirement/reuse
  are implemented in a deterministic caller-driven service. A bounded ordered
  RT command port carries demand and generation-qualified cancellation, and an
  immutable asset owner publishes trivial audio views under a checked preload
  contract and service-issued source-registration proof. Source collection is
  audio-watermark gated. An allocation-free linear forward voice reader plans
  coalesced page demand, crosses preload/page boundaries, and advances the
  current source timeline through explicit starvation. A fixed-count decode
  pool now provides bounded SPSC job/completion mailboxes, one in-flight decode
  per source, preallocated worker scratch, leased completion views, and
  cooperative or join-only teardown. The cache and decode pool are now joined
  by an owner-thread async service with exact registration/reservation tickets,
  queue-full retry without cross-source head-of-line blocking, stale-completion
  rejection, two-phase audio-watermark retirement, and decode-before-cache
  teardown. Release and AddressSanitizer coverage exercise registration
  rollback, reprepare identity, active-I/O retirement, slot recycling, and
  completion lease release. `PulpSampler` now exercises this path for strict
  ranged WAV/AIFF forward one-shots with contract-derived preload/lookahead,
  a bounded eight-voice cache working set, explicit starvation, coherent source
  replacement, and 10,000 allocation-probed callbacks. Streamed loop/reverse
  policy remains open. The resident loop state machine has now been extracted
  into a storage-independent, trivially copyable `LoopPlaybackCursor`; the
  existing renderer delegates to it without changing ordinary-step or
  crossfade output. Independent-oracle parity covers reverse entry, two-phase
  direction changes, high-rate fade-zone probes, multiple wraps/reflections,
  and short-loop residual travel. Stream admission also prepares the tail page
  before publication so reverse entry cannot begin on an uncached page;
  shutdown and timed-out in-flight admission roll back without leaking either
  source-registration slot. Wiring the shared cursor into the streamed voice
  reader and its lookahead scheduler remains open.
- The existing Release audio harness baseline and all 375 Audio Quality Lab
  self-tests pass. Quality Lab remains supplementary to exact transport,
  telemetry, lifetime, and allocation gates.

## Decision

Pulp's sampler hardening is CPU-first and CPU-complete. Live audio processing,
stream scheduling, interpolation, mip selection, and heritage modeling must not
depend on a GPU. An optional offline accelerator may be proposed only after a
representative CPU implementation is measured, the accelerated workload is
large enough to matter, and the proposal explains why established CPU methods
are insufficient. No GPU work is part of this plan.

This is not a greenfield sampler. Pulp already contains generation-safe sample
publication, sample pools and zones, voice rendering, loop rendering, prepared
stream pages, a preload-plus-ring streaming source, file codecs, RT-safety
contracts, and an offline audio-analysis harness. The project integrates and
hardens those pieces instead of introducing a parallel sampler stack.

## Current-state correction

The original proposal describes Pulp as a buffer player and assumes shared
oscillator mip infrastructure already exists. Neither assumption is current:

- `SampleVoiceRenderer`, `LoopRenderer`, `SampleZoneMap`, `SamplePool`, and the
  instrument runtime already cover much of resident polyphonic playback.
- `StreamingSampleSource` already owns a joinable reader thread, preload head,
  SPSC ring, deterministic manual-pump mode, underrun counters, and a resident
  fast path.
- `SampleStreamWindow` already provides prepared generation-qualified pages,
  but it is not connected to a multi-voice scheduler.
- The example `PulpSampler` remains an eight-voice, fully resident integration
  and does not exercise the streaming primitives.
- The oscillator suite's alias-analysis changes are uncommitted in a separate
  worktree. Its production mip representation has not been designed. Sampler
  asset mips therefore belong under `core/audio`; only stable analysis helpers
  are shared after they land.
- A separate live branch owns `core/signal/resampler.hpp`. Sampler work must not
  modify that file while the branch is active.

## Product boundary

The suite is built as reusable `pulp::audio` primitives plus an honest
`PulpSampler` example integration. Fixed-topology sampler DSP remains a
`Processor` concern; it is not expressed as a `SignalGraph`.

The runtime has four explicit layers:

1. **Immutable sample asset** — metadata, resident preload, optional mip assets,
   loop metadata, and generation-qualified backing-source identity. Control and
   worker code may retain owners; the callback uses prepared views/tokens and
   never performs shared-owner reference-count churn or final destruction.
2. **Voice reader** — source position, pitch ratio, interpolation policy, loop
   policy, starvation envelope, and immutable asset reference.
3. **Stream service** — a bounded worker pool and a shared prepared page cache
   per sample/source, scheduled by time-to-starvation. Voices do not each decode
   or cache duplicate pages. The service owns I/O and decode, never voice DSP.
4. **Sampler processor policy** — MIDI, zones, voice allocation/steal, envelope,
   parameters, and mixdown.

No layer may hide allocation, locks, file access, decoder calls, or worker waits
inside an audio-callback API.

## Ownership and collision rules

- Sampler-owned production work: `core/audio/**`, sampler example integration,
  sampler-specific tests, and sampler documentation.
- Consume the oscillator lane's stable spectrum/tone analysis APIs only after
  they land. Do not duplicate its least-squares tone projection or alias
  classifier.
- Do not edit the oscillator lane's in-flight generator/analyzer tests,
  `core/signal/osc/**`, or `wavetable.hpp`.
- Do not edit `core/signal/resampler.hpp` or `test/test_resampler.cpp` while the
  float-reassociation branch is active.
- Long-sample mip storage, persistence, budget accounting, and stream lifetime
  are sampler concerns even if an offline filter designer is later shared.

## Deliverables and gates

### S0 — Evidence harness

- A fully-resident reference reader and a streamed reader render the same
  source-coordinate schedule.
- The null comparison covers head-to-tail handoff, block partitions, pitch
  ratios, seeks, forward/reverse/short loops, crossfades, steal, and recovery.
- Throttled readers are deterministic and expose latency, throughput, and
  failure injection without sleeping in the audio callback.
- Every null, spectral, and click analyzer has known-good, known-bad, and
  detection-floor fixtures before it gates production DSP.
- Allocation counters require zero callback allocations. Worker decoders have
  explicit bounded high-water memory with no monotonic growth; allocator-free
  workers are required only where the codec contract can honestly provide it.
- Spectral gates declare stimulus, window, warm-up, estimator, seed, sample
  rate, and named tolerance.

The assertion domains stay separate:

- Raw float-bit comparison and `pulp audio validate compare --mode null` gate
  lossless transport, block independence, and resident/streamed equivalence.
- Explicit counters gate underruns, decode errors, stale generations, cache
  misses, and final source position. A perceptual result cannot waive a
  nonzero transport residual or service error.
- `RtAllocationProbe` gates prepared callback work across at least 10,000
  invocations. `ScopedNoAlloc` alone is not Release evidence.
- Audio Quality Lab profiles (`added-hf`, `transient-integrity`,
  `noise-roughness`, `graininess`, `stereo-width`, and `tonal-balance`) provide
  supplementary regression evidence over exported float WAV artifacts.
  Quality Lab alignment is inspection support, not latency proof.
- Strict alias-rejection thresholds wait for the oscillator lane's stable
  shared analyzer. Until then, independent-reference nulls are hard gates and
  generic spectrum/THD results are characterization only.

### S1 — Honest file-backed streaming

- The file adapter retains a mapped ranged reader rather than decoding the
  entire file into an `AudioFileData` object.
- Capability metadata distinguishes true ranged reads from decode-once
  fallback formats.
- Preload sizing is derived from certified I/O latency, scheduler margin,
  decoder latency, source rate, worst instantaneous source-consumption ratio,
  host-block guard, interpolation guard taps, and any loop/crossfade dual-region
  demand. Invalid contracts fail at prepare rather than gambling at note-on.
- Reader thread ownership is joinable and teardown is tested during active I/O.

### S2 — Multi-voice stream service

- `StreamingSampleSource` remains a narrow sequential utility; the production
  multi-voice path does not grow a one-thread/per-voice or one-ring/per-voice
  architecture around it.
- A bounded worker pool serves the shared page cache.
- Requests are prioritized by resident source frames divided by current source
  consumption rate, with stable tie-breaking for determinism.
- Prepared rings/pages come from a global memory governor; load admission fails
  before note-on when the configured budget cannot satisfy the contract.
- Voice steal cancels future work without invalidating immutable data currently
  referenced by another voice or worker.
- Streamed one-shots, forward/reverse loops, short loops, and crossfade loops
  prefetch every region needed at the boundary.

### S3 — Starvation behavior

- In-contract torture renders have zero underruns.
- Out-of-contract misses use a bounded power-preserving fade to silence and a
  bounded recovery fade. Missing frames are not replayed stale and recovery is
  click-gated.
- Telemetry distinguishes service starvation, decode failure, invalid preload
  contract, and end of source.
- Scheduling differences never change rendered audio when the requested source
  frames arrive within contract.

### S4 — Interpolation tiers

- Playback exposes nearest, linear, cubic Hermite, optimal polynomial, and
  ratio-tracking windowed-sinc policies without overloading loop semantics.
- Polynomial high-quality modes select persisted octave sample mips. They do
  not claim anti-alias performance without a suitably filtered source level.
- The sinc kernel scales its cutoff for source-consumption ratios above one and
  blends adjacent cutoff tables during modulation.
- Kernel tables and mip assets build off the callback and are immutable while
  voices read them.
- Constant input remains unity gain within a named tolerance, boundaries remain
  finite, and loop neighborhoods use the same indexing policy as resident
  interpolation.

### S5 — Quality and performance proof

- Swept known-tone tests report passband gain, wanted-tone residual, image
  level, and worst unexpected in-band component for ratios 0.25 through 4.0.
- Detection-floor and negative-control tests prove that the analyzer can see
  below each claimed threshold.
- Fixed-ratio renders compare against an independent offline reference without
  sharing the production kernel implementation.
- Pitch modulation covers 5–200 Hz and multiple block sizes.
- Per-tier CPU cost is reported per voice and at target polyphony on the named
  baseline machine. Budgets are ratcheted from measurements, not guessed.

### S6 — Heritage engine

- A typed, versioned profile schema with neutral public profile IDs describes
  stage order and parameters. Hardware/trademark names remain behavioral
  references in compatibility and research prose. JSON
  is validated into that type off-thread; the callback never parses data.
- Machine-domain conversion, quantization, clock/pitch family, DAC hold,
  reconstruction filtering, noise, and output stages are independently
  bypassable and testable.
- Bypassing every stage is transparent within the declared host-rate conversion
  contract.
- The clean host-to-machine and machine-to-host SRC stages meet the same
  production sinc gates, so host-rate folding cannot masquerade as hardware
  character.
- Deterministic dither/noise uses explicit seeds and serialized seed policy.

### S7 — Machine profiles and evidence

No named hardware profile ships from folklore. Each profile requires a cited
research note with exact or explicitly uncertain rate, coding law, pitch
mechanism, filter topology, and measurement procedure. Hardware names describe
behavioral targets; no ROM, firmware, or third-party source enters the repo.
Service-manual pages, schematics, factory samples, and uncleared captures are
not committed. Public CI uses synthetic fixtures; hardware captures remain in a
provenance-controlled private evidence pack.

Every profile requires:

- analytic image-position tests;
- reference captures with recorded hardware, firmware/revision, signal path,
  gain staging, and capture interface;
- level-matched spectral and noise-floor comparisons;
- a blind A/B pack and human sign-off; and
- a clean-room commit trailer for implementation commits.

Until supplied captures exist, the generic heritage engine can be built and
tested with synthetic profiles, but a hardware profile remains unverified and
must not be labeled capture-matched.

### S8 — Integration and documentation

- `PulpSampler` uses the production asset/voice/stream service rather than a
  private example-only playback stack.
- The example exposes interpolation quality, memory budget, preload policy,
  stream diagnostics, and deterministic seed state where relevant.
- WAV is the required ranged baseline. AIFF and FLAC must report honestly
  whether they range-decode or use a bounded/imported fallback.
- Public docs include thread ownership, memory formulas, supported loop modes,
  codec capability, performance results, and failure behavior.

## Sequencing

Work proceeds in vertical, independently provable increments:

1. Correct the file-backed adapter so today's streaming primitive performs a
   true ranged WAV read and reports fallback capability honestly.
2. Add the resident-versus-streamed null harness and preload-contract model.
3. Introduce the bounded multi-voice stream service and immutable sample asset,
   then integrate one-shot playback in `PulpSampler`.
4. Add loop-aware scheduling and starvation fades, followed by torture, churn,
   teardown, ASan, and TSan gates.
5. Add interpolation policy and sampler-owned mip assets without changing the
   in-flight core resampler surface.
6. Land ratio-tracking sinc and spectral/performance gates after consuming the
   oscillator analyzer seam from main.
7. Build the typed heritage engine with synthetic profiles.
8. Add named hardware profiles only as their research notes and captures meet
   the evidence bar.

Each increment must leave the CPU path useful, deterministic, documented, and
testable. A later increment may extend a stable seam but may not retroactively
make an earlier RT or lifetime claim true.

## Completion audit

"Built" means all S0–S8 gates applicable to shipped claims have direct current
evidence: source, focused tests, full Release build, full test suite, sanitizer
results, performance report, docs, and any required capture artifacts. A green
unit test for a primitive does not prove the example integration, multi-voice
torture contract, or hardware authenticity claim. Missing external captures
keep only the affected named profiles unshipped; they do not weaken generic
engine gates.
