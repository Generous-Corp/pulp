# PulpEffect

**Category**: validation
**Type**: Effect
**Path**: `examples/pulp-effect/`

## Summary

A biquad filter effect with frequency, resonance, filter type (lowpass/highpass/bandpass), dry/wet mix, and bypass parameters. Validates diverse parameter types and more complex DSP than PulpGain across the three plugin formats.

## What It Demonstrates

- Multiple parameter types: continuous (frequency, resonance, mix), stepped enum (filter type), boolean (bypass)
- Wide-range frequency parameter (20 Hz to 20 kHz)
- Biquad filter DSP with coefficient computation for lowpass, highpass, and bandpass modes
- Dry/wet mix blending
- Per-channel filter state management (supports up to 8 channels)
- The `release()` lifecycle method for clearing DSP state
- Parameter diversity validation across VST3, AU, and CLAP adapters

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | Yes |
| AU v2 | Yes |
| CLAP | Yes |
| Standalone | No |

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes |
| Windows | Build stubs present, not yet validated |
| Linux | Build stubs present, not yet validated |

## Key Files

| File | Purpose |
|------|---------|
| `pulp_effect.hpp` | Processor with biquad filter, coefficient computation, and mix parameter |
| `vst3_entry.cpp` | VST3 format entry point |
| `au_v2_entry.cpp` | Audio Unit v2 entry point |
| `clap_entry.cpp` | CLAP format entry point |
| `test_pulp_effect.cpp` | Unit tests for filter processing and state serialization |
| `Info.plist.au` | AU bundle metadata |
| `CMakeLists.txt` | Build configuration using `pulp_add_plugin()` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 1 | Frequency | Hz | 20 to 20000 | 1000 |
| 2 | Resonance | (Q) | 0.1 to 10 | 0.707 |
| 3 | Type | (stepped) | 0 to 2 | 0 (lowpass) |
| 4 | Mix | % | 0 to 100 | 100 |
| 5 | Bypass | (toggle) | 0 to 1 | 0 |

## Known Limitations

- Filter coefficients are recalculated per block rather than per sample. Fast frequency sweeps may produce audible artifacts.
- No standalone target is configured for this example.

## Related Examples

- [PulpGain](example-pulp-gain.html) -- simpler effect with fewer parameters
- [PulpCompressor](example-pulp-compressor.html) -- another effect example with sidechain and multi-bus support
