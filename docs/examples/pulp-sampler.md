# PulpSampler

`PulpSampler` is Pulp's production-shaped sampler example. It combines
eight-voice MIDI playback, resident sample publication, strict file-backed
streaming, loop and interpolation policies, starvation handling, and an
optional data-defined Sample Heritage chain. The validated plugin format is
CLAP.

The implementation is in `examples/PulpSampler/`. It is an example integration,
not a promise that every record in `sampler_api.hpp` is already exposed through
every plugin-format adapter.

This example uses the shared polyphonic/random-access page-service model. See
the [sampler playback chooser](../guides/sampler-playback.md) before adopting
it for a sequential single-consumer or fully resident product.

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
forward/reverse wrap-crossfade loops. Hermite/Lagrange forward one-shots may
select an available exact positive-octave mip; streamed `.pulpmip` sidecars
provide at most two levels. Loop, reverse, non-exact, and missing-level playback
stays on the base asset. When a polynomial policy would consume the selected or
base source above 1x, it is promoted to ratio-tracking sinc. If the sinc bank
has no prepared table for that ratio, playback falls back to Hermite instead of
dropping the voice. The coherent `diagnostics()` snapshot exposes the lifetime
number of these selections as `interpolation.sinc_fallback_selections`, so
quality degradation is observable. Down-pitched fractional ratios do not
require that anti-alias promotion.

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
source usable. A successful open streams from an immutable private disk
snapshot retained for that publication, so replacing, overwriting, or
truncating the original path afterward does not change its audio. Snapshot
creation is a control-thread disk-copy cost bounded by the mapped-source byte
limit; it does not decode the whole source into RAM. Admission derives the
resident preload and page geometry from
the 20 ms certified I/O latency, 5 ms scheduler margin, 5 ms decoder allowance,
host block size, interpolation guard, loop guard, and source rate. Streamed
playback admits an effective source-consumption ratio of at most 4x. Admission
combines note/key-map pitch, the selected Heritage pitch family and clock
domain, and live cyclic stretch's `1 / factor` source demand.

Resident mono and interleaved-stereo loads accept at most 2,880,000 frames.
Automatic resident mips are attempted only up to 96,000 total samples (96,000
mono frames or 48,000 stereo frames). Larger valid resident buffers still
publish and use the base/sinc policy.

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
Those six physical slot/member identities are stable across replacement loads;
reuse advances their non-zero generations, so the service's stale-token history
does not grow with the number of files loaded. Source or selection generation
exhaustion fails closed rather than publishing the reserved zero value.
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

The stream service also enforces aggregate decode throughput. Across all active
streamed voices it conservatively computes:

```text
effective source frames/second = sum(each voice's declared source consumption)
```

Heritage-driven note-on rejection increments heritage
`rate_admission_rejections`; a live pitch change that invalidates an active
streamed voice increments heritage `rate_automation_rejections`. Those counters
are distinct from the legacy aggregate-rate counters in `stream_stats()`.

## Control and inspection API

The example exposes typed control-plane results rather than collapsing load,
prepare, codec, or sidecar failures into booleans. Lifecycle is method-specific:
load APIs run off the audio thread after prepare and may publish concurrently
with `process()`; `set_config()` is pre-prepare; heritage replacement or disable
requires stopped audio; inspection belongs on a non-audio control/diagnostic
thread.

| API | Purpose |
|-----|---------|
| `set_config(PulpSamplerConfig)` / `config()` | Set or inspect the pre-prepare streaming-memory budget; configuration changes are rejected after prepare |
| `prepare_result()` | Inspect typed prepare status, exceeded resource limit, and required/configured streaming bytes |
| `load_sample_file_result(path)` / `last_load_result()` | Load a strict ranged asset and inspect codec capability, sidecar status, channel/rate/frame metadata, preload, memory request, mip count, and selection generation |
| `has_sample()` / `sample_length()` | Inspect whether a source is published and its saturated integer frame length |
| `stream_stats()` | Read detailed starvation, decode, admission, memory, and source/page lifecycle counters |
| `diagnostics()` | Read one coherent prepare/load/preload/envelope/interpolation/heritage/memory snapshot, including sinc fallback selections |
| `set_heritage_profile(profile)` / `disable_heritage()` | Replace or disable data-defined Sample Heritage processing and its latency contract |
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

## Sample Heritage Kit

The optional Sample Heritage Kit describes sampler character as strict,
portable profile data. It is not a library of product emulations. Pulp ships
the neutral engine, schema, authoring workflow, and verification tools; it does
not currently ship named machine profiles.

Schema version 3 separates three ordered domains:

```text
per voice (up to 8 blocks, one prepared engine per sampler voice)
  machine domain -> clock -> pitch family -> converter
  -> live cyclic stretch -> hold/droop -> reconstruction -> analog color

post mix (up to 2 bus blocks)
  noise/idle color -> output drive

offline record commit (up to 4 blocks)
  input drive/clip -> anti-alias + record rate -> converter -> commit stretch
```

Every block is independently bypassable, each type can appear at most once in
its domain, and order is part of the contract. A profile may omit any block it
does not need. Strict parsing rejects unknown, duplicate, missing, mistyped,
out-of-range, or out-of-order data instead of guessing.

### Typed profile format

A minimal importable profile is:

```json
{"schema_version":3,"profile_id":"neutral.gritty-cycle-v1","host_sample_rate":48000,"voice":[{"domain":"voice","type":"machine_domain","bypass":false,"sample_rate":32000},{"domain":"voice","type":"pitch","bypass":false,"family":"variable_clock","max_transpose_semitones":24},{"domain":"voice","type":"converter","bypass":false,"family":"linear_pcm","bit_depth":12,"dac_nonlinearity":0.08,"dither_lsb":0.25,"seed":"17","seed_policy":"restart_from_profile_seed"},{"domain":"voice","type":"live_cyclic_stretch","bypass":false,"factor":1.25,"cycle_ms":40,"splice_ms":2,"stereo_link":true,"shuffle_divisions":0,"seed":"29","seed_policy":"restart_from_profile_seed","pitch_mode":"preserve","tempo_lock":true},{"domain":"voice","type":"hold_droop","bypass":false,"mode":"zero_order","hold_samples":2,"droop":0.05},{"domain":"voice","type":"reconstruction","bypass":false,"family":"butterworth","cutoff_law":"machine_rate_ratio","cutoff_value":0.42,"order":4,"ripple_db":0,"stopband_attenuation_db":0},{"domain":"voice","type":"analog_color","bypass":false,"drive":1.2,"asymmetry":0.04,"mix":0.7,"filter_family":"ladder_4pole","cutoff_law":"machine_rate_ratio","cutoff_value":0.3,"resonance":0.1}],"bus":[{"domain":"bus","type":"noise_idle","bypass":false,"noise_amplitude":0.001,"idle_amplitude":0.0002,"tilt_db_per_octave":-1.5,"tilt_reference_hz":1000,"tilt_floor_hz":20,"gate":"voice_active","seed":"41","seed_policy":"restart_from_profile_seed"},{"domain":"bus","type":"output_drive","bypass":false,"drive":1.1,"ceiling":0.95}],"record_commit":[]}
```

The `neutral.` prefix is mandatory. The remainder uses lowercase ASCII
letters, digits, dots, and hyphens, without adjacent or trailing separators;
the complete ID is at most 63 bytes. Seeds are decimal strings so JSON tools
do not truncate 64-bit values. `host_sample_rate` is execution context and is
not folded into the profile identity digest.

The voice pitch block chooses how note pitch is produced:

| Family | Behavior |
|---|---|
| `variable_clock` | Changes the machine clock, so pitch and machine-rate character move together |
| `drop_repeat` | Keeps the machine clock fixed and advances source frames with zero-order drop/repeat selection |
| `early_linear` | Keeps the machine clock fixed and uses linear source interpolation |

`max_transpose_semitones` declares the largest absolute note transposition the
profile supports. PulpSampler rejects new notes outside that envelope. When
automation crosses it, a resident voice retains its last admitted rate; a
streamed voice fades closed and terminates under the sampler's stream-contract
failure policy.

The `Heritage Clock Ratio` parameter multiplies the authored clock block from
0.25x to 4x without changing note pitch. It is a realtime parameter; streamed
voices admit the combined note pitch, authored clock, live-stretch consumption,
and clock multiplier against the 4x certificate before adopting a change.

The converter supports linear PCM and continuous mu=255 or A=87.6 companding
curves with effective quantizer resolution, a normalized Pulp DAC-curve amount,
and optional deterministic bipolar-rectangular dither measured in LSBs. These
curve modes are not G.711 byte codecs. Hold count and droop are likewise neutral
Pulp controls rather than claims about a physical converter's update period or
volts-per-second behavior. Hold/droop runs in the variable machine frame. The
one-pole/Butterworth/Chebyshev/elliptic reconstruction filters and analog
color remain per voice but run in the fixed host-rate reconstruction frame,
after return conversion from the machine frame. Analog color can feed Pulp's
existing nonlinear four-pole ladder module before its normalized asymmetric
drive/VCA mix; `none` keeps the filter out of the path. This is neutral DSP
topology, not a claim about a named circuit or component family. Ladder cutoff
is accepted from 1 Hz through 0.49 times machine rate, additionally bounded by
0.49 times the host processing rate, and resonance from 0 through 0.3; the
bounded envelope excludes self-oscillating settings. The bus can add voice-gated or
always-on seeded noise/idle color with a deterministic spectral-tilt law, then
apply bounded output drive. `output_drive` is a bounded rational soft
saturation (softsign), not a hard clamp: `ceiling` sets the saturation scale
and `drive` sets how quickly the curve approaches it. In the noise block,
`idle_amplitude` is the always-on
component and `noise_amplitude` is controlled by `gate`; both are normalized
linear full-scale amplitudes rather than SNR measurements.

For reconstruction, `fixed_hz` expresses the profile's selected digital design
edge in hertz and `machine_rate_ratio` expresses that edge as a 0-to-0.5 ratio
of machine rate. The resolved edge must also remain below the host-rate Nyquist
limit because the fixed reconstruction frame is processed at host rate. The
named filter family determines how that design edge is interpreted. The
supported orders and parameter ranges are Pulp implementation bounds, not
claims about historical hardware.

### Live and committed stretch

Live cyclic stretch is a deliberately cyclic sampler mechanism, not the
general-purpose phase-vocoder stretch API. It runs inside each voice between
the converter and hold/reconstruction stages. `factor` controls duration,
`cycle_ms` and `splice_ms` define machine-domain cycle and crossfade lengths,
`pitch_mode` selects `preserve` (unit-rate reads within each cycle) or
`rate_linked` (cycle-local reads follow the stretch ratio), and `tempo_lock`
decides whether `factor` changes source consumption. With `tempo_lock:false`,
the live stage is an exact duration-neutral bypass. Optional seeded divisions
rearrange material deterministically. Zero
shuffle divisions is identity order. The processor uses fixed-capacity rings,
precomputed bounds, and no callback allocation, locks, FFT, or similarity
search. A factor-one configuration has an exact bypass. At end of source it
drains to exactly the rounded stretched one-shot duration.

An active non-unity configuration prebuffers one bounded cycle plus splice and
interpolator guards from the asset before its first output. This is bounded
source pre-read, not additional host/PDC latency or an adaptive lookahead
window. Once activated, the algorithm is causal: it retains bounded source
history and performs no future similarity search.

Record commit begins with optional `input_drive_clip`, which uses the same
bounded rational soft-saturation shape with `clip_level` as its scale; despite
the field name, it is not a hard clamp. Record-commit stretch is separate and
always offline. `cyclic` uses fixed
sample-domain cycles and crossfades; the cycle value remains the output-domain
snap period even when the crossfade is nonzero. `adaptive` performs
deterministic, bounded similarity search with explicit hop, radius, stride,
crossfade, zone, and stereo-link controls. `quality` and `width` are normalized
0-to-99 authoring controls: `quality` scales the explicit search-radius maximum,
and `width` scales the explicit crossfade maximum. Zero disables that derived
search or crossfade; 99 uses the advanced low-level value in full. Intermediate
values use deterministic rounded integer scaling. This does not claim to copy
an undocumented historical algorithm. It is intended for committing a
transformed asset, not for work on the audio callback.

`estimate_sample_heritage_auto_cycle()` can suggest an explicit cyclic sample
count before building the profile. It searches a caller-bounded lag range using
DC-removed normalized correlation, returns its score, and deterministically
keeps the shortest tied lag. Store the selected count in `cycle_samples`; the
profile never performs hidden analysis during rendering. This is a neutral
authoring helper, not a reproduction of a hardware auto-cycle control.

`commit_sample_heritage_recording()` applies the record-domain drive, record
filter/rate, converter, and optional stretch as one allocating transaction. It
returns machine-rate PCM plus canonical metadata containing the source and
committed audio hashes, profile ID/digest, dimensions, and caller-supplied
provenance. `reload_sample_heritage_committed_asset()` verifies that envelope
and audio hash before publication. Record-commit blocks in a playback profile
are not silently re-applied during live rendering.

This is intentionally distinct from `signal::OfflineStretch`, which is the
high-quality conventional tempo/pitch tool used by PulpTempoSampler. Choose
Heritage cyclic stretch only when cyclic resynthesis itself is the desired
character.

### Load, export, inspect, and render

Use the Heritage CLI off the audio thread:

```bash
pulp audio heritage validate profile.json --json
pulp audio heritage canonicalize profile.json --out canonical.json
pulp audio heritage canonicalize canonical.json --out canonical-again.json
cmp canonical.json canonical-again.json
pulp audio heritage inspect canonical.json --json
pulp audio heritage render canonical.json --fixture impulse \
  --out impulse.wav --report impulse.json
```

`canonicalize` is the export path: its deterministic schema-v3 JSON is the
portable interchange artifact. Canonicalize twice to prove byte idempotence,
then compare the digest reported by `inspect` before and after re-import. The
render command also accepts `sine`, `two-tone`, or an exact-profile-rate mono
WAV fixture and emits Float32 WAV plus a canonical evidence report. See the
[CLI reference](../reference/cli.md#pulp-audio) and the public
[`heritage-profile` skill](../../.agents/skills/heritage-profile/SKILL.md).

Profiles carry an explicit schema version. A future incompatible format can
therefore ship an explicit, tested old-to-new migration. Until the installed
release documents such a migration, unsupported versions fail closed; do not
change the number by hand or silently discard fields.

### PulpSampler lifecycle, state, and admission

Configure or replace a profile only while audio is stopped:

```cpp
const auto parsed = pulp::audio::parse_sample_heritage_profile_json(json);
if (!parsed.valid()) {
    // Report parsed.status and parsed.field_path on the control thread.
    return;
}

const auto status = processor.set_heritage_profile(parsed.profile);
const auto diagnostics = processor.heritage_diagnostics();
```

Validation, coefficient preparation, buffers, sinc banks, rings, and scratch
allocation happen off the callback. The resource estimate accounts separately
for all eight voice engines, bus state, conversion kernels, cyclic rings,
fixed-clock pitch history, scratch, resident storage, and shared streaming
storage. An undersized or unrepresentable configuration fails transactionally.
The audio callback consumes only prepared fixed-capacity state.

Active host-to-machine and machine-to-host conversion legs report their causal
latency. PulpSampler rounds the nominal host-frame value up and notifies the
host when it changes. An all-bypassed profile is transparent and zero-latency.
Invalid plans or processing failures fail closed to silence and increment
typed diagnostics.

Streaming admission follows actual source demand. The profile clock and
variable-clock pitch change the machine rate; fixed-clock pitch changes source
advance; live stretch consumes `1 / factor` source frames per stretched output
frame. PulpSampler combines those terms for preload, page lookahead, aggregate
decode-throughput admission, live automation checks, and frozen starvation
fades. Resident sources are exempt from streamed decode admission. Loop and
reverse traversal still use their canonical logical source order before the
per-voice Heritage chain.

Canonical JSON readers/writers cover both profiles and bounded runtime state.
Typed runtime-state schema v2 preserves slot order for all eight voices and the
bus. Only converter/noise/live-cyclic RNG streams that opt into
`continue_serialized_state` continue; SRC phase/history, pitch history,
hold/filter state, and other DSP transients reset. The example publishes its
bounded continuation snapshot through a `SeqLock`. Same-rate restore applies
it before the first callback. A different host rate keeps the profile but
resets runtime continuation and reports `ReadyRuntimeResetForHostRate`.

### Authoring profiles without overclaiming

No capture or listening session is required to use, extend, or release the
neutral SDK. Captures and listening are optional calibration evidence for a
particular profile. A profile can instead be designed from a neutral audible
goal and proved with analytic fixtures.

If research refers to a real company, product, or model, use the name only
where factually necessary in provenance. Those names and marks remain their
owners' property. Do not place them in the neutral profile ID, imply
affiliation or endorsement, or claim an exact hardware or serial-number match.
Keep citations, confidence, inferences, prompts, renders, and optional captures
in a caller-owned artifact repository; the runtime profile remains portable
data.

The public skill includes a neutral template, evidence-manifest template, and
a complete copy-paste prompt. A concise handoff is:

```text
Use Pulp's heritage-profile skill to create a data-only schema-v3 profile for
<desired character>. Research only legally accessible primary/technical
sources, keep factual names in provenance rather than the neutral.* ID, and do
not imply affiliation or an exact hardware match. Validate, canonicalize twice,
inspect, render analytic and factor-one/all-bypass controls, prove one invalid
profile fails, compare round-trip digests, and archive the prompt, citations,
confidence per value, reports, hashes, and optional listening/capture notes in
my chosen artifact repository. Report any mechanism the typed blocks cannot
express instead of approximating it silently.
```

Creating named or reference-informed profiles is therefore an extension
workflow, not an unshipped SDK dependency. Pulp currently bundles no named
profiles.

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
  --target pulp-sampler-render-wav pulp-sampler-heritage-render-wav \
           pulp-test-sample-heritage-shipping-gates
ctest --test-dir build-aql \
  -R '^sampler-(quality-lab|heritage-quality-lab)-' \
  --output-on-failure
ctest --test-dir build-aql -L heritage-g1 --output-on-failure
ctest --test-dir build-aql -N -L quality-lab
```

The C++ shipping gate compares the full representative Heritage chain against
the sampler's actual `LoopRenderer` ratio-tracking-sinc voice path, measures
live cyclic cost after both short and long histories, and retains deterministic
storage bounds. Run these CPU gates in Release builds.

The checked-in interpolation benchmark under
`docs/validation/sampler-interpolation/` is a current 108-row Release capture.
Its verifier checks the complete matrix, acceptance budgets,
source bundle, binary hash, environment, and negative controls. Later sampler
integration commits do not make it stale when the verifier's content-addressed
interpolation source bundle remains unchanged; the current source-only verifier
passes that exact digest.
