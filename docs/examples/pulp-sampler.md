# PulpSampler

`PulpSampler` is Pulp's production-shaped sampler example. It combines
eight-voice MIDI playback, resident sample publication, strict file-backed
streaming, loop and interpolation policies, starvation handling, and an
optional synthetic heritage-processing chain. The validated plugin format is
CLAP.

The implementation is in `examples/PulpSampler/`. It is an example integration,
not a promise that every record in `sampler_api.hpp` is already exposed through
every plugin-format adapter.

## Build and load

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_EXAMPLES=ON
tools/ci/governed-build.sh cmake --build build --target PulpSampler_CLAP
```

The bundle is written under `build/CLAP/`. The processor accepts MIDI and has a
stereo output. It reports an indefinite tail because a held or looping voice
can sustain until note release.

## Playback controls

The example exposes gain, ADSR, pitch (`-24` to `+24` semitones), loop, reverse,
and interpolation parameters. The interpolation integer contract is stable:

| Value | Policy |
|---:|---|
| 0 | Hold |
| 1 | Nearest |
| 2 | Linear (default) |
| 3 | Cubic Hermite |
| 4 | Cubic Lagrange |
| 5 | Ratio-tracking sinc |

Resident and streamed sources support forward and reverse one-shots and
forward/reverse wrap-crossfade loops. Resident forward one-shots can use
immutable octave mips for exact-octave polynomial playback. Fractional-octave,
loop, reverse, oversized, or missing-level cases use ratio-tracking sinc rather
than claiming unsupported anti-alias performance. Streamed sources can select
the same offline-built octave levels from a valid `.pulpmip` sidecar.

## Loading samples

Call all loading APIs off the audio thread and after `prepare()`:

```cpp
bool mono_ok = processor.load_sample(mono, frame_count, sample_rate);
bool stereo_ok = processor.load_sample_stereo(
    interleaved_stereo, frame_count, sample_rate);
bool file_ok = processor.load_sample_file("samples/kick.wav");

auto file = processor.load_sample_file_result("samples/piano.wav");
if (!file.loaded()) {
    std::cerr << pulp::examples::pulp_sampler_load_status_name(file.status)
              << '\n';
}
```

The file path admits only true ranged readers: WAV and uncompressed
AIFF/AIFF-C `NONE`, at one or two channels and no more than 192 kHz. FLAC is
available to the general audio-file layer as a decode-once fallback, not as a
ranged sampler reader, so the sampler rejects it transactionally instead of
silently loading the entire asset. The same rule applies to any other codec
that lacks ranged reads: a failed replacement leaves the previously published
source usable. Admission derives the resident preload and page geometry from
the 20 ms certified I/O latency, 5 ms scheduler margin, 5 ms decoder allowance,
host block size, interpolation guard, loop guard, and source rate. Streamed
playback admits an effective consumption ratio of at most 4x: the note/key-map
pitch ratio multiplied by the active heritage clock ratio.

`PulpSamplerProcessor::load_sample_file()` is the boolean convenience form.
Use `load_sample_file_result()` when the caller needs the codec capability,
sidecar status, preload sizes, memory demand, selection generation, or precise
failure status. `last_load_result()` returns the most recent file-load attempt
result. Concurrent control-plane operations are serialized; a failed
replacement leaves the previously published source and its generation usable.

Build one or two persisted streamed mip levels next to a source with:

```bash
pulp audio sampler-mip build samples/piano.wav --levels 2 --json
```

The command uses the sampler's 140 dB-design decimator, writes hash-addressed
float32 WAV payloads, and atomically publishes and verifies the `.pulpmip`
manifest. An invalid sidecar is ignored and reported by the detailed runtime
result; it never replaces the base source.

## Threads, memory, and failure behavior

The callback owns voice state and pushes bounded demand/cancellation commands.
One non-audio owner thread mutates file, source, and cache state; two decode
workers perform bounded page reads. The callback does not allocate, wait, open
files, or invoke a decoder. See the [shared sample-stream service](../guides/sample-stream-service.md)
for the multi-voice cache and worker contract, and the narrower
[sequential streaming source](../guides/streaming-sample-source.md) for the
preload-plus-ring primitive.

The example derives its default memory capacity during prepare. To impose a
smaller non-zero streaming cap, supply `PulpSamplerConfig` at construction or
through `set_config()` before prepare:

```cpp
pulp::examples::PulpSamplerConfig config{
    .streaming_memory_budget_bytes = 64ull * 1024ull * 1024ull,
};
pulp::examples::PulpSamplerProcessor processor(config);

// After the host calls prepare():
const auto prepared = processor.prepare_result();
if (!prepared.prepared()) {
    std::cerr << pulp::examples::pulp_sampler_prepare_status_name(prepared.status)
              << '\n';
}
```

Zero selects the certified derived default. `set_config()` fails after a
successful prepare, and an undersized cap fails transactionally with
`StreamingMemoryBudgetTooSmall`. The general prepare-resource estimate also
accounts for resident-bank, interpolation, heritage, voice, and service
storage; the streaming cap does not pretend to cover those separate owners.
The principal streamed storage is:

```text
page bytes = 2 channels * page_frames * source_capacity * cache_pages_per_source * sizeof(float)
preload bytes = 2 channels * required_preload_frames * source_capacity * sizeof(float)
decode scratch bytes = 2 channels * page_frames * 2 workers * 8 jobs/source * sizeof(float)
```

`page_frames` is derived from the certified worst case and is at least 2048.
`source_capacity` is six (two publication bundles with a base plus at most two
mip members), and `cache_pages_per_source` is 128 (eight voices times the
16-page maximum block footprint).
Each registered source or mip member receives a shared page budget sized for
the bounded aggregate demand footprint of eight independently positioned
voices; voices do not own duplicate caches. Resident sample and resident-mip
ownership are outside this streaming figure.

Stream misses never replay stale data. A predicted miss uses a 64-frame
equal-power fade to silence; recovery uses a 64-frame fade from silence.
`stream_stats()` separates service starvation, starved output frames, decode
failure, invalid preload contract, stale generation, normal end of source,
invalid render contract, memory high-water marks, and source/page lifecycle
counters. `diagnostics()` returns the top-level `PulpSamplerDiagnostics`
snapshot with the last prepare and load results, the matching preload policy,
callback-published starvation-envelope state, heritage status, and streaming
memory capacity/current/peak values. Its `snapshot_epoch` advances with each
coherent diagnostic publication; a successful load's `selection_generation`
matches the preload record from the same source publication. These inspection
APIs are for a control or diagnostic thread, never the audio callback.

The stream service also enforces aggregate decode throughput. For active voices
sharing one source it computes:

```text
effective source frames/second = sum(pitch ratio * source sample rate) * active clock ratio
```

Clock-driven note-on rejection increments heritage
`rate_admission_rejections`; a live pitch change that invalidates an active
streamed voice increments heritage `rate_automation_rejections`. Those counters
are distinct from the legacy aggregate-rate counters in `stream_stats()`.

## Control and inspection API

The example exposes typed control-plane results rather than collapsing load,
prepare, codec, or sidecar failures into booleans. Call these methods while the
audio callback is stopped unless the method is explicitly an inspection
snapshot:

| API | Purpose |
|-----|---------|
| `set_config(PulpSamplerConfig)` / `config()` | Set or inspect the pre-prepare streaming-memory budget; configuration changes are rejected after prepare |
| `prepare_result()` | Inspect typed prepare status, exceeded resource limit, and required/configured streaming bytes |
| `load_sample_file_result(path)` / `last_load_result()` | Load a strict ranged asset and inspect codec capability, sidecar status, channel/rate/frame metadata, preload, memory request, mip count, and selection generation |
| `has_sample()` / `sample_length()` | Inspect whether a source is published and its saturated integer frame length |
| `stream_stats()` | Read detailed starvation, decode, admission, memory, and source/page lifecycle counters |
| `diagnostics()` | Read one coherent prepare/load/preload/envelope/heritage/memory snapshot |
| `set_heritage_profile(profile)` / `disable_heritage()` | Replace or disable synthetic heritage processing and its latency contract |
| `heritage_diagnostics()` | Inspect profile identity, clock/rate state, reported latency, failures, and admission counters |

For example, return to the transparent zero-latency sampler path while stopped:

```cpp
if (processor.disable_heritage() !=
    pulp::examples::PulpSamplerHeritageStatus::Disabled) {
    // Inspect processor.heritage_diagnostics() before resuming audio.
}
```

`PulpSamplerPrepareStatus`, `PulpSamplerLoadStatus`,
`PulpSamplerCodecCapability`, `PulpSamplerSidecarStatus`, and
`PulpSamplerHeritageStatus` all have stable string-name helpers in
`sampler_api.hpp`. A built `.pulpmip` sidecar does not override runtime source
admission: PulpSampler still requires one or two channels and a source rate no
greater than 192 kHz.

## Synthetic heritage processing

An optional typed profile applies ordered, independently bypassable stages for
machine-rate conversion, quantization, clock/pitch, DAC hold, reconstruction
filtering, noise, and output. Profile IDs are neutral identifiers; no named
hardware profile or capture-matched claim ships.

Configure or replace a profile only while audio is stopped:

```cpp
pulp::audio::SampleHeritageProfile profile{
    .schema_version = pulp::audio::kSampleHeritageProfileSchemaVersion,
    .profile_id = "neutral.example-two-leg-v2",
    .host_sample_rate = 48000.0,
    .stages = {
        {false, pulp::audio::SampleHeritageMachineDomainStage{32000.0}},
        {false, pulp::audio::SampleHeritageClockPitchStage{1.25}},
    },
};

auto status = processor.set_heritage_profile(profile);
auto diagnostics = processor.heritage_diagnostics();
```

The engine validates and prepares off-thread. Active host-to-machine and
machine-to-host conversion legs each contribute the causal 24-tap-half-width
delay in their output coordinate; the processor rounds the resulting nominal
host-frame latency up and notifies the host when it changes. An all-bypassed
profile is transparent and reports zero latency. Render-plan or render failure
fails closed to silence and increments heritage diagnostics.

Prepare binds streaming to the heritage processing input domain. Its output
sample rate is the host rate multiplied by the active clock ratio, and its
maximum block is the heritage engine's maximum input-frame requirement. The
preload contract therefore records this clocked stream rate, not always the
host's external output rate. An all-bypassed profile uses a clock ratio of one.

Canonical JSON readers/writers exist for profiles and bounded runtime state.
Runtime persistence captures only RNG continuation for quantization/noise
stages that opt into `ContinueSerializedState`; it does not capture SRC phase
or history, DAC-hold state, or reconstruction-filter transients. The example's
`sampler_heritage_state.hpp` adds a strict, versioned envelope for the profile
and optional runtime JSON, and `PulpSamplerProcessor` uses that envelope for its
outer plugin state. At callback end, the processor publishes a bounded RNG
continuation snapshot through a `SeqLock`; same-rate restore applies it before
the first callback. Restoring at a different host rate keeps the profile but
resets runtime state and reports `ReadyRuntimeResetForHostRate`. SRC history,
DAC-hold state, and reconstruction-filter transients always restart rather than
pretending to continue across a session restore.

## Verification

Focused sampler tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON
tools/ci/governed-build.sh cmake --build build --target pulp-sampler-test
./build/examples/PulpSampler/pulp-sampler-test
```

The dependency-bearing interpolation and heritage reference gates use separate
render executables and independent Python oracles. Ordinary pytest collection
skips these renderer-dependent cases; configure the CMake gate to require them:

```bash
python3 -m venv .venv-aql
.venv-aql/bin/python -m pip install -e 'tools/audio/quality-lab[test]'

cmake -S . -B build-aql -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_TESTS=ON \
  -DPULP_AUDIO_QUALITY_LAB_GATE=ON \
  -DPULP_AUDIO_QUALITY_LAB_PYTHON="$PWD/.venv-aql/bin/python"
tools/ci/governed-build.sh cmake --build build-aql \
  --target pulp-sampler-render-wav pulp-sampler-heritage-render-wav
ctest --test-dir build-aql \
  -R '^sampler-(quality-lab|heritage-quality-lab)-' \
  --output-on-failure
```

The checked-in interpolation benchmark under
`docs/validation/sampler-interpolation/` is a current 108-row Apple M5 Max
Release capture. Its verifier checks the complete matrix, acceptance budgets,
source bundle, binary hash, environment, and negative controls. Later sampler
integration commits do not make it stale when the verifier's content-addressed
interpolation source bundle remains unchanged; the current source-only verifier
passes that exact digest.

## Current gap

- Named machine profiles remain intentionally unshipped until research,
  provenance, capture, and listening gates are satisfied.
