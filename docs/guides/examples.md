# Examples

Pulp's curated example guide covers the core plugin gallery, selected standalone
apps, and source-only DSP lanes. The broader `examples/` tree also contains
subsystem demos and platform-specific proof projects; use each example's local
README or subsystem guide for those specialized workflows.

All examples live under `examples/`.

## PulpGain

**Path**: `examples/pulp-gain/`
**Type**: Effect
**Formats**: VST3, AU v2, AUv3, CLAP, Standalone, optional AAX
**Purpose**: The simplest possible plugin. A gain knob. Validates that the build system works, format entry points are correct, and audio passes through.

Features:
- Single gain parameter (dB)
- Default VST3, CLAP, and Standalone targets; AU v2/AUv3 on Apple; optional AAX on macOS/Windows
- Golden-file audio test
- Includes `Info.plist.au`, `Info.plist.vst3`, `moduleinfo.json`

## PulpTone

**Path**: `examples/pulp-tone/`
**Type**: Instrument
**Formats**: VST3, AU v2, CLAP, Standalone
**Purpose**: A simple oscillator synth with MIDI input. Validates audio I/O, MIDI processing, and the instrument plugin path.

Features:
- Sine/saw/square oscillators
- MIDI note input
- Multiple format targets

## PulpEffect

**Path**: `examples/pulp-effect/`
**Type**: Effect
**Formats**: VST3, AU v2, CLAP
**Purpose**: A filter effect with cutoff, resonance, and mix parameters. Validates parameter automation and state save/load across formats.

Features:
- Multi-parameter processing
- State serialization
- Format adapter validation

## PulpCompressor

**Path**: `examples/pulp-compressor/`
**Type**: Effect
**Formats**: VST3, AU v2, CLAP
**Purpose**: A dynamics processor. Validates the sidechain API, multi-bus routing, and more complex DSP.

Features:
- Threshold, ratio, attack, release parameters
- Sidechain input support
- Envelope follower and gain reduction

## PulpDrums

**Path**: `examples/PulpDrums/`
**Type**: MIDI Effect
**Formats**: CLAP
**Purpose**: A generative drum sequencer. The first example using MIDI output. Validates MIDI effect processing.

Features:
- Four tracks (kick, snare, hi-hat, clap)
- Density, velocity, pattern, and randomize controls
- Pattern generation
- MIDI output

## PulpSynth

**Path**: `examples/PulpSynth/`
**Type**: Instrument
**Formats**: CLAP
**Purpose**: A macro oscillator synthesizer. Validates polyphonic instrument processing and more complex parameter layouts.

Features:
- Multiple oscillator models
- Modulation parameters
- Polyphonic voice handling

## PulpSampler

**Path**: `examples/PulpSampler/`
**Type**: Instrument
**Formats**: CLAP
**Purpose**: A production-shaped sampler with MIDI triggering, ADSR, pitch,
loop/reverse playback, selectable interpolation, bounded file streaming, and
an optional synthetic heritage-processing chain. Validates transactional
off-thread mono/stereo publication, multi-voice playback, starvation handling,
streamed mip selection, and parameter/state round-trips.

Features:
- Off-thread mono/stereo resident loading and strict ranged WAV/AIFF streaming
- MIDI note triggering with velocity
- ADSR, pitch, loop, reverse, and interpolation controls
- Eight-voice polyphony with bounded starvation fades
- Resident and persisted streamed octave mips with ratio-tracking sinc fallback
- Checked streaming-memory configuration plus typed prepare/load and coherent
  combined diagnostics
- Optional typed synthetic heritage processing, transactional clock-domain
  rebind, and strict profile/runtime-state persistence

## PulpPluck

**Path**: `examples/pulp-pluck/`
**Type**: Instrument
**Formats**: VST3, AU v2, CLAP
**Purpose**: A Karplus-Strong plucked-string synthesizer. Validates the
instrument plugin path with a compact physical-modeling processor that also
feeds the web demo lane.

Features:
- Plucked-string synthesis
- MIDI note input
- Decay, brightness, and volume parameters
- Native plugin targets plus WAM demo reuse

## PulpMultiOut

**Path**: `examples/pulp-multi-out/`
**Type**: Instrument
**Formats**: VST3, AU v2 (aumu), CLAP
**Purpose**: An 8-voice synth that renders each voice to its OWN stereo output
bus (1 main + 7 aux). Demonstrates the multi-bus output path — the AU v2 adapter
turns the descriptor's eight output buses into eight AU output elements, so a DAW
lists the aux outputs and routes each voice to its own mixer channel.

Features:
- Eight stereo output buses addressed by index via `process(ProcessBuffers&)`
- Round-robin voice assignment from incoming MIDI notes
- `auval`-validated multi-output aumu; headless routing + bus-contract tests
- See `docs/guides/multi-bus-audio.md` for the multi-bus contract and the manual
  DAW bus-visibility checklist

## UI Preview

**Path**: `examples/ui-preview/`
**Type**: Standalone application (not a plugin)
**Purpose**: A standalone app for testing the view/widget system and GPU rendering pipeline without building a full plugin.

## Additional CMake-Backed Examples

`examples/CMakeLists.txt` includes more shipped targets than the curated plugin
gallery above. Current examples include PulpTempoSampler, PulpMpeSynth,
ViewBridge, Audio Inspector, Plugin Host, Stream, SDF, WebView, Three.js, and iOS
AUv3 demos. Some are gated by platform, optional dependencies, or feature flags.

## FAUST Examples

These examples validate the current supported FAUST lane: offline codegen,
checked-in generated headers, `FaustProcessor<T>` wrapping, and headless
regression tests.

### FaustGain

**Path**: `examples/faust-gain/`
**Type**: Effect
**Purpose**: Smallest FAUST example. Validates metadata reflection, state
round-trip, and predictable gain processing.

### FaustFilter

**Path**: `examples/faust-filter/`
**Type**: Effect
**Purpose**: Multi-parameter FAUST effect. Validates parameter reflection and
expected filter behavior.

### FaustTremolo

**Path**: `examples/faust-tremolo/`
**Type**: Effect
**Purpose**: Modulation effect example. Validates another non-trivial generated
DSP shape and the current offline-codegen workflow.

## Cmajor Example

This example validates the current Cmajor support lane in its truthful scope:
source-owned patch files plus an external-toolchain helper. It is **not** built
as part of the normal CMake example set and does not imply a bundled Cmajor
runtime.

### CmajorGain

**Path**: `examples/cmajor-gain/`
**Type**: Source-only effect example
**Purpose**: Minimal patch for validating Pulp's MIT-safe Cmajor workflow:
patch/source structure, helper diagnostics, and user-supplied `cmaj` codegen.

## JSFX Examples

These examples validate Pulp's current bounded JSFX lane: source-only `.jsfx`
files plus explicit subset validation. They do **not** imply a bundled REAPER
runtime or full JSFX compatibility.

### PulpJsfxGain

**Path**: `examples/jsfx-gain/`
**Type**: Source-only effect example
**Purpose**: Minimal gain example validating `desc:`, slider declarations, and
the `@init` + `@slider` + `@sample` subset.

### PulpJsfxTremolo

**Path**: `examples/jsfx-tremolo/`
**Type**: Source-only effect example
**Purpose**: Modulation example validating the same subset with a more musical
processing shape.

### PulpJsfxDelay

**Path**: `examples/jsfx-delay/`
**Type**: Source-only effect example
**Purpose**: Memory-based delay idiom validating `@block` + `@sample` style in
the bounded subset.

## Building Examples

CMake-backed examples in the root example graph build as part of the main build
when their platform and option gates are satisfied. Source-only Cmajor and JSFX
examples use the lane-specific validators below.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To build a specific example:

```bash
cmake --build build --target PulpGain_VST3
cmake --build build --target PulpTone_Standalone
```

## Testing Examples

Each plugin example includes a test file (`test_pulp_*.cpp`) that exercises the processor through the headless host:

```bash
pulp test -R PulpGain
pulp test -R PulpTone
```

For the FAUST lane specifically:

```bash
cmake --build build --target pulp-faust-gain-test pulp-faust-filter-test pulp-faust-tremolo-test -j8
ctest --test-dir build --output-on-failure -R Faust
```

For the Cmajor external lane specifically:

```bash
python3 tools/scripts/cmajor_external.py doctor \
  --patch examples/cmajor-gain/CmajorGain.cmajorpatch
python3 -m unittest tools/scripts/test_cmajor_external.py
```

For the JSFX bounded lane specifically:

```bash
python3 tools/scripts/jsfx_subset.py doctor --file examples/jsfx-gain/PulpJsfxGain.jsfx
python3 tools/scripts/jsfx_subset.py doctor --file examples/jsfx-tremolo/PulpJsfxTremolo.jsfx
python3 tools/scripts/jsfx_subset.py doctor --file examples/jsfx-delay/PulpJsfxDelay.jsfx
python3 -m unittest tools/scripts/test_jsfx_subset.py
```
