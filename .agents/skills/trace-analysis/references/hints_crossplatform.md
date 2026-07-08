# Cross-platform hints — reading a trace by where it came from

The same `pulp trace explain` surface works across standalone, plugin-in-DAW,
iOS/iPadOS AUv3, Android/Oboe, and the iOS Simulator. But a trace reads
differently depending on where it was captured. Establish the surface first —
it changes what is trustworthy and what is even present.

## Standalone host

The friendliest capture: the process is yours, shutdown is graceful (cmd-Q or
`pulp trace stop`), so the ring flushes cleanly and the full session is present.
Startup, frame, and offline-DSP questions all work here. This is the surface to
use for reproducible docs/CI captures.

## Plugin-in-DAW (VST3 / AU / CLAP)

- **Flush depends on teardown.** There is no cmd-Q. The trace flushes on the
  format adapter's destroy path (VST3 `terminate`, AU dealloc, CLAP `destroy`)
  or `Trace.stopSession`. A **host crash loses the in-memory ring** — inherent
  to the in-process backend. If a trace is empty after a host crash, that is
  expected, not a bug.
- **N instances, one process.** DAWs load many plugin instances in a single
  process; tracing is process-global (one session, ref-counted). A trace can
  therefore contain spans from *several* instances — disambiguate by thread /
  track, not by assuming one plugin.
- **Do not expect live-`process()` DSP spans.** Perfetto is off the live audio
  thread. In-DAW DSP cost is the fixed-slot telemetry path, not a `.pftrace`.

## iOS / iPadOS AUv3

- App-extension sandbox: the `.pftrace` output path must be somewhere the
  extension can write; the default temp dir may differ from the host app's.
- Teardown/flush rides the AUv3 deallocation, same contract as desktop AU.
- Touch/Pencil-driven interactions are the usual frame-jank trigger here;
  capture `render,layout,canvas,text,gpu` and correlate with a motion trace.

## Android / Oboe

- **atrace interleave (the free win).** Perfetto *is* the Android system tracer,
  so Pulp `dsp`/`frame`/`gpu` spans interleave with the platform's atrace
  (SurfaceFlinger, binder, the Oboe callback) in **one** timeline at zero extra
  cost. You can follow a blocker from a Pulp span straight into a system span —
  e.g. a present stall into SurfaceFlinger, or an Oboe callback into the audio
  HAL. Use this; it is unavailable on other platforms.
- Audio runs on the Oboe callback thread — name-check the thread before
  attributing DSP cost.

## iOS Simulator

- Rendering and audio paths differ from device (software GPU, simulated audio
  device). GPU-pass timing is **not** representative of on-device performance —
  treat Simulator `gpu` spans as functional, not as a perf baseline.
- Useful for frame-pipeline logic and startup structure; not for GPU-ms or
  real-time audio characterization.

## Cross-cutting: thread naming and sample-position args

- **Name the threads.** Attribute cost by `thread.name` (audio callback vs UI vs
  main), joining on stable `utid` (never raw `tid` — see `trace-sql`). A cost on
  "the wrong thread" is often the real finding.
- **Sample-position args, not wall-clock alignment.** DSP spans carry
  `position_samples` / block index as span args. You **cannot** infer
  sample-accurate musical position from Perfetto's monotonic timestamps (offline
  bounce, looping, and clock jumps break the mapping). Read the sample position
  from the arg via `EXTRACT_ARG`; never present a wall-clock span as aligned to
  the DAW's sample timeline.

## Traps

- **Empty trace on a DAW capture** → almost always a host crash (ring lost) or
  the session never stopped. Confirm the teardown/flush path fired before
  concluding the code was fast.
- **Comparing perf across surfaces** → don't. A Simulator or CPU-only build
  trace is not comparable to a device/GPU build. State the surface in the
  answer.
