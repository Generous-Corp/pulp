# PulpTone

**Category**: validation
**Type**: Instrument
**Path**: `examples/pulp-tone/`

## Summary

A simple 8-voice polyphonic oscillator synth with MIDI input. Validates the instrument plugin path including audio output without audio input, MIDI note handling, and the parameter system across all four plugin formats.

## What It Demonstrates

- Instrument plugin configuration (no input buses, stereo output, `accepts_midi = true`)
- MIDI note-on and note-off processing with sample-accurate event scheduling
- 8-voice polyphony with voice stealing
- Sine, saw, and square waveform generation
- Simple attack/release envelope per voice
- Infinite tail length declaration for synth plugins (`tail_samples = -1`)
- Building instrument targets for VST3 (`aumu`), AU, CLAP, and Standalone

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | Yes |
| AU v2 | Yes |
| CLAP | Yes |
| Standalone | Yes |

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes |
| Windows | Build stubs present, not yet validated |
| Linux | Build stubs present, not yet validated |

## Key Files

| File | Purpose |
|------|---------|
| `pulp_tone.hpp` | Processor with polyphonic voice engine, oscillator, and envelope |
| `vst3_entry.cpp` | VST3 format entry point |
| `au_v2_entry.cpp` | Audio Unit v2 entry point |
| `clap_entry.cpp` | CLAP format entry point |
| `main.cpp` | Standalone application entry point |
| `test_pulp_tone.cpp` | Unit tests for MIDI processing and audio output |
| `Info.plist.au` | AU bundle metadata |
| `CMakeLists.txt` | Build configuration using `pulp_add_plugin()` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 10 | Waveform | (stepped) | 0 to 2 | 0 (sine) |
| 11 | Volume | dB | -60 to 0 | -6 |
| 12 | Attack | ms | 1 to 1000 | 10 |
| 13 | Release | ms | 1 to 2000 | 200 |

## Known Limitations

- No anti-aliasing on saw and square waveforms. This is intentional for simplicity; PulpSynth uses polyBLEP oscillators from the signal library for alias-free output.
- Voice stealing always takes the first voice slot rather than using a smarter allocation strategy.
- The envelope is a simple linear attack/release, not a full ADSR.

## Related Examples

- [PulpGain](example-pulp-gain.html) -- simpler starting point for effects
- [PulpSynth](example-pulp-synth.html) -- more advanced synth using the signal DSP library with ADSR, filter, and detune
- [PulpDrums](example-pulp-drums.html) -- MIDI output instead of MIDI input
