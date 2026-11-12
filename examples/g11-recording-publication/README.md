# G-11 recording and atomic publication

This installed SDK consumer wires the public `SignalGraph` processor path to
`RealtimeSampleRecorder`, publishes the completed capture with
`publish_completed_recording_to_slot`, and renders it through
`LoopPlaybackCursor` and `LoopReader`. The recorder is the only callback-side
writer. Publication and generation acknowledgement happen after graph
processing has stopped, so no callback performs allocation, locking, or file
I/O.

Configure this directory against the exact Release SDK prefix built from the
same Pulp source checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk
cmake --build build --target pulp-g11-recording-publication
ctest --test-dir build --output-on-failure
```

The positive case captures the same 64-frame input under regular and irregular
block schedules, checks the passthrough capture, publishes an immutable sample
view, and plays it through a second graph. It also checks invalid preparation,
idle stop refusal, reset, republication, and generation validity. The three
planted controls feed a wrong buffer, a one-frame timing shift, and a stale
generation view to the same oracle; each passes only when the oracle rejects
it and prints the `G11 planted defect detected:` marker CTest requires.

The `--two-take` case keeps one prepared graph alive while it captures and
publishes a 440 Hz take, loops voice A, captures a 660 Hz take under an
irregular schedule, and publishes voice B while A remains held. It then
releases and acknowledges A's retired generation before checking B alone under
a regular schedule. Slot exhaustion, stale-generation reads, and idle-command
refusal are negative controls; exact scalar 440/660 Hz oracles check the
captured and sounding samples.

This is a bounded offline graph fixture. It does not claim device capture,
callback file I/O, realtime deadline qualification, graph serialization of
processor-owned recorder state, or a complete sampler UI.
