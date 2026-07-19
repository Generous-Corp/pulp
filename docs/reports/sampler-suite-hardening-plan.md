# Sampler Suite Hardening Plan

**Status:** implementation and local landing validation complete; PR landing pending

**Baseline:** Pulp `2fa9948ba` (v0.675.0)

**Scope:** CPU audio, storage, import, and verification paths

## Implementation ledger

Current branch evidence:

- S0-S2: exact resident/streamed parity, bounded ranged WAV and uncompressed
  AIFF admission, checked preload contracts, immutable publication, shared
  page caching, two decode workers, generation-safe retirement, reverse-entry
  prewarm, loop-aware lookahead, rollback, teardown, and callback-allocation
  probes are implemented. `PulpSampler` exercises the production-shaped path
  with eight independently positioned voices.
- S3: the paged readers distinguish ready, service starvation, decode failure,
  invalid contract, stale generation, and normal end. A 64-frame equal-power
  fade to silence and 64-frame recovery ramp shape misses; focused tests cover
  predicted, emergency, recovery, and held-cursor behavior.
- S4: hold, nearest, linear, cubic Hermite, cubic Lagrange, and ratio-tracking
  Kaiser-sinc policies share prepared resident/paged footprints. Exact-octave
  polynomial forward one-shots can select immutable resident mips or strict
  persisted streamed `.pulpmip` sidecars; other traversal/rate cases fall back
  to ratio-tracking sinc. The offline `pulp audio sampler-mip build` command
  atomically publishes hash-addressed levels using the 140 dB-design decimator.
- S5: focused C++ interpolation gates and dependency-bearing Python reference
  and negative-control gates exist. The current checked-in Apple M3 Ultra Release
  capture contains the complete 108-row matrix and passes its full source,
  binary, budget, environment, and negative-control verifier. Its
  content-addressed interpolation source bundle remains current across later
  sampler-integration-only commits.
- S6: the versioned neutral heritage schema, strict canonical JSON, validated
  prepared profile, ordered independently bypassable stages, exact causal
  two-leg SRC, deterministic RNG policies, bounded runtime RNG-state capture,
  example integration, latency reporting, fail-closed diagnostics, and an
  independent Audio Quality Lab reference/negative-control gate are present.
  The example's strict versioned profile/runtime envelope is wired through
  outer plugin state. Callback-end `SeqLock` publication supplies same-rate RNG
  continuation before the next session's first callback; host-rate changes
  preserve the profile but reset runtime state and DSP transients. Streaming is
  prepared in the `host rate * active clock ratio` domain with the heritage
  maximum input block. Per-voice admission caps `pitch * clock` at 4x, and
  aggregate source throughput is `sum(pitch * source rate) * clock`; heritage
  diagnostics distinguish clock-caused note admission from live automation
  rejection.
- S7 remains intentionally unshipped. There are no named hardware profiles or
  capture-matched claims; those still require the research, provenance,
  measurement, listening, and clean-room gates below.
- S8 is implemented and locally validated. This documentation
  describes current integration and limitations. The processor exposes its
  pre-prepare streaming-memory configuration, typed prepare and file-load
  results, last-load result, stream statistics, heritage controls and state,
  and one coherent combined diagnostic record covering prepare, load, preload,
  starvation envelopes, heritage, and streaming-memory use. Control-plane
  publication is serialized so concurrent loads, rebinds, release, and
  diagnostic reads cannot expose a destroyed runtime or a mixed publication.

Quality Lab remains supplementary to exact transport, telemetry, lifetime,
allocation, and current artifact-verification gates.

## Final local landing evidence

After the final rebase, the final runtime source revision is
`f08669e06aa05359efea494313b21f0ce249f5b3`. The checked benchmark artifact
identifies that source revision explicitly and passes supplied-binary,
source-only, and self-test verification.

The final source revision passed:

- all 25 focused Release binaries, all 25 ASan binaries with
  `detect_leaks=0:halt_on_error=1`, and all 25 TSan binaries with
  `halt_on_error=1`;
- the Debug+coverage bounded decode-pool regression;
- `pulp-sampler-test` with 4,255 assertions in 145 cases;
- the post-rebase default-GPU Release build; 14,551 of 14,552 configured CTests
  passed in the aggregate run, and the sole failure exposed a test-only
  reclamation predicate that observed cache teardown before ownership-slot
  teardown. After the predicate was fixed, that case passed 100 consecutive
  processes and the complete sampler binary passed all 4,255 assertions;
- the explicit audio-harness slice, 789 of 789 tests;
- the pinned Audio Quality Lab Python suite, 599 passed and 42 skipped, and all
  four configured sampler/heritage quality gates;
- all six applicable advisory AQL axes, with the graininess negative control
  producing `regression_suspected` as required;
- the block-partition null at `-inf dBFS` and the wrong-ratio negative control
  at `-0.1 dBFS`;
- the sampler-mip shellout, 119 assertions in two cases, and its CLI help test.

Three successive exact Ultra review cycles found and drove fixes for decode
worker wakeup/cancellation, immutable mapped-source snapshots, scheduler tie
handling, stream-registration integrity, heritage stream-domain rebinding, and
shared-worker throughput admission. A subsequent exact Ultra closure review of
the complete pre-publication branch found no actionable correctness
regressions. The same command is repeated against the final post-rebase
publication commit before the PR is opened.

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
- `SampleStreamWindow` now feeds the bounded multi-voice cache scheduler and
  allocation-free paged loop reader built in S2.
- `PulpSampler` now exercises ranged file admission, shared paged streaming,
  reverse/crossfade traversal, and the prepared CPU interpolation tiers across
  its eight voices.
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

- Playback exposes nearest, linear, cubic Hermite, a specifically defined and
  tested high-quality polynomial tier, and
  ratio-tracking windowed-sinc policies without overloading loop semantics.
- The current four-point Lagrange tier remains named Lagrange. It must not be
  relabeled "optimal" unless an approximation objective, order, coefficient
  derivation, and independent error fixture are selected and pinned.
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

## Landing validation matrix

Run this matrix from one final integrated revision. Focused success cannot
substitute for the full Release suite, and ordinary pytest skips cannot
substitute for the configured Audio Quality Lab gates.

### Focused Release and sanitizers

```bash
sampler_targets=(
  pulp-sampler-test
  pulp-test-audio-file
  pulp-test-mmap-reader-ranged
  pulp-test-streaming-sample-source
  pulp-test-loop-rendering
  pulp-test-sampler-stream-parity
  pulp-test-sampler-loop-oracle
  pulp-test-sampler-loop-cursor-parity
  pulp-test-sampler-paged-loop-oracle
  pulp-test-sample-stream-service
  pulp-test-sample-stream-async-service
  pulp-test-sample-stream-decode-pool
  pulp-test-sample-stream-window
  pulp-test-sample-stream-voice-reader
  pulp-test-sample-stream-loop-voice-reader
  pulp-test-sample-voice-renderer
  pulp-test-sample-sinc-kernel
  pulp-test-sample-asset
  pulp-test-sample-memory-governor
  pulp-test-sample-heritage
  pulp-test-sample-heritage-json
  pulp-test-sample-starvation-envelope
  pulp-test-sample-interpolation-quality
  pulp-test-sampler-rt-safety-contract
  pulp-test-dsp-runtime-no-alloc
)

sampler_binaries=(
  examples/PulpSampler/pulp-sampler-test
  test/pulp-test-audio-file
  test/pulp-test-mmap-reader-ranged
  test/pulp-test-streaming-sample-source
  test/pulp-test-loop-rendering
  test/pulp-test-sampler-stream-parity
  test/pulp-test-sampler-loop-oracle
  test/pulp-test-sampler-loop-cursor-parity
  test/pulp-test-sampler-paged-loop-oracle
  test/pulp-test-sample-stream-service
  test/pulp-test-sample-stream-async-service
  test/pulp-test-sample-stream-decode-pool
  test/pulp-test-sample-stream-window
  test/pulp-test-sample-stream-voice-reader
  test/pulp-test-sample-stream-loop-voice-reader
  test/pulp-test-sample-voice-renderer
  test/pulp-test-sample-sinc-kernel
  test/pulp-test-sample-asset
  test/pulp-test-sample-memory-governor
  test/pulp-test-sample-heritage
  test/pulp-test-sample-heritage-json
  test/pulp-test-sample-starvation-envelope
  test/pulp-test-sample-interpolation-quality
  test/pulp-test-sampler-rt-safety-contract
  test/pulp-test-dsp-runtime-no-alloc
)

cmake -S . -B build-sampler-release -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON -DPULP_ENABLE_GPU=OFF
tools/ci/governed-build.sh cmake --build build-sampler-release \
  --target "${sampler_targets[@]}"
grep '^CMAKE_BUILD_TYPE' build-sampler-release/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-sampler-release/examples/PulpSampler/CMakeFiles/pulp-sampler-test.dir/flags.make
for binary in "${sampler_binaries[@]}"; do
  "./build-sampler-release/$binary"
done

cmake -S . -B build-sampler-asan -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON -DPULP_ENABLE_GPU=OFF \
  -DPULP_TEXT_SHAPING=OFF \
  -DPULP_SANITIZER=address
tools/ci/governed-build.sh cmake --build build-sampler-asan \
  --target "${sampler_targets[@]}"
grep '^CMAKE_BUILD_TYPE' build-sampler-asan/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-sampler-asan/examples/PulpSampler/CMakeFiles/pulp-sampler-test.dir/flags.make
for binary in "${sampler_binaries[@]}"; do
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    "./build-sampler-asan/$binary"
done

cmake -S . -B build-sampler-tsan -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON -DPULP_ENABLE_GPU=OFF \
  -DPULP_TEXT_SHAPING=OFF \
  -DPULP_SANITIZER=thread
tools/ci/governed-build.sh cmake --build build-sampler-tsan \
  --target "${sampler_targets[@]}"
grep '^CMAKE_BUILD_TYPE' build-sampler-tsan/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-sampler-tsan/examples/PulpSampler/CMakeFiles/pulp-sampler-test.dir/flags.make
for binary in "${sampler_binaries[@]}"; do
  TSAN_OPTIONS=halt_on_error=1 \
    "./build-sampler-tsan/$binary"
done
```

The decode-pool concurrency regression must also pass in the configuration that
previously exposed the stall: Debug with coverage enabled. The 30-second CTest
timeout turns a recurrence into a bounded failure rather than a wedged landing
job.

```bash
cmake -S . -B build-sampler-coverage -DCMAKE_BUILD_TYPE=Debug \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON \
  -DPULP_ENABLE_GPU=OFF -DPULP_TEXT_SHAPING=OFF \
  -DPULP_ENABLE_COVERAGE=ON
tools/ci/governed-build.sh cmake --build build-sampler-coverage \
  --target pulp-test-sample-stream-decode-pool
ctest --test-dir build-sampler-coverage \
  -R '^Sample stream decode pool bounds cross-source concurrency by workers$' \
  --timeout 30 --output-on-failure
```

### Full Release and audio harness

```bash
cmake -S . -B build-final-release -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON
tools/ci/governed-build.sh cmake --build build-final-release
grep '^CMAKE_BUILD_TYPE' build-final-release/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-final-release/examples/PulpSampler/CMakeFiles/pulp-sampler-test.dir/flags.make
tools/ci/governed-build.sh ctest --test-dir build-final-release \
  --output-on-failure

tools/ci/governed-build.sh cmake --build build-final-release --target \
  pulp-test-audio-support pulp-test-render-scenario pulp-test-audio-contracts \
  pulp-test-audio-doctor pulp-test-adapter-audio-parity pulp-test-golden \
  pulp-test-audio-matrix pulp-test-audio-tone-regression \
  pulp-test-latency-contract
tools/ci/governed-build.sh ctest --test-dir build-final-release \
  -R 'audio|golden|render|contract|doctor' --output-on-failure
```

Configure the quality-lab renderer before the sampler-specific null commands
that consume it. Pinning the oscillator renderer also prevents the Python suite
from selecting a stale binary in another build directory.

```bash
python3 -m venv .venv-aql
.venv-aql/bin/python -m pip install --upgrade pip
.venv-aql/bin/python -m pip install -e 'tools/audio/quality-lab[test]'

cmake -S . -B build-final-aql -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_AUDIO_QUALITY_LAB_GATE=ON \
  -DPULP_AUDIO_QUALITY_LAB_PYTHON="$PWD/.venv-aql/bin/python"
tools/ci/governed-build.sh cmake --build build-final-aql --target \
  pulp-osc-render-wav pulp-sampler-render-wav \
  pulp-sampler-heritage-render-wav
grep '^CMAKE_BUILD_TYPE' build-final-aql/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-final-aql/test/CMakeFiles/pulp-sampler-render-wav.dir/flags.make
```

The generic harness does not exercise the sampler renderer. Preserve its
block-partition invariant as a sampler-specific null gate, with a same-length
wrong-ratio negative control that proves the gate can fail:

```bash
mkdir -p build-final-evidence/sampler
./build-final-aql/test/pulp-sampler-render-wav \
  --source-out build-final-evidence/sampler/source-block-1.wav \
  --candidate-out build-final-evidence/sampler/ratio-sinc-block-1.wav \
  --policy ratio-sinc --ratio 1.5 --source-frequency 0.0211995442708333 \
  --frames 16385 --block-size 1
./build-final-aql/test/pulp-sampler-render-wav \
  --source-out build-final-evidence/sampler/source-block-257.wav \
  --candidate-out build-final-evidence/sampler/ratio-sinc-block-257.wav \
  --policy ratio-sinc --ratio 1.5 --source-frequency 0.0211995442708333 \
  --frames 16385 --block-size 257
./build-final-aql/test/pulp-sampler-render-wav \
  --source-out build-final-evidence/sampler/source-wrong-ratio.wav \
  --candidate-out build-final-evidence/sampler/ratio-sinc-wrong-ratio.wav \
  --policy ratio-sinc --ratio 1.25 --source-frequency 0.0211995442708333 \
  --frames 16385 --block-size 257

# Positive control: block partitioning must null at the stock tolerance.
./build-final-release/tools/cli/pulp-cpp audio validate compare \
  build-final-evidence/sampler/ratio-sinc-block-1.wav \
  build-final-evidence/sampler/ratio-sinc-block-257.wav --mode null

# Negative control: same frame count, intentionally wrong playback ratio.
if ./build-final-release/tools/cli/pulp-cpp audio validate compare \
  build-final-evidence/sampler/ratio-sinc-block-1.wav \
  build-final-evidence/sampler/ratio-sinc-wrong-ratio.wav --mode null; then
  echo 'sampler wrong-ratio negative control unexpectedly passed' >&2
  exit 1
fi
```

### Audio Quality Lab, CLI, and benchmark evidence

Run the Python and configured CTest gates against the pinned renderer above.

```bash
PULP_OSC_RENDER_WAV="$PWD/build-final-aql/test/pulp-osc-render-wav" \
  .venv-aql/bin/python -m pytest tools/audio/quality-lab/tests -q
tools/ci/governed-build.sh ctest --test-dir build-final-aql \
  -R '^sampler-(quality-lab|heritage-quality-lab)-' --output-on-failure
```

The configured renderer-dependent tests remain the normative spectral and
heritage oracles. In addition, record one advisory report for every named AQL
axis. Tonal balance, added HF, and noise roughness use a high-frequency sampler
render. Graininess needs sustained spectral evolution to make its flux detector
applicable, so it uses a deterministic time-varying mix of three independently
rendered sampler tones. A single stationary tone is not valid graininess
evidence. Transient integrity uses a train assembled from repeated sampler
heritage impulse renders, because that axis intentionally refuses material with
too few onsets. The stereo axis uses a declared deterministic right-channel
delay around the sampler-exported mono signal because this test renderer is
mono; it proves preservation of that stereo mapping, not independent
stereo-voice rendering.

```bash
./build-final-aql/test/pulp-sampler-render-wav \
  --source-out build-final-evidence/sampler/aql-tone-source-1.wav \
  --candidate-out build-final-evidence/sampler/aql-tone-block-1.wav \
  --policy ratio-sinc --ratio 1.5 --source-frequency 0.1875 \
  --frames 16385 --block-size 1
./build-final-aql/test/pulp-sampler-render-wav \
  --source-out build-final-evidence/sampler/aql-tone-source-257.wav \
  --candidate-out build-final-evidence/sampler/aql-tone-block-257.wav \
  --policy ratio-sinc --ratio 1.5 --source-frequency 0.1875 \
  --frames 16385 --block-size 257

for profile in tonal-balance added-hf noise-roughness; do
  .venv-aql/bin/python -m quality_lab.cli compare \
    build-final-evidence/sampler/aql-tone-block-1.wav \
    build-final-evidence/sampler/aql-tone-block-257.wav \
    --profile "$profile" --reference-role golden \
    --json "build-final-evidence/sampler/aql-$profile.json"
done

for source_frequency in 0.03125 0.0729166666666667 0.1145833333333333; do
  key=${source_frequency//./_}
  for block in 1 257; do
    ./build-final-aql/test/pulp-sampler-render-wav \
      --source-out \
        "build-final-evidence/sampler/grain-source-$key-block-$block.wav" \
      --candidate-out \
        "build-final-evidence/sampler/grain-tone-$key-block-$block.wav" \
      --policy ratio-sinc --ratio 1.5 \
      --source-frequency "$source_frequency" --frames 65537 \
      --block-size "$block"
  done
done

./build-final-aql/test/pulp-sampler-heritage-render-wav \
  --source-out build-final-evidence/sampler/aql-impulse-source-64.wav \
  --candidate-out build-final-evidence/sampler/aql-impulse-block-64.wav \
  --input-ratio 0.75 --return-ratio 1.5 --impulse-index 2048 \
  --frames 8192 --block-size 64
./build-final-aql/test/pulp-sampler-heritage-render-wav \
  --source-out build-final-evidence/sampler/aql-impulse-source-257.wav \
  --candidate-out build-final-evidence/sampler/aql-impulse-block-257.wav \
  --input-ratio 0.75 --return-ratio 1.5 --impulse-index 2048 \
  --frames 8192 --block-size 257
.venv-aql/bin/python - <<'PY'
from pathlib import Path
import numpy as np
import soundfile as sf
from quality_lab import generate

root = Path("build-final-evidence/sampler")
keys = ("0_03125", "0_0729166666666667", "0_1145833333333333")
modulation_rates = (3.1, 4.7, 6.3)
phases = (0.0, 0.7, 1.3)
for block in (1, 257):
    tones = []
    rate = None
    for key in keys:
        tone, tone_rate = sf.read(root / f"grain-tone-{key}-block-{block}.wav",
                                  dtype="float32")
        rate = tone_rate if rate is None else rate
        assert tone_rate == rate
        tones.append(tone)
    time = np.arange(len(tones[0]), dtype=np.float64) / rate
    mix = np.zeros_like(tones[0], dtype=np.float64)
    for tone, modulation_rate, phase in zip(
            tones, modulation_rates, phases):
        weight = 0.6 + 0.4 * np.sin(
            2.0 * np.pi * modulation_rate * time + phase)
        mix += weight * tone
    mix = (mix / len(tones)).astype(np.float32)
    sf.write(root / f"aql-graininess-block-{block}.wav",
             mix, rate, subtype="FLOAT")
    if block == 257:
        sf.write(root / "aql-graininess-negative.wav",
                 generate.grainy(mix, rate, amount=0.18),
                 rate, subtype="FLOAT")
for block in (1, 257):
    mono, rate = sf.read(root / f"aql-tone-block-{block}.wav", dtype="float32")
    right = np.zeros_like(mono)
    right[17:] = mono[:-17] * np.float32(0.8)
    sf.write(root / f"aql-stereo-block-{block}.wav",
             np.column_stack((mono, right)), rate, subtype="FLOAT")
for block in (64, 257):
    impulse, rate = sf.read(root / f"aql-impulse-block-{block}.wav",
                            dtype="float32")
    sf.write(root / f"aql-transient-train-{block}.wav",
             np.tile(impulse, 4), rate, subtype="FLOAT")
PY
.venv-aql/bin/python -m quality_lab.cli compare \
  build-final-evidence/sampler/aql-graininess-block-1.wav \
  build-final-evidence/sampler/aql-graininess-block-257.wav \
  --profile graininess --reference-role golden \
  --json build-final-evidence/sampler/aql-graininess.json
.venv-aql/bin/python -m quality_lab.cli compare \
  build-final-evidence/sampler/aql-graininess-block-1.wav \
  build-final-evidence/sampler/aql-graininess-negative.wav \
  --profile graininess --reference-role golden \
  --json build-final-evidence/sampler/aql-graininess-negative.json
.venv-aql/bin/python -m quality_lab.cli compare \
  build-final-evidence/sampler/aql-stereo-block-1.wav \
  build-final-evidence/sampler/aql-stereo-block-257.wav \
  --profile stereo-width --reference-role golden \
  --json build-final-evidence/sampler/aql-stereo-width.json
.venv-aql/bin/python -m quality_lab.cli compare \
  build-final-evidence/sampler/aql-transient-train-64.wav \
  build-final-evidence/sampler/aql-transient-train-257.wav \
  --profile transient-integrity --reference-role golden \
  --json build-final-evidence/sampler/aql-transient-integrity.json

.venv-aql/bin/python - <<'PY'
import json
from pathlib import Path

root = Path("build-final-evidence/sampler")
profiles = (
    "tonal-balance", "added-hf", "noise-roughness", "graininess",
    "stereo-width", "transient-integrity",
)
for profile in profiles:
    report = json.loads((root / f"aql-{profile}.json").read_text())
    assert report["schema"] == "quality_lab.compare.v1", profile
    assert report["verdict"] == "no_material_change_detected", profile
    assert report["measurements"][0]["applicable"] is True, profile

negative = json.loads((root / "aql-graininess-negative.json").read_text())
assert negative["schema"] == "quality_lab.compare.v1"
assert negative["verdict"] == "regression_suspected"
assert negative["measurements"][0]["applicable"] is True
assert negative["measurements"][0]["materiality"]["exceeds"] is True
PY

# CLI and benchmark evidence continue after the six applicable AQL reports.
```

```bash

tools/ci/governed-build.sh cmake --build build-final-release --target \
  pulp-cli pulp-test-cli-shellout
./build-final-release/test/pulp-test-cli-shellout '[sampler-mip]'
tools/ci/governed-build.sh ctest --test-dir build-final-release \
  -R '^cli-audio-sampler-mip-help$' --output-on-failure

cmake -S . -B build-final-benchmark -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF
tools/ci/governed-build.sh cmake --build build-final-benchmark \
  --target pulp-sampler-interpolation-benchmark
grep '^CMAKE_BUILD_TYPE' build-final-benchmark/CMakeCache.txt
grep '^CXX_FLAGS ' \
  build-final-benchmark/test/CMakeFiles/pulp-sampler-interpolation-benchmark.dir/flags.make
PULP_BENCHMARK_CAPTURED_UTC=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
PULP_BENCHMARK_SOURCE_REVISION=$(git rev-parse HEAD)
PULP_BENCHMARK_SOURCE_SHA256=$(python3 \
  tools/scripts/verify_sampler_interpolation_benchmark.py \
  --print-source-bundle-sha256)
PULP_BENCHMARK_BINARY_SHA256=$(shasum -a 256 \
  build-final-benchmark/test/pulp-sampler-interpolation-benchmark | \
  awk '{print $1}')
./build-final-benchmark/test/pulp-sampler-interpolation-benchmark \
  --machine-label "Apple M3 Ultra Mac15,14" \
  --machine-model "Mac Studio Mac15,14, Apple M3 Ultra" \
  --os "macOS 26.5.2 build 25F84" --architecture arm64 \
  --compiler "Apple clang 21.0.0 (clang-2100.1.1.101)" \
  --source-base-revision "$PULP_BENCHMARK_SOURCE_REVISION" \
  --source-bundle-sha256 "$PULP_BENCHMARK_SOURCE_SHA256" \
  --benchmark-binary-sha256 "$PULP_BENCHMARK_BINARY_SHA256" \
  --generated-utc "$PULP_BENCHMARK_CAPTURED_UTC" \
  > docs/validation/sampler-interpolation/apple-m3-ultra-mac15-14.release.json
python3 tools/scripts/verify_sampler_interpolation_benchmark.py \
  --benchmark-binary \
  build-final-benchmark/test/pulp-sampler-interpolation-benchmark
python3 tools/scripts/verify_sampler_interpolation_benchmark.py --source-only
python3 tools/scripts/verify_sampler_interpolation_benchmark.py --self-test \
  --benchmark-binary \
  build-final-benchmark/test/pulp-sampler-interpolation-benchmark
```

### Documentation

```bash
tools/check-docs.sh
python3 tools/scripts/docs_noise_lint.py --mode report
git diff --check
```

## Completion audit

"Built" means all S0–S8 gates applicable to shipped claims have direct current
evidence: source, focused tests, full Release build, full test suite, sanitizer
results, performance report, docs, and any required capture artifacts. A green
unit test for a primitive does not prove the example integration, multi-voice
torture contract, or hardware authenticity claim. Missing external captures
keep only the affected named profiles unshipped; they do not weaken generic
engine gates.
