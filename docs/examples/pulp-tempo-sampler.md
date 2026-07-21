# PulpTempoSampler

`PulpTempoSampler` demonstrates offline tempo matching followed by resident,
generation-safe playback. It detects a loop tempo and onsets, renders the
time-stretched result away from the audio callback, and atomically publishes
the sample with its matching fixed-capacity slice regions.

This example uses conventional `signal::OfflineStretch`; it does not consume a
Sample Heritage profile. Heritage fixed/adaptive commit stretch is the separate
choice when audible cyclic resynthesis is itself the intended character.

The example is version 1.7.1 and builds as CLAP, VST3, standalone, and AU v2
on Apple platforms. Its bundle identifier, parameter IDs, and plugin-state v2
schema remain stable.

## Architecture

- `BuiltInKeyTempoAnalyzer` supplies the initial tempo estimate; the example
  retains its bar-exact loop policy.
- `OnsetDetector` and `SlicePointAnalyzer` select the strongest bounded cuts
  and snap shared boundaries to sign transitions.
- `SampleKeyMap` owns root-relative slice indices and chromatic pitch ratios.
- `OfflineStretch` renders on the worker side.
- `PublishedSampleStore` retains audio generations while
  `TripleBuffer<TempoSamplerPublishedSource>` publishes the matching sample
  view and slice-region array to the callback without allocation or locking.
- `LoopRenderer` provides per-voice traversal. The example deliberately keeps
  its fixed-first-voice stealing, sustain and held-loop behavior, panic
  handling, play-through rules, and dynamically updated `signal::Adsr`.

This is a resident live-playback design. It does not use the shared page cache
after the offline render; long-source streaming products should start with the
[sampler playback chooser](../guides/sampler-playback.md).

## Build and test targets

Configure Pulp with tests enabled, then build only the desired targets:

```bash
cmake --build build --target PulpTempoSampler_CLAP
cmake --build build --target pulp-tempo-sampler-test
```

The focused test target covers tempo/slice publication, MIDI and UI triggers,
fixed voice stealing, sustain, loop re-engagement, one-shot and gate behavior,
panic, root mapping, out-of-range slice notes, state-v2 round trips, and a
steady-state realtime allocation/lock probe.

## State and automation

Seventeen existing parameters define gain, ADSR, tempo link, pitch/formant,
quality, loop and direction policy, root note, onset sensitivity, playback
mode, gate, and Flex Speed. The serialized plugin payload stores the raw loop
audio and the engaged target tempo in schema v2; slices and detected tempo are
derived again when restored.
