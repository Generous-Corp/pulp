# D4: adapters publish neutral timing facts

Status: **PARTIAL**. CLAP, VST3, AU v3 and LV2 publish the facts and the host
projection consumes them; AU v2 does not publish them yet (see "Remaining
work"). A graph runtime consumes the projection through
`host::HostTimelineBinding` (`S4-reset-transaction.md`).

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
| AU v2 | not published | not published | not published |

In every adapter a block that skips the Processor (bypass, a refused layout,
or state-restore contention), or a host block longer than the prepared maximum
of which only the prefix was rendered, marks the next observed block
discontinuous only when that block has no steady counter to measure the skip.
With a counter, the projection sees the skip as a gap in the same epoch. AU v3
rejects an oversized block instead of clamping it and counts the rejection as
a skip; its `-reset` also invalidates the multi-output render cache, so a
repeated time stamp after a reset renders afresh.

The legacy `transport_jump` / `should_reset_dsp_state()` flags are unchanged.

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
- `examples/process-timing-consumer`: an installed-SDK consumer of the public
  headers, also built in-tree as `process-timing-public-consumer`.

## Remaining work

1. **AU v2.** `AUEffectBase::Render` can call `ProcessBufferLists` several
   times per render for scheduled parameter slices, so the steady counter and a
   playing transport must advance by each slice's offset inside the render.
   The effect path also has no timestamp at `ProcessBufferLists`; it needs the
   render timestamp stashed in `PulpAUEffect::Render`. Then: publish the
   `CallHostTransportState` position from
   `apply_host_callbacks_to_process_context()`, the slice-adjusted
   `mSampleTime` from the effect, instrument and MIDI-processor render paths,
   and a discontinuity from `Initialize()` and `Reset()` in all three classes,
   with traces through `test_au_plugin_state.mm` covering a sliced render.
2. **Graph consumption.** Done by `host::HostTimelineBinding`: it owns the
   `HostBlockTimeline`, maps a timeline `NewEpoch` onto one reset transaction
   across the graph ingress, the voice bank and staged parameters, and maps
   each window onto the graph's own epoch and cursor. See
   `S4-reset-transaction.md`. Adapters still do not drive the binding
   themselves; a graph-owning Processor does.
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
