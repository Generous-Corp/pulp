# Creative Timeline Engine Phase 1 examples

These two examples prove the smallest useful Creative Timeline Engine through
the same runtime path a desktop product uses. Both create a real immutable
`Project`, compile a `PlaybackProgram`, advance `MasterTransport`, and render
through `TimelineGraphPlaybackBinding` into `SignalGraph`. The headless tests and
standalone executables share the same processor classes; neither substitutes a
mock renderer.

## Audio-file player

`pulp-timeline-audio-player` reads a complete WAV on the control thread. The
bounded, no-exceptions `DecodedAudioAssetPool::decode_wav` path validates and
decodes it before the processor is started. The project contains one asset, one
sequence, one track, and one absolute `MediaRef` clip. Play, stop, seek, and loop
are changes to `MasterTransport`; audio remains a compiled arrangement track.

```bash
cmake --build build --target pulp-timeline-audio-player
./build/examples/timeline-phase1/pulp-timeline-audio-player ./loop.wav
```

Its deterministic device-free validation path is:

```bash
./build/examples/timeline-phase1/pulp-timeline-audio-player --headless
```

## Step-pattern player

`pulp-timeline-step-sequencer` owns the existing frozen
`SequencerStateChannel` for its fixed-capacity grid and UI-facing playhead. It
does not widen or redesign that channel. The channel-owned snapshot is persisted
as the registered `pulp.examples.timeline.step_pattern` typed document content.
Its control-thread pump drains and reduces queued edits, updates that component,
lowers the resulting grid into a timeline `NoteContent` clip, and publishes the
recompiled program. `ArrangementNoteRenderer` then injects notes through the
binding's stable `MidiInput` to a small audible sine destination inside the same
`SignalGraph`. Its one-bar transport loop repeats the arrangement and flushes
active notes at discontinuities and stop. The minimal standalone has no editor,
but its processor exposes the real channel and edit/recompile pump a view would
drive; edits are not mirrored into a second grid model or ignored.

```bash
cmake --build build --target pulp-timeline-step-sequencer
./build/examples/timeline-phase1/pulp-timeline-step-sequencer
```

The device-free validation path is:

```bash
./build/examples/timeline-phase1/pulp-timeline-step-sequencer --headless
```

## Focused tests

The `pulp-test-timeline-phase1-examples` suite covers bounded decode rejection,
variable block sizes, one-wrap split blocks, registered-pattern persistence,
command-to-recompile output changes, audible note delivery, stop-time note
flushing, and allocation-free processing after prepare. It also drives both
real processors through `StandaloneApp`'s device-independent callback seam and
requires nonzero output; unavailable hardware cannot turn the test green. The
two standalone binaries register their `--headless` paths as CTest cases so
their real entry points cannot silently rot.
