# G-11 recording and atomic sample publication

`examples/g11-recording-publication` is the G-11 live sampling boundary
fixture. It uses only installed public headers and libraries:

1. A `SignalGraph` input feeds a `Processor` whose `process()` calls
   `RealtimeSampleRecorder::process()` and copies the input to the graph output.
2. A queued `Start` command with a fixed frame bound ends in the recorder's
   `Completed` state. The callback never opens a file or publishes a sample
   slot.
3. After graph processing stops, the control side calls
   `publish_completed_recording_to_slot()`. `SampleSlotBank` copies the complete
   recording into a prepared slot and publishes a generation-tagged immutable
   `PublishedSampleView`.
4. A second graph owns a `Processor` that snapshots the published view during
   `prepare()`, then reads it with `LoopPlaybackCursor` and `LoopReader` in the
   callback. The view is retained until the graph stops and the generation is
   acknowledged.

## Acceptance evidence

The consumer renders 64 known input frames under both a regular 16/16/16/16
schedule and an irregular 7/13/5/19/3/11/6 schedule. An independent scalar
sine oracle checks every captured and played sample. The same run checks
invalid recorder configuration, an idle stop refusal, reset, completed-state
publication, and generation validity. Three planted controls must fail:

- `--planted-wrong-buffer` changes the expected sample oracle.
- `--planted-timing` shifts the expected frame chronology by one sample.
- `--planted-stale-publication` replaces the slot and verifies the old
  generation is invalid and returns no channel pointer.

The focused CMake registration is isolated in
`test/cmake/g11_recording_publication_tests.cmake`; the static sample-region
cookbook remains a separate recipe and is not reused as recording evidence.

## Boundary

This fixture proves deterministic offline graph capture, stopped control-side
publication, immutable generation reads, and captured-sample playback. It does
not establish audio-device permissions, callback file I/O, realtime deadline
margins, graph serialization of a `Processor` node, or a complete production
sampler. Those claims require separate contracts and receipts.
