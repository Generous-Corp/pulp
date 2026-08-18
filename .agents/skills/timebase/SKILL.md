---
name: timebase
description: Pulp musical/media time primitives, exact beat divisions, tempo and meter maps, transport-range grid projection, order-preserving groove kernels, coordinate randomness, streaming cursors, and quantization arithmetic.
---

# Timebase

Use this skill when changing `core/timebase`, tempo conversion, or the transport
quantizer's beat/frame arithmetic.

## Contracts

- `kTicksPerQuarter` is 705,600. Musical positions are stored and accumulated as
  integer `TickPosition`; samples use integer `SamplePosition`. Tick-position,
  duration, and `MonotonicBeat` arithmetic saturates at the signed 64-bit
  endpoints; it must not invoke signed-overflow UB.
- `TempoMap` and `MeterMap` are editable document values built through
  nonthrowing factories. Their first point is tick zero and points are strictly
  ordered. Meter changes must compile on exact preceding bar boundaries.
- `CompiledTempoMap` is immutable and sample-rate-specific. Construct it only
  through `CompiledTempoMap::compile()` and handle `TempoMapError`; public
  throwing construction is forbidden. Its first tempo point
  is tick zero, points are strictly ordered, and BPM is finite in `[1, 1000]`.
- Tempo ramps are BPM-linear in tick position. Integrate them analytically; do not
  approximate ramps block-by-block or accumulate floating-point deltas.
- Every segment begins at an integer sample anchor. `samples_to_ticks()` returns
  the first canonical tick mapping to that sample when one exists. Exact sample
  -> tick -> sample requires a tick grid at least as dense as samples. On a
  sparser grid, use `resolve_sample()` and inspect `represented_sample`,
  `absolute_error_samples`, and `exact`; the nearest tick is returned.
- Arbitrary tick -> sample -> tick cannot be identity because many ticks share an
  integer sample. Test monotonicity and canonical-sample preservation instead.
- Render-time phase mapping may use `fractional_ticks_to_samples()` and its
  analytic inverse `fractional_samples_to_ticks()`. Both retain the compiled
  integer segment anchors but avoid rounding their input/output domains; keep
  ramp round-trip coverage at fractional interior positions.
- `TempoCursor` is the allocation-free playback path. Monotonic sample advances
  consume segment transitions once (amortized O(1)); seeks and loop wraps reset
  it explicitly. Differential tests must match cold-map canonical results.
- `CompiledMeterMap` uses zero-based bars and exact integer bar/tick conversion.
  Tempo changes never affect bar conversion and meter changes never affect
  tick/sample conversion. Conversion is total across `INT64_MIN..INT64_MAX`:
  exact results are returned when representable and out-of-range results
  saturate without signed-overflow UB.
- Keep `TransportQuantizer`'s public behavior stable. Generic beat/frame/grid
  arithmetic belongs in `<pulp/timebase/quantize.hpp>` and the format wrapper
  delegates to it.
- `BeatDivision` is an append-only persisted ordinal vocabulary. Append new
  values immediately before `Count`, assign every ordinal explicitly, and keep
  `beat_fraction()` reduced. `division_ticks()` must fail if a future fraction
  is not exactly representable on the 705,600-tick quarter-note lattice.
- `BeatDivision` owns the canonical fraction table. The older
  `signal::units::Division` vocabulary is a compatibility adapter: preserve its
  lowercase public spellings and persisted ordinals, map it to `BeatDivision`,
  and derive its beat values from `beat_fraction_or()`. Append both enums in the
  same change and keep exhaustive compile-time and runtime parity coverage in
  `test_signal_units.cpp`; never add a second division formula in signal.
- Grid projection consumes explicit document and monotonic anchors in
  `GridProjectionRange`; it does not infer transport state from an unwrapped
  sample clock. This matches `playback::MasterTransport`: pre-loop material uses
  its ordinary document interval, loop passes repeat the loop's document sample
  interval and tempo image, and seeks change the document anchor without
  resetting `MonotonicBeat`. Ranges and callbacks are half-open, so splitting a
  callback cannot duplicate a boundary. Keep capacity and signed-domain failure
  explicit and leave caller output untouched on insufficient capacity. Bound
  candidate opportunities before entering either timeline- or bar-grid loops;
  counting only emitted points leaves incoherent remote-sample ranges able to
  burn unbounded callback time.
- For document-clock projection, enumerate the rounded end tick as a candidate
  and let `[timeline_sample_start, timeline_sample_start + frame_count)` decide
  ownership. Sparse maps can give a valid one-frame transport range equal
  rounded tick endpoints (at 1 BPM/48 kHz, tick 0 maps to sample 0 and tick 1 to
  sample 4). Returning early drops tick 0 from a `1 + 3` split even though a
  four-frame block emits it; excluding the end candidate merely moves the bug.
  The next range's sample filter prevents duplication.
- A host-beat-mapped transport range carries fractional host tick endpoints and
  maps an exact document tick proportionally into output frames. Preserve that
  metadata in the dependency-lower grid range and match playback's half-open,
  floor-to-frame rule. Range-local proportions are not callback invariant when
  a loop boundary's output count was rounded. Retain one `HostGridAnchor`
  (normalized source tick, absolute frame, ticks per frame) across the continuous
  session interval, give each range its absolute first frame and loop-pass
  document-to-source offset, and floor on that stable clock before clamping to
  the owning half-open range. Initialize the source tick from the first resolved
  range in a normalization epoch, not from an absolute host beat that the
  transport has already wrapped into document coordinates; reset the anchor on
  an epoch or slope discontinuity. Never feed such a range through
  `CompiledTempoMap::ticks_to_samples()`: session tempo is independent of the
  document tempo, including on split loop ranges.
- `project_ratchet_interval()` treats the hit count as including the onset and
  excludes the later clock boundary. It distributes integer-tick remainders
  from the original interval coordinates on every projection; do not advance a
  floating-point phase or carry remainder state between callbacks. Half-open
  windows must concatenate to the same schedule as one whole-window call. Reject
  a hit count greater than the integer-tick span rather than emitting duplicate
  positions.
- `LoopRegion` (`<pulp/timebase/loop_region.hpp>`) is two document positions plus
  whether they are in force, and it lives here rather than beside a consumer
  because that is the whole of it. `playback::LoopRegion` is an alias of it and
  `timeline_editor::UiPlayhead::loop` names it directly, so the rung that runs
  the transport and the rung that draws the ruler cannot drift apart. `enabled`
  gates wrapping, not existence: a disabled loop keeps its bounds so a view goes
  on drawing the region and re-enabling returns the user to it.
- **A value type both the transport rung and the editor rung need belongs here,
  and this module is the only place it can go.** `playback`'s floor and
  `timeline_editor`'s floor exclude each other; `timebase` is in both, so it is
  their entire intersection apart from `platform`/`runtime`. Reaching for a
  shared home anywhere else means widening a floor row, which is the thing the
  ladder exists to prevent. Before adding one, confirm the intersection still
  holds in `MODULE_FLOORS` (`timeline_engine_dependency_floor_check.py`) rather
  than assuming it.

## Swing

`swing_position()` moves the interior boundary of every pair of grid cells to an
exact rational fraction of the pair and rescales the material on either side
onto the new halves. Ticks are integers, so the two halves land on integer
ranges of different lengths and one of them **compresses**: distinct input ticks
can map to the same output tick. The map is therefore *not* a bijection, and
claiming a lossless round trip would be wrong. What holds, and what to assert:

- pair boundaries are exact fixed points, and the grid point inside a pair maps
  exactly onto the pivot;
- the map is non-decreasing, so material is never reordered;
- at `kStraightSwing` it is the identity on every tick, bit for bit — the
  general path produces it, so there is no early-out hiding a bug;
- `unswing_position()` recovers a position only to within
  `grid / (2 * min(pivot, pair - pivot)) + 1/2` ticks. That bound is tight: an
  exhaustive sweep of a pair hits its floor at every swung ratio.

State the bound rather than exactness. `SwingRatio` is a `{numerator,
denominator}` rational and not a double on purpose — the result is a
document-visible tick that two machines must agree on, and a float ratio makes
that agreement depend on how one division rounds.

The pivot is clamped inside the pair. A coarse grid does not have enough ticks
to express an extreme ratio, and without the clamp the pivot would round onto a
pair boundary and erase half the warp while still reporting a valid setting.
An invalid grid or ratio makes both functions the identity: the caller
validates, and a bad setting must not silently move music.

## Groove projection and coordinate randomness

The canonical authored groove remains `timeline::GrooveTemplate`: it owns the
name, persistence, independent swing/table grids, strengths, and 0..4x accent
domain. `timebase::OrderPreservingGrooveKernel` is deliberately narrower: a
fixed-capacity, allocation-free projection of the non-reordering subset. Keep
its numeric domains aligned with timeline (strength 0..1000, velocity 0..4000,
at most 1024 steps), but do not call it a template or add a second persistence
model. Timing strength scales both swing and table displacement; zero must be
exact identity, including swing. Validate the joint swing/table period
within the documented bound and reject either reorder or an unbounded period.

`coordinate_random()` is a pure hash of seed plus stable musical coordinates
(tick, lane, loop pass/cycle, stream). Never replace it with callback-local RNG
state. Golden vectors pin the hash, callback-partition tests pin coordinate
selection, and `coordinate_chance()` multiply-high reduction is checked against
an independent wide/shift-add oracle in tests.

## Validation

Build and run `pulp-test-timebase`, `pulp-test-timebase-groove-kernels`, and the
existing `pulp-test-transport-quantizer` oracle. Keep at least 1,000,000
deterministic randomized constant/ramp cases, plus tempo-point boundary cases.
Grid tests must adapt real `MasterTransport` ranges and cover pre-loop playback,
variable-tempo repeated passes, callback partitions, and a seek whose monotonic
anchor is intentionally independent. Also adapt `begin_tempo_synced_block()`
ranges with fractional host endpoints, a session/document tempo mismatch, and a
loop split. Pin the 180 BPM/48 kHz regression at source beat 0.93748125: tick
29400 stays on absolute frame 1666 for one 4800-frame callback and a 1500 + 3300
split. Exercise signed ceiling at `INT64_MIN` and prove huge incoherent
tick ranges fail the candidate preflight before enumeration. Include a real
MasterTransport sparse-map whole-vs-partitioned oracle (`4` frames versus
`1 + 3` and finer splits at 1 BPM/48 kHz).

`MonotonicBeat` is the strong type for the transport's non-looping musical
clock; the transport owns how it advances while timeline positions seek or
wrap.

## Never use `__int128` for wide intermediates

MSVC does not support `__int128` on **any** architecture, so it is not a
portability question about 32-bit targets — it fails on `windows-x64` and
`windows-arm64` alike with `error C4235`, and a header that uses it cascades
into `C4430` / `C2059` / `C2064` at every use site. Clang and GCC accept it,
so macOS and Linux build clean and the break surfaces on the Windows leg of
the release matrix, where a failure means the GitHub release object and its
assets are never produced.

Use the portable saturating helpers in `pulp::timebase::detail`
(`tick.hpp`): `saturating_add`, `saturating_subtract`, `saturating_multiply`.
They are `constexpr` and total over the full signed 64-bit range.

Two things to know when composing them:

- **Composition saturates per operation.** `saturating_add(a,
  saturating_multiply(b, c))` rails the product first and then the sum, where
  a wide intermediate would clamp the exact value once. These differ only when
  a railed product is pulled back into range by the addend. State the choice in
  a comment where it could matter rather than leaving it implicit.
- **`kMin` is the case that actually bites.** It has no positive counterpart,
  so negating it must saturate, and `kMin / -1` overflows. Any new helper needs
  that case handled explicitly before it can divide to test for overflow.

When adding a helper here, cross-check it against `__int128` as an independent
oracle *in the test* (guarded by `#if defined(__SIZEOF_INT128__)`). The test is
not shipped to MSVC, so it may use the wide type that the header may not — that
verifies the portable path against the maths it replaced rather than against
itself. `test_timebase.cpp` does this over an exhaustive small grid.

## Authored trigger grids

`TriggerGrid` is fixed-capacity authored track×step data, not another clock or
transform chain. Its caller supplies the cycle origin, half-open projection
window, and one stable random word per configured coordinate. Keep projection
allocation-free, step-major then track-major, and block-partition invariant.
Microtiming must remain within the per-step bounds that prevent adjacent steps
from reversing. Groove/swing, coordinate RNG generation, generative pattern
algorithms, transport advancement, and note lifetime remain separate owners.

## The compiled tempo range is public — validate against it, do not re-declare it

`compiled_tempo_map.hpp` exports `kMinimumCompiledTempoBpm` and
`kMaximumCompiledTempoBpm`. They were originally file-private constants in
`compiled_tempo_map.cpp`, and were promoted to the header precisely so a second
consumer could not drift from them.

Any downstream kernel that accepts a BPM (tempo-synced delay, groove, transport
projection) must validate against these symbols rather than writing its own
`1.0` and `1000.0`. Two copies of a range look identical the day they are
written and diverge silently the first time one side is widened, which produces
a value one component accepts and another rejects with no visible error.

Promoting them is behaviour-neutral and must stay that way: the timebase suite
reports 43,136,088 assertions across 27 cases, and that figure should not move
when the constants are relocated. If it does, the promotion changed a
comparison rather than just its spelling.
