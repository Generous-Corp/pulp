# Audio Validation Audit and Hardening Plan

Date: 2026-03-25
Purpose: audit the current validation stack as it exists in the repo today, then define the concrete work needed to make audio validation comprehensive and trustworthy.

## Executive Conclusion

Pulp does not yet have comprehensive audio validation in place.

What exists today is a strong start:

- deterministic DSP and unit tests are real
- headless plugin processing tests are real
- some golden-style audio tests are real
- some plugin validators are wired for some examples

What is still missing:

- a reusable deterministic matrix across sample rates, buffer sizes, channel layouts, latency, and automation cases
- complete and enforced validator automation across formats and examples
- official VST3 validator coverage
- host-level regression testing in real DAWs

The current state is best described as:

- deterministic DSP validation: present but partial
- automated plugin validation: present but uneven
- host-level regression testing: absent

## Audit Scope

This audit is grounded in the current repo state, especially:

- `test/`
- `examples/*/test_*.cpp`
- `examples/*/CMakeLists.txt`
- `tools/cli/pulp_cli.cpp`
- `.github/workflows/build.yml`
- `.github/workflows/validate.yml`
- `.github/workflows/sanitizers.yml`
- `docs/guides/testing.md`
- `docs/guides/testing-advanced.md`
- `planning/15-validation-plan.md`

## Layer 1: Deterministic DSP and Unit Validation

### What Exists Today

There is meaningful deterministic coverage already:

- `test/test_signal.cpp`
  - gain conversion
  - compressor/limiter behavior
  - smoothed value behavior
  - ADSR timing
  - biquad/SVF/ladder/Linkwitz-Riley behavior
  - oscillator frequency checks
  - FFT and convolver tests
  - impulse-style and tail-style assertions
- `test/test_golden_audio.cpp`
  - PulpGain golden-style checks for unity gain, +6 dB, silence, bypass, stereo independence, and state round-trip
- `test/test_headless.cpp`
  - headless processing and state round-trip with a simple processor
- example tests such as:
  - `examples/pulp-gain/test_pulp_gain.cpp`
  - `examples/pulp-tone/test_pulp_tone.cpp`
  - `examples/pulp-effect/test_pulp_effect.cpp`
  - `examples/pulp-compressor/test_pulp_compressor.cpp`
  - `examples/PulpSynth/test_pulp_synth.cpp`
  - `examples/PulpDrums/test_pulp_drums.cpp`

This is a good baseline. The framework is not untested.

### Current Gaps

The current deterministic coverage is not yet comprehensive enough to claim a mature validation story.

Missing or incomplete areas:

1. Sample-rate matrix coverage
   - existing tests usually pick one sample rate such as 44.1 kHz or 48 kHz
   - there is no shared harness that sweeps standard rates

2. Buffer-size matrix coverage
   - existing tests use a few fixed sizes
   - there is no systematic sweep from tiny buffers through large buffers

3. Plugin-level parameter automation and smoothing
   - there are unit tests for `SmoothedValue`
   - there is not yet a broad plugin-level contract proving automation and smoothing behavior under block transitions

4. Latency verification
   - format adapters expose latency APIs
   - there is not yet a framework-level test that sends an impulse, measures actual offset, and verifies it matches reported latency

5. Channel-layout and bus-layout coverage
   - stereo behavior is covered in places
   - descriptor metadata is checked for sidechain in `PulpCompressor`
   - there is not yet a reusable runtime harness for mono/stereo/multi-bus/channel-routing validation

6. Golden coverage breadth
   - current golden-style coverage is centered on `PulpGain`
   - there is no broad golden suite for `PulpEffect`, `PulpCompressor`, `PulpTone`, `PulpSynth`, or `PulpDrums`

7. Deterministic edge-case corpus
   - no shared matrix yet for silence, impulse, DC offset, denormal-prone tails, sample-rate changes, and zero-length buffers at the plugin level

### Hardening Plan for Layer 1

#### 1. Build a shared offline validation harness

Create one reusable test utility layer for:

- sample-rate sweeps
- buffer-size sweeps
- deterministic signal generation
- impulse/step/noise/sine/sweep helpers
- RMS/peak/null-test/spectral comparison helpers
- latency measurement helpers

This should stop every example from inventing its own mini harness.

#### 2. Define per-plugin validation contracts

Each example plugin should have an explicit validation contract:

- `PulpGain`
  - gain accuracy
  - bypass transparency
  - stereo independence
  - state round-trip
- `PulpEffect`
  - frequency-response checkpoints
  - filter type switching
  - dry/wet behavior
  - bypass transparency
- `PulpCompressor`
  - threshold/ratio behavior
  - attack/release behavior
  - sidechain routing behavior
  - latency contract if introduced later
- `PulpTone` and `PulpSynth`
  - note timing
  - pitch accuracy
  - envelope timing
  - voice allocation/polyphony behavior
- `PulpDrums`
  - deterministic MIDI generation with fixed seed/state
  - audio pass-through behavior
  - pattern state round-trip

#### 3. Add a deterministic matrix tier

Minimum per-plugin matrix:

- sample rates: 44.1 kHz, 48 kHz, 96 kHz
- buffer sizes: 32, 64, 256, 1024
- layouts: mono or stereo as applicable, plus sidechain where applicable

Release-tier matrix:

- sample rates through 192 kHz
- buffer sizes from 1 to 4096 or 8192 where reasonable
- additional bus layouts for processors that advertise them

#### 4. Add latency and layout assertions to the framework layer

At the framework level, add reusable tests for:

- reported latency vs measured latency
- bus count and optional-bus handling
- channel routing and independence
- block-boundary correctness when buffer size changes

## Layer 2: Automated Plugin Validation

### What Exists Today

There is real validator wiring, but it is incomplete and inconsistent.

What is present:

- example CMake files for `PulpGain`, `PulpTone`, `PulpEffect`, and `PulpCompressor` add:
  - CLAP dlopen tests
  - AU `auval` tests where applicable
  - VST3 `pluginval` tests if `pluginval` is installed
- `PulpSynth` and `PulpDrums` currently register only CLAP dlopen validation
- `pulp validate` exists
- CI has a dedicated `validate.yml` workflow

### Current Gaps

#### 1. Validator behavior is not fully aligned with the public contract

The docs and policy surface talk about `pluginval`, `auval`, and `clap-validator`, but the current CLI and CI do not fully enforce that contract.

Current gaps:

- `pulp validate` currently runs CLAP validation and AU validation, but does not run VST3 `pluginval`
- `validate.yml` does not install `pluginval`
- `validate.yml` does not install `clap-validator`
- `validate.yml` explicitly skips AU validation in CI
- there is no use of Steinberg's official VST3 `validator`
- there is no nightly stricter validator tier
- validator coverage is example-specific rather than standardized across all format-bearing targets

#### 2. Validation depth differs by example

- `PulpGain`, `PulpTone`, `PulpEffect`: strongest validator coverage
- `PulpCompressor`: no AU validation test today
- `PulpSynth`, `PulpDrums`: only CLAP dlopen, not CLAP community validator

That is acceptable for an evolving repo, but not yet for a mature public validation story.

### Hardening Plan for Layer 2

#### 1. Make validator support a first-class build contract

Standardize validator registration so every plugin target declares:

- supported formats
- expected validators
- whether a validator is required on CI, nightly, or release only

Do not leave this as hand-written per-example drift.

#### 2. Make `pulp validate` match the docs

The CLI should become the canonical local validation surface and cover:

- CLAP community validator when installed, with dlopen fallback explicitly labeled as weaker
- AU `auval`
- VST3 `pluginval`
- Steinberg VST3 `validator` when available

The output should distinguish:

- full validator pass
- smoke-check pass
- skipped because tool missing

#### 3. Raise CI from "best effort" to "enforced tiers"

Recommended tiers:

- PR tier
  - install `pluginval`
  - install `clap-validator`
  - run `pluginval` at a moderate strictness level
  - run CLAP validator
  - run AU validation where CI can support it, or move AU validation to a dedicated macOS self-hosted/release runner if GitHub-hosted runners remain impractical
- nightly tier
  - higher `pluginval` strictness
  - Steinberg official VST3 validator
  - full CLAP validator
  - wider example/format matrix
- release tier
  - all of the above plus release-only formats and signing/package verification

#### 4. Add validator parity across examples

Target state:

- every VST3 example has `pluginval`
- every AU example has `auval`
- every CLAP example uses `clap-validator` when available
- any missing validator coverage is an explicit documented exception, not an accident

## Layer 3: Host-Level Regression Testing

### What Exists Today

There is effectively no automated host-level regression layer in the repo.

What does exist:

- planning documents mention DAW validation and a DAW matrix
- docs describe headless testing and format validation

What does not exist:

- Reaper automation scripts
- Logic automation scripts
- DAW project fixtures for bounce/render comparison
- CI or release workflows that open plugins in real hosts and compare outputs

### Conclusion for This Layer

Host-level regression testing is currently a planning goal, not an implemented capability.

### Hardening Plan for Layer 3

#### 1. Start with one automation-friendly host

Use Reaper first because it is scriptable and cross-platform.

Initial Reaper regression pack:

- scan/load plugin
- instantiate plugin on a track
- set parameters
- save and reopen project
- offline render a reference clip
- compare bounced output against a known-good reference or headless render

#### 2. Add AU-specific host coverage on macOS

Add Logic Pro or another Apple-hosted AU workflow for:

- AU scan/load
- state restore
- automation playback
- offline bounce comparison

This can start as semi-automated if full CI automation is not immediately practical.

#### 3. Define what host-level regression is supposed to catch

Host regression should verify issues that headless tests and validators do not catch well:

- DAW scan behavior
- lifecycle ordering differences
- bus activation quirks
- automation playback behavior in the real host
- state restore behavior from host project files
- offline bounce parity
- UI/editor opening and focus behavior where relevant

#### 4. Keep the first host matrix small and high value

Recommended first matrix:

- Reaper: VST3, CLAP, macOS and Windows
- Logic Pro: AU, macOS
- Bitwig: CLAP, macOS or Windows

Do not try to automate every DAW first.

## Recommended Order of Work

### Stage 1: Truth-in-advertising cleanup

Goal: make docs, CLI, and CI describe the current validation reality accurately.

Deliverables:

- current-state validation inventory
- explicit distinction between smoke checks and full validators
- explicit list of which examples/formats are actually covered today

### Stage 2: Deterministic audio matrix

Goal: make offline validation systematic instead of ad hoc.

Deliverables:

- shared test harness
- sample-rate and buffer-size matrix
- plugin-level contracts
- latency/layout assertions

### Stage 3: Validator enforcement

Goal: ensure the local and CI validator story is real and standardized.

Deliverables:

- `pulp validate` parity with docs
- validator installation in CI
- VST3 official validator integration
- standardized validator registration across examples

### Stage 4: Host-level regression

Goal: catch real-world DAW regressions before release.

Deliverables:

- first Reaper automation pack
- first AU host regression workflow
- bounce comparison fixtures

## Release Gate Recommendation

Do not call audio validation "comprehensive" until all three of these are true:

1. Deterministic plugin-level audio contracts exist for every flagship example
2. Format validators are enforced in local CLI and CI for every supported format
3. At least one real DAW regression lane exists for each major host-facing format family

Until then, the honest statement is:

"Pulp has strong deterministic tests and partial validator automation, but does not yet have comprehensive end-to-end audio validation."
