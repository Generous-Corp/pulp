# PulpCompressor

**Category**: validation
**Type**: Effect
**Path**: `examples/pulp-compressor/`

## Summary

A dynamics compressor with optional sidechain input. The first Pulp example with multiple input buses. Validates the multi-bus architecture, sidechain routing, and more complex DSP processing across three plugin formats.

## What It Demonstrates

- Multi-bus input configuration: main stereo input plus an optional sidechain bus
- Optional bus declaration (`true` flag on the sidechain bus descriptor)
- Sidechain detection: uses `sidechain_input()` API to access the second input bus when connected
- Automatic fallback to main input for detection when sidechain is not connected
- Envelope follower with separate attack and release coefficients
- Gain reduction computation based on threshold and ratio
- Makeup gain parameter
- No standalone target (standalone has no sidechain routing)

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | Yes |
| AU v2 | Yes |
| CLAP | Yes |
| Standalone | No (no sidechain routing in standalone) |

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes |
| Windows | Build stubs present, not yet validated |
| Linux | Build stubs present, not yet validated |

## Key Files

| File | Purpose |
|------|---------|
| `pulp_compressor.hpp` | Processor with multi-bus input, envelope follower, and gain reduction |
| `vst3_entry.cpp` | VST3 format entry point |
| `au_v2_entry.cpp` | Audio Unit v2 entry point |
| `clap_entry.cpp` | CLAP format entry point |
| `test_pulp_compressor.cpp` | Unit tests for compression behavior and sidechain routing |
| `CMakeLists.txt` | Build configuration using `pulp_add_plugin()` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 1 | Threshold | dB | -60 to 0 | -20 |
| 2 | Ratio | :1 | 1 to 20 | 4 |
| 3 | Attack | ms | 0.1 to 100 | 10 |
| 4 | Release | ms | 10 to 1000 | 100 |
| 5 | Makeup | dB | 0 to 30 | 0 |
| 6 | Bypass | (toggle) | 0 to 1 | 0 |

## Known Limitations

- No AU-specific validation test (auval) is configured, unlike PulpGain and PulpTone. Only CLAP dlopen and pluginval (VST3) validation tests are present.
- Peak detection is per-sample across channels, not RMS-based.
- No look-ahead or latency compensation is implemented despite the existing guide mentioning latency reporting.

## Related Examples

- [PulpEffect](example-pulp-effect.html) -- simpler single-bus effect
- [PulpGain](example-pulp-gain.html) -- the simplest effect example
