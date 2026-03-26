# Examples

Pulp ships with eight plugin examples plus standalone apps (GPU demo, UI preview, web demos) that validate different capabilities of the framework. Each example is self-contained under `examples/` and builds as part of the standard CMake build.

## Gallery

### Reference

These examples are polished and demonstrate best practices for building Pulp plugins.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpGain](example-pulp-gain.html) | Stereo gain effect with input/output gain and bypass | VST3, AU, CLAP, Standalone |

### Validation

These examples exist to test and validate specific subsystems of the framework.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpTone](example-pulp-tone.html) | Polyphonic oscillator synth with MIDI input | VST3, AU, CLAP, Standalone |
| [PulpEffect](example-pulp-effect.html) | Biquad filter with frequency, resonance, type, and mix | VST3, AU, CLAP |
| [PulpCompressor](example-pulp-compressor.html) | Sidechain compressor with multi-bus input | VST3, AU, CLAP |
| [PulpDrums](example-pulp-drums.html) | Generative drum sequencer with MIDI output | CLAP |
| [PulpSynth](example-pulp-synth.html) | Macro oscillator synth using the signal DSP library | CLAP |
| [PulpSampler](example-pulp-sampler.html) | Audio file sampler with MIDI, ADSR, pitch control | CLAP |
| [PulpPluck](example-pulp-pluck.html) | Karplus-Strong plucked string synth | VST3, AU, CLAP |

### Experimental

These examples explore features that are not yet stable across all platforms.

| Example | Summary | Formats |
|---------|---------|---------|
| [UI Preview](example-ui-preview.html) | Standalone app for testing the view/widget and GPU rendering pipeline | Standalone (macOS only) |

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
