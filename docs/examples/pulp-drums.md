# PulpDrums

**Category**: validation
**Type**: MIDI Effect
**Path**: `examples/PulpDrums/`

## Summary

A generative drum sequencer that outputs MIDI note events. The first Pulp example that produces MIDI output. Validates MIDI effect processing, pattern sequencing, and the `produces_midi` capability. Currently built only as a CLAP plugin.

## What It Demonstrates

- MIDI output: generating note-on events and writing them to `midi_out`
- The `produces_midi = true` descriptor flag
- 16-step drum pattern sequencing with four tracks (kick, snare, hi-hat, clap)
- GM drum map note numbers (channel 10)
- Four built-in patterns: basic 4/4, breakbeat, sparse, and dense
- Swing timing (offset applied to even steps)
- Density-based pattern modulation (probabilistic step skipping and addition)
- Velocity randomization
- Audio pass-through while generating MIDI (acts as a MIDI insert effect)
- Simple PRNG for deterministic randomization

## Supported Formats

| Format | Supported |
|--------|-----------|
| VST3 | No |
| AU v2 | No |
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
| `pulp_drums.hpp` | Processor with 16-step sequencer, pattern storage, and MIDI output generation |
| `clap_entry.cpp` | CLAP format entry point |
| `test_pulp_drums.cpp` | Unit tests for pattern generation and MIDI output |
| `CMakeLists.txt` | Build configuration using `pulp_add_plugin()` |

## Parameters

| ID | Name | Unit | Range | Default |
|----|------|------|-------|---------|
| 200 | Tempo | BPM | 60 to 240 | 120 |
| 201 | Swing | (amount) | 0 to 1 | 0 |
| 202 | Density | (amount) | 0 to 1 | 0.5 |
| 203 | Velocity | (value) | 1 to 127 | 100 |
| 204 | Pattern | (stepped) | 0 to 3 | 0 |
| 205 | Randomize | (amount) | 0 to 1 | 0 |

## Known Limitations

- CLAP only. MIDI effect support in VST3 and AU adapters is not yet validated for output.
- Uses its own internal tempo clock rather than reading host transport tempo.
- Note-off events are not generated; relies on the receiving instrument to handle one-shot drum samples.
- The PRNG is deterministic but not reset between prepare calls, so randomization state carries across sessions.

## Related Examples

- [PulpTone](example-pulp-tone.html) -- MIDI input (note-on/off consumption) rather than MIDI output
- [PulpSynth](example-pulp-synth.html) -- another CLAP-only example, focused on synthesis
