# PulpGain

**Category**: reference
**Type**: Effect
**Path**: `examples/pulp-gain/`

## Summary

The simplest possible Pulp plugin. A stereo gain effect with input gain, output gain, and bypass parameters. This is the canonical example for learning how to build a Pulp plugin and is the recommended starting point for new developers.

## What It Demonstrates

- Subclassing `format::Processor` with `descriptor()`, `define_parameters()`, `prepare()`, and `process()`
- Defining parameters with stable IDs, ranges, units, and step values
- Reading parameter values from `StateStore` on the audio thread via `state().get_value()`
- Stereo audio pass-through and gain processing
- Bypass parameter handling
- Building all four plugin format targets from a single processor
- Format-specific entry point files (`vst3_entry.cpp`, `au_v2_entry.cpp`, `clap_entry.cpp`, `main.cpp`)
- Bundle metadata files (`Info.plist.au`, `Info.plist.vst3`, `moduleinfo.json`)
- Format validation tests (auval, pluginval, CLAP dlopen)

## Try in Browser

**[Open PulpGain in Browser Host →](../browser-host/?plugin=PulpGain)**

The same Processor code that runs as VST3/AU/CLAP is compiled to WebAssembly and runs in your browser via the Web Audio API. Drop an audio file to hear gain processing in real-time.

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | Yes |
| AU v2 | Yes |
| CLAP | Yes |
| Standalone | Yes |
| WAMv2 (Browser) | Yes |
| WebCLAP (WASM) | Yes |

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes |
| Windows | Build stubs present, not yet validated |
| Linux | Build stubs present, not yet validated |

## Key Files

| File | Purpose |
|------|---------|
| `pulp_gain.hpp` | Processor implementation: descriptor, parameters, audio processing |
| `vst3_entry.cpp` | VST3 format entry point |
| `au_v2_entry.cpp` | Audio Unit v2 entry point |
| `au_register.cpp` | AU component registration |
| `clap_entry.cpp` | CLAP format entry point |
| `main.cpp` | Standalone application entry point |
| `test_pulp_gain.cpp` | Unit tests exercising the processor through the headless host |
| `Info.plist.au` | AU bundle metadata |
| `Info.plist.vst3` | VST3 bundle metadata |
| `moduleinfo.json` | VST3 module info |
| `CMakeLists.txt` | Build configuration using `pulp_add_plugin()` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 1 | Input Gain | dB | -60 to 24 | 0 |
| 2 | Output Gain | dB | -60 to 24 | 0 |
| 3 | Bypass | (toggle) | 0 to 1 | 0 |

## Known Limitations

- No GUI. The processor is headless; UI is tested separately in the ui-preview example.
- dB-to-linear conversion is computed per-block, not per-sample smoothed. This may produce zipper noise during fast automation.

## Related Examples

- [PulpEffect](pulp-effect.md) -- adds more parameter types and biquad DSP
- [PulpTone](pulp-tone.md) -- adds MIDI input and polyphonic voice handling
- [UI Preview](ui-preview.md) -- demonstrates the view/widget system using PulpGain-like parameters
