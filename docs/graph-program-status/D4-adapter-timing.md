# D4: adapters publish neutral timing facts

Status: **PARTIAL**. CLAP, VST3, AU v3, AU v2 and LV2 publish the facts and
the host projection consumes them; no graph runtime consumes them yet (see
"Remaining work").

## Rule

Format adapters publish facts; `pulp-host` owns the projection. An adapter
reads its native timing fields and writes `format::ProcessTiming` on
`ProcessContext::timing`. It never decides what is a seek, never groups blocks
into epochs, and never retimes events. A Processor or host binding that owns an
event timeline advances one `host::HostBlockTimeline` per block from the
context alone. Every other Processor can ignore the field, and nothing makes a
Processor use a graph.

`ProcessTiming` carries:

| Fact | Meaning |
|---|---|
| `has_steady_sample`, `steady_sample` | Host monotonic sample counter at the block's first frame, which keeps counting across seeks and loops. Samples only; never converted from wall-clock or host time. |
| `has_transport_sample`, `transport_sample` | Transport position in samples at the first frame. Equals `position_samples` when both are set. |
| `transport_moving` | The host reports the transport as moving. |
| `playback_rate` | `Unknown` (the host has no rate field), `Unit` (LV2 `time:speed` exactly 1) or `NonUnit`. A `NonUnit` transport is never compared with elapsed samples. |
| `discontinuity` | The adapter knows this block does not follow the last one the Processor saw. |

The field is appended to `ProcessContext`, so existing offsets do not move.

## Per-format sources

| Format | Steady counter | Transport | Discontinuity |
|---|---|---|---|
| CLAP | `clap_process_t::steady_time` when non-negative | `song_pos_seconds` converted from 2^31 fixed point with `clap_sectime_to_samples()` (exact for host-derived values; the same decode feeds `position_samples`) | first observed block after `clap_activate()` or `clap_reset()` |
| VST3 | `continousTimeSamples` when `kContTimeValid` is set and it is non-negative; `kNeedContinousTimeSamples` is now requested | `projectTimeSamples` whenever a context is supplied | first observed block after `setActive(true)` or `setProcessing(true)` |
| AU v3 | rounded `mSampleTime` when `kAudioTimeStampSampleTimeValid` is set, representable and non-negative | `transportStateBlock` position when the block returned YES | first observed block after `allocateRenderResources` or `-reset` |
| LV2 | none (LV2 defines no counter) | latched `time:frame` at the block start; each `time:Position` applies only the fields it carries, and a mid-block position is moved back along the piecewise speed path (a speed change applies from its own frame) | first `run()` after instantiation or `activate()`, or after a host block clamped to the prepared maximum |
| AU v2 | render `mSampleTime` (same validity and rounding as AU v3) plus the call's offset into the render, so each scheduled-parameter slice of an effect render starts where the previous one ended | `CallHostTransportState` position when the callback succeeds, advanced by the slice offset while playing | first observed call after `Initialize()` or `Reset()` (effect, instrument and MIDI processor) |

In every adapter a block that skips the Processor (bypass, a refused layout,
or state-restore contention), or a host block longer than the prepared maximum
of which only the prefix was rendered, marks the next observed block
discontinuous only when that block has no steady counter to measure the skip.
With a counter, the projection sees the skip as a gap in the same epoch. AU v3
rejects an oversized block instead of clamping it and counts the rejection as
a skip; its `-reset` also invalidates the multi-output render cache, so a
repeated time stamp after a reset renders afresh.

AU v2 marks a skip with a scope guard on every Processor call
(`ScopedSkipMark`), so any return before `process()` counts, and the effect
also marks a render that reached no Processor call at all (the stock
`kAudioUnitProperty_BypassEffect` path, or a missing or failed main input
inside `AUEffectBase::Render`). A request that `AUBase::DoRender` refuses
before it reaches the adapter (uninitialized, more frames than the maximum,
a wrong buffer count) is invisible to the adapter: with a counter the next
block measures it as a gap, without one it is reported as continuous.
Discontinuity is a generation the control thread bumps and the render thread
compares, so a `Reset()` that races a render, or a skipped block after a
reset, is never lost.

The legacy `transport_jump` / `should_reset_dsp_state()` flags are unchanged,
except that an AU v2 effect slice now reports the transport (samples, and
beats when the tempo is known) at the slice's first frame while playing.
Before, every slice after the first repeated the render's position, so a
playing transport read as a jump on each sliced render.

## Host projection

- `clock_from_process_context()` / `clock_from_process_timing()` map the facts
  onto `HostBlockClock`. `clock_from_vst3()`, `clock_from_au()` and
  `clock_from_lv2()` now map through the same `format::*_process_timing()`
  builders the adapters use, so the two cannot drift.
- `advance_from_context()` places a block on the timeline.
- `make_record_from_block_offset()` turns a signed adapter offset (for example
  `MidiEvent::sample_offset`) into a `HostEventRecord`; a negative offset or one
  at or past the block's frame count is refused with `OutOfRange`, never
  clamped.
- Blocks above `kHostBlockMaxFrames` (4096): `advance()` refuses a whole
  oversized block with `OutOfRange`. A caller that must accept one splits it
  with `host_block_chunk()` into contiguous chunks of at most 4096 frames; the
  steady counter (and a playing transport) advance by the chunk offset, only
  the first chunk carries the discontinuity, and the absolute samples equal
  those of the same samples delivered as separate host blocks.

## Shared AU event placement

`format::detail::au_event_raw_offset()` is the one AU v3 event placement
(immediate window, or absolute against the rounded block `mSampleTime`, with
overflow refused). The legacy MIDI path (`au_event_block_offset()`) still
clamps an out-of-block offset to the nearest edge and falls back to the event
time when the block time is unusable; the host path (`au_event_block_time()`)
refuses both. The policies stay separate; only the arithmetic is shared.

## Evidence

- `test/test_process_timing.cpp`: builder semantics per format, SDK constant
  mirrors, CLAP fixed-point round trip at 44.1/48/88.2/96 kHz across a day of
  audio (with a truncation negative control), unclocked default context, the
  shared AU raw offset under both policies, oversized-block refusal and
  chunk/partition equality, LV2 half speed (with a dropped-rate negative
  control), offset and stale-window refusals.
- `test/test_clap_process_timing.cpp`: the real CLAP adapter into a recording
  Processor, then `HostBlockTimeline` + `HostEventIngress`: activation and
  reset, five block partitions with identical absolute samples, playing seek,
  loop wrap, stopped transport and relocation, host gaps, adapter-skipped
  blocks with and without `steady_time` (with a stripped-fact negative
  control), clock-source changes, out-of-block offsets refused, a
  retired-epoch record refused.
- `test/test_vst3_process_timing.cpp`: the real `PulpVst3Processor` through
  activation, processing restart, seek, cycle wrap, stopped transport, gap,
  `kContTimeValid` dropping out, a missing context, four partitions, and
  out-of-block offsets.
- `test/test_vst3_plugin_state.cpp`: the requirement mask now must include
  `kNeedContinousTimeSamples` because the decoder consumes it.
- `test/test_au_plugin_state.mm`: the real AU v3 render block through
  activation, fractional transport, invalid sample time, stopped transport,
  reset, bypass with and without a counter, oversized and output-less renders,
  and re-allocation. `test/test_au_multibus.mm` resets between two renders of
  one time stamp and requires a fresh Processor call.
- `test/test_lv2_adapter.cpp`: the real LV2 `run()` through activation and
  re-activation, half-speed playback projected in one epoch (and as a seek per
  block without the rate fact), a mid-block `time:Position`, partial and
  multiple Positions in one block, speed changes, stops and starts inside a
  block, numeric boundaries, and a clamped tail.
- `test/test_au_v2_process_timing.cpp`: the real AU v2 effect through
  `AUBase::DoRender` with a host input callback and host transport callbacks,
  and the instrument and MIDI processor through `Render()`: activation, invalid
  sample time, stopped transport, missing transport callback, a sliced render
  (counter and transport advance per slice, no legacy jump), five slice
  partitions with and without a counter that place every frame at the same
  absolute sample (with an unadvanced-slice negative control), `Reset()` and
  re-initialization, the stock bypass and the Bypass parameter with and without
  a counter (with a no-skip negative control), a skip that must not consume a
  pending reset, and the MIDI processor's oversized render. No AU host loads
  these units; the SDK classes are constructed in process.
- `examples/process-timing-consumer`: an installed-SDK consumer of the public
  headers, also built in-tree as `process-timing-public-consumer`.

## Remaining work

1. **AU v2 in a real host.** The adapter traces run against SDK classes in
   process; no AU v2 host (Logic, auval with transport) has been driven here.
   The `DoRender` refusals above are unobservable by design.
2. **Graph consumption.** `SignalGraph::begin_host_event_block()` and
   `reset_host_event_ingress()` take the prepared trigger generation's epoch,
   not a `HostBlockTimeline` epoch. Mapping a timeline `NewEpoch` onto a graph
   ingress reset is the reset-transaction design in the S4 item; wire it there
   rather than here.
3. **Adapter event-time checks still owned by the legacy paths.** CLAP and
   VST3 copy event times into `sample_offset` without a bound check, and the
   AU v3 MIDI path still clamps. The projection refuses those offsets, but a
   Processor that reads `MidiBuffer` directly still sees them. LV2 still reads
   `time.frames` without checking the sequence unit.
4. **Varispeed.** VST3, AU and CLAP carry no playback rate, so `NonUnit` is
   never published for them; a varispeed host still reads as a seek per block
   unless the host also supplies a steady counter. An LV2 relocation at
   non-unit speed is only detected when the host sets a new epoch some other
   way.
5. **Installed-SDK receipt.** The consumer is built in-tree on every gate run;
   the installed-prefix run is recorded in the PR body when it is made, not
   here.
