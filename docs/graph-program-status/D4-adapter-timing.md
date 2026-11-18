# D4: adapters publish neutral timing facts

Status: **PARTIAL**. CLAP, VST3, AU v3, AU v2 and LV2 publish the facts and
the host projection consumes them. A graph runtime consumes the projection
through `host::HostTimelineBinding` (`S4-reset-transaction.md`); see
"Remaining work".

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
| `refused_events` | Host events (notes, MIDI, SysEx, UMP) the adapter refused this block because their time did not place them inside the frames the Processor renders. |

The field is appended to `ProcessContext`, so existing offsets do not move,
and `refused_events` is appended to `ProcessTiming` for the same reason.

## Event-time admission

`format::classify_event_time()` places a host event time against the frames
the Processor renders: `InBlock`, `BeforeBlock`, `AtOrPastEnd`, or `WrongUnit`
(a time that is not a frame offset). `format::EventTimeAdmission` applies the
one policy every adapter uses: an in-block time is delivered at its offset,
unchanged; any other event is refused, counted on `refused_events`, and never
clamped into the block. A time is never read through an out-of-range offset,
and an in-block time that does not fit the delivered `int32` offset is refused
rather than truncated. Admission is per block, allocation-free and lock-free.

| Format | Events admitted | Time source and unit check |
|---|---|---|
| CLAP | note on/off/choke, MIDI, SysEx, note expression (MPE on), MIDI 2 (UMP on) | `clap_event_header_t::time`, widened from `uint32_t` rather than cast to `int32_t` |
| VST3 | note on/off, poly pressure, note expression (MPE on), SysEx with a payload, `IMidiMapping` controller points | `Event::sampleOffset` and the controller point offset |
| AU v3 | short MIDI and each MIDI event list (counted once) | `eventSampleTime` placed with `format::admit_au_event_time()`, the same `au_event_raw_offset()` arithmetic the host timeline uses; an absolute time with no valid `mSampleTime` is `WrongUnit` |
| LV2 | MIDI events in the input sequence | `time.frames` only when `format::lv2_sequence_unit_is_frames()` holds for the sequence unit (0 or `atom:frameTime`); a `time:Position` in another unit is placed at the previous event's frame |

The bound is the rendered frame count, so in an oversized host block whose
prefix alone is rendered (CLAP, VST3, LV2) an event in the unrendered tail is
refused too. Before this policy the AU v3 MIDI path copied `eventSampleTime`
as an offset without placing it; with a valid block time it is now placed on
the render axis, as AU v3 parameter events already were. Refusing a release
can leave a note held; `refused_events` is the fact a Processor reads to
reconcile its voices.

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
overflow refused). The AU v3 MIDI path admits through it and refuses
(`format::admit_au_event_time()`), as does the host path
(`au_event_block_time()`). AU v3 parameter events still use
`au_event_block_offset()`, which clamps an out-of-block offset to the nearest
edge and falls back to the event time when the block time is unusable.

## Evidence

- `test/test_process_timing.cpp`: event-time classification at every boundary
  (frames - 1 accepted, frames, negative and int64 extremes refused, a zero
  frame block, a wrong unit), admission counting, int32 overflow and
  saturation, AU placement before admission, and the LV2 unit rule. Builder
  semantics per format, SDK constant
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
  control), clock-source changes, a retired-epoch record refused. The adapter
  refuses out-of-block note times itself (at the end, and a time a cast to
  `int32_t` would have turned negative) and events in the unrendered tail of
  an oversized block, with an in-block positive control.
- `test/test_vst3_process_timing.cpp`: the real `PulpVst3Processor` through
  activation, processing restart, seek, cycle wrap, stopped transport, gap,
  `kContTimeValid` dropping out, a missing context, four partitions, and
  adapter refusal of negative, end, `INT32_MAX` and unrendered-tail offsets
  with an in-block positive control.
- `test/test_vst3_plugin_state.cpp`: the requirement mask now must include
  `kNeedContinousTimeSamples` because the decoder consumes it.
- `test/test_au_plugin_state.mm`: the real AU v3 render block through
  activation, fractional transport, invalid sample time, stopped transport,
  reset, bypass with and without a counter, oversized and output-less renders,
  and re-allocation; MIDI placed against the block time (last frame and an
  immediate offset accepted; end, before, huge and a small absolute time
  refused; an absolute time without a sample time refused).
  `test/test_au_multibus.mm` resets between two renders of one time stamp and
  requires a fresh Processor call.
- `test/test_lv2_adapter.cpp`: the real LV2 `run()` through activation and
  re-activation, half-speed playback projected in one epoch (and as a seek per
  block without the rate fact), a mid-block `time:Position`, partial and
  multiple Positions in one block, speed changes, stops and starts inside a
  block, numeric boundaries, a clamped tail, MIDI times refused outside the
  block, and a beat-time sequence refused whole (beat 0 shares its bytes with
  frame 0) beside an explicit `atom:frameTime` positive control.
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
2. **Graph consumption.** Done by `host::HostTimelineBinding`: it owns the
   `HostBlockTimeline`, maps a timeline `NewEpoch` onto one reset transaction
   across the graph ingress, the voice bank and staged parameters, and maps
   each window onto the graph's own epoch and cursor. See
   `S4-reset-transaction.md`. Adapters still do not drive the binding
   themselves; a graph-owning Processor does.
3. **Event times outside the MIDI paths.** CLAP, VST3, AU v3 and LV2 MIDI
   event times now go through one admission (see "Event-time admission").
   Not covered: parameter and modulation event offsets (CLAP and VST3 queue
   them as delivered; AU v3 parameter events still clamp through
   `au_event_block_offset()`), because refusing one would also drop its value
   from the store; and AU v2 MIDI, whose `inStartFrame` arrives before the
   render's frame count is known.
4. **Varispeed.** VST3, AU and CLAP carry no playback rate, so `NonUnit` is
   never published for them; a varispeed host still reads as a seek per block
   unless the host also supplies a steady counter. An LV2 relocation at
   non-unit speed is only detected when the host sets a new epoch some other
   way.
5. **Installed-SDK receipt.** The consumer is built in-tree on every gate run;
   the installed-prefix run is recorded in the PR body when it is made, not
   here.
