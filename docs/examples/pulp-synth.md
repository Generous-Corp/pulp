# PulpSynth

**Category**: validation
**Type**: Instrument
**Path**: `examples/PulpSynth/`

## Summary

A macro oscillator synthesizer with filter, full ADSR envelope, oscillator detune, and master gain. This is the most complex example and validates integration with the `pulp::signal` DSP library. Currently built only as a CLAP plugin.

## What It Demonstrates

- Integration with `pulp::signal` DSP processors: `Oscillator` (polyBLEP), `Svf` (TPT state-variable filter), `Adsr`, `SmoothedValue`, `Gain`
- Dual-oscillator voice architecture with detuning for timbral richness
- Filter envelope modulation (cutoff modulated by ADSR level)
- Full ADSR envelope (attack, decay, sustain, release) rather than simple attack/release
- Parameter smoothing via `SmoothedValue` for gain and cutoff
- 10 parameters covering oscillator, filter, envelope, and output sections
- 8-voice polyphony with voice stealing

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | No (planned) |
| AU v2 | No (planned) |
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
| `pulp_synth.hpp` | Processor using signal library DSP: Oscillator, Svf, Adsr, SmoothedValue |
| `clap_entry.cpp` | CLAP format entry point |
| `test_pulp_synth.cpp` | Unit tests for synthesis and signal library integration |
| `CMakeLists.txt` | Build configuration; links `pulp::signal` in addition to `pulp::format` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 100 | Waveform | (stepped) | 0 to 3 | 1 (saw) |
| 101 | Detune | ct (cents) | -100 to 100 | 0 |
| 102 | Cutoff | Hz | 20 to 20000 | 5000 |
| 103 | Resonance | (Q) | 0.1 to 10 | 0.707 |
| 104 | Filter Env | (amount) | 0 to 1 | 0.5 |
| 105 | Attack | s | 0.001 to 2 | 0.01 |
| 106 | Decay | s | 0.001 to 2 | 0.1 |
| 107 | Sustain | (level) | 0 to 1 | 0.7 |
| 108 | Release | s | 0.001 to 5 | 0.3 |
| 109 | Gain | dB | -60 to 12 | -6 |

## Known Limitations

- CLAP build fails with a missing `pulp/signal/oscillator.hpp` include path. This is a known issue tracked in CLAUDE.md. Exclude from test runs with `ctest -E PulpSynth`.
- Only CLAP format is configured. The CMakeLists.txt notes VST3 and AU will be added when validated.
- No standalone target.

## Related Examples

- [PulpTone](example-pulp-tone.html) -- simpler synth without the signal library, implements oscillators inline
- [PulpDrums](example-pulp-drums.html) -- another CLAP-only example, focused on MIDI output
