# Examples

Pulp ships with seven example projects that validate different capabilities of the framework. Each example is self-contained under `examples/` and builds as part of the standard CMake build.

## Gallery

### Reference

These examples are polished and demonstrate best practices for building Pulp plugins.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpGain](pulp-gain.md) | Stereo gain effect with input/output gain and bypass | VST3, AU, CLAP, Standalone |

### Validation

These examples exist to test and validate specific subsystems of the framework.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpTone](pulp-tone.md) | Polyphonic oscillator synth with MIDI input | VST3, AU, CLAP, Standalone |
| [PulpEffect](pulp-effect.md) | Biquad filter with frequency, resonance, type, and mix | VST3, AU, CLAP |
| [PulpCompressor](pulp-compressor.md) | Sidechain compressor with multi-bus input | VST3, AU, CLAP |
| [PulpDrums](pulp-drums.md) | Generative drum sequencer with MIDI output | CLAP |
| [PulpSynth](pulp-synth.md) | Macro oscillator synth using the signal DSP library | CLAP |

### Experimental

These examples explore features that are not yet stable across all platforms.

| Example | Summary | Formats |
|---------|---------|---------|
| [UI Preview](ui-preview.md) | Standalone app for testing the view/widget and GPU rendering pipeline | Standalone (macOS only) |

## Building Examples

All examples build as part of the standard CMake build:

```bash
cmake -B build
cmake --build build
```

To build a specific example target:

```bash
cmake --build build --target PulpGain_VST3
cmake --build build --target PulpTone_Standalone
```

## Which Example Should I Start With?

Start with **PulpGain**. It is the simplest plugin and covers the full pipeline: `Processor` subclass, parameter definition, format adapters, format-specific entry points, and validation tests. Every other example builds on patterns established in PulpGain.
