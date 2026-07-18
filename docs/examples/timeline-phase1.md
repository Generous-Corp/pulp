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
does not widen or redesign that channel. The persistent pattern is lowered into
a timeline `NoteContent` clip, compiled into `ArrangementNoteRenderer`, injected
through the binding's stable `MidiInput`, and routed to a small audible sine
destination inside the same `SignalGraph`. Its one-bar transport loop repeats
the arrangement and flushes active notes at discontinuities and stop. This
minimal standalone has a fixed pattern and no editor; the channel publishes its
coherent grid snapshot and live playhead without implying a second timeline
state model.

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
headless construction, variable block sizes, one-wrap split blocks, audible
note delivery, stop-time note flushing, and allocation-free processing after
prepare. The two standalone binaries also register their `--headless` paths as
CTest cases so their real entry points cannot silently rot.
