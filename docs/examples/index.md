# Examples

This page is the curated gallery for the documented core examples. The broader
`examples/` tree also contains subsystem demos and platform-specific proof
projects such as MPE, WebView, Three.js, SDF, Stream,
Plugin Host, Audio Inspector, and iOS AUv3 examples.

## Gallery

### Reference

These examples are polished and demonstrate best practices for building Pulp plugins.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpGain](example-pulp-gain.html) | Stereo gain effect with input/output gain and bypass | VST3, AU v2/AUv3, CLAP, Standalone, optional AAX |

### Validation

These examples exist to test and validate specific subsystems of the framework.

| Example | Summary | Formats |
|---------|---------|---------|
| [PulpTone](example-pulp-tone.html) | Polyphonic oscillator synth with MIDI input | VST3, AU v2, CLAP, Standalone |
| [Timeline Phase 1](example-timeline-phase1.html) | Tempo-mapped audio-file and step-pattern playback through the Creative Timeline Engine | Headless, Standalone |
| [PulpEffect](example-pulp-effect.html) | Biquad filter with frequency, resonance, type, and mix | VST3, AU v2, CLAP |
| [PulpCompressor](example-pulp-compressor.html) | Sidechain compressor with multi-bus input | VST3, AU v2, CLAP |
| [PulpDrums](example-pulp-drums.html) | Generative drum sequencer with MIDI output | CLAP |
| [PulpSynth](example-pulp-synth.html) | Macro oscillator synth using the signal DSP library | CLAP |
| [PulpSampler](example-pulp-sampler.html) | Eight-voice sampler with bounded streaming, loops, interpolation, mips, and data-defined Sample Heritage profiles | CLAP |
| [PulpTempoSampler](pulp-tempo-sampler.md) | Offline tempo matching with matched-generation resident sample and slice publication | VST3, AU v2, CLAP, Standalone |
| [PulpPluck](example-pulp-pluck.html) | Karplus-Strong plucked string synth | VST3, AU v2, CLAP |

### Experimental

These examples explore features that are not yet stable across all platforms.

| Example | Summary | Formats |
|---------|---------|---------|
| [UI Preview](example-ui-preview.html) | Standalone app for testing the view/widget and GPU rendering pipeline | Standalone (macOS only) |
| [SuperConvolver](super-convolver.html) | GPU convolution reverb with live IR swap — built on the [GPU audio runtime](../reference/modules.md#gpu_audio) | VST3, AU v2, CLAP, Standalone |
| [Spectral Lab](spectral-lab.html) | N-layer GPU spectral freeze / morph cloud — built on the [GPU audio runtime](../reference/modules.md#gpu_audio) | VST3, AU v2, CLAP, Standalone |
| [GPU NAM](gpu-nam.html) | Neural Amp Modeler (`.nam`) player with a GPU audio engine — now in [its own repo](https://github.com/danielraffel/pulp-gpu-nam) | VST3, AU v2, CLAP, Standalone |
| [Bendr](bendr.html) | Pitch / formant shifter with spectral freeze and a pitched feedback delay — now in [its own repo](https://github.com/danielraffel/pulp-bendr) | VST3, AU v2, CLAP, Standalone |

## Building Examples

The CMake-backed gallery examples build as part of the standard CMake build
when their platform and option gates are satisfied:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To build a specific example target:

```bash
cmake --build build --target PulpGain_VST3
cmake --build build --target PulpTone_Standalone
```

## Which Example Should I Start With?

Start with **PulpGain**. It is the simplest plugin and covers the full pipeline: `Processor` subclass, parameter definition, format adapters, format-specific entry points, and validation tests. Every other example builds on patterns established in PulpGain.
