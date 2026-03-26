# Examples

Pulp ships with eight example plugins plus standalone apps (GPU demo, UI preview, web demos). Each example validates a specific capability of the framework.

All examples live under `examples/`.

## PulpGain

**Path**: `examples/pulp-gain/`
**Type**: Effect
**Formats**: VST3, AU v2, CLAP, Standalone
**Purpose**: The simplest possible plugin. A gain knob. Validates that the build system works, format entry points are correct, and audio passes through.

Features:
- Single gain parameter (dB)
- All four output formats
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
**Purpose**: A dynamics processor. Validates the sidechain API, latency reporting, and more complex DSP.

Features:
- Threshold, ratio, attack, release parameters
- Sidechain input support
- Latency compensation

## PulpDrums

**Path**: `examples/PulpDrums/`
**Type**: MIDI Effect
**Formats**: CLAP
**Purpose**: A generative drum sequencer. The first example using MIDI output. Validates MIDI effect processing.

Features:
- Three voices (kick, snare, hi-hat)
- Density and chaos controls
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

## UI Preview

**Path**: `examples/ui-preview/`
**Type**: Standalone application (not a plugin)
**Purpose**: A standalone app for testing the view/widget system and GPU rendering pipeline without building a full plugin.

## Building Examples

All examples are built as part of the main build:

```bash
cmake -B build
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
