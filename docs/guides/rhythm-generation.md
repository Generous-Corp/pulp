# Rhythm generation and timing feel

Pulp ships a set of small, allocation-free primitives for building rhythms and
for moving events off a grid on purpose. They live in two modules:

| Header | What it gives you |
|---|---|
| `core/music/include/pulp/music/pattern.hpp` | `euclidean_pattern()`, `BinaryPattern<N>`, `EuclideanPatternRecipe`, walkers, cellular evolution, a looping shift register |
| `core/music/include/pulp/music/rhythm_relationship.hpp` | `derive_rhythm_relationship()` — one lane derived from another |
| `core/music/include/pulp/music/markov.hpp` | `PreparedMarkovModel<N>` — weighted state transitions |
| `core/timebase/include/pulp/timebase/trigger_grid.hpp` | `TriggerGrid<>` — authored track×step cells with per-cell microtiming and probability |
| `core/timebase/include/pulp/timebase/quantize.hpp` | `swing_position()`, `unswing_position()`, `swing_displacement()`, `SwingRatio` |
| `core/timebase/include/pulp/timebase/groove_kernel.hpp` | `OrderPreservingGrooveKernel` — swing plus table displacement that refuses to reorder |
| `core/timebase/include/pulp/timebase/coordinate_random.hpp` | `coordinate_random()`, `coordinate_chance()` — determinism keyed to musical coordinates |

**These are general rhythmic primitives, not a style preset.** Nothing here
encodes a genre. A Euclidean distribution is an even spread of onsets over
steps; a relationship is a rule for deriving one lane from another; a swing
ratio is a warp of the grid. What idiom comes out depends entirely on what you
ask for. The worked example near the end builds one specific feel, but it is an
illustration of the tools, not the point of them.

Everything on this page is integer arithmetic on the shared tick domain
(`kTicksPerQuarter = 705600`). No floating point is involved in any position,
so two machines agree on a tick exactly.

## Three words that are not synonyms

Conflating these is the mistake this page exists to prevent, because the
symptoms look identical and the fixes are opposite.

- **Swing** is a ratio-based warp of the grid. It moves the interior boundary of
  every pair of grid cells and stretches the material on either side onto the
  new halves. It is expressed as an exact rational — `SwingRatio`, with
  `kStraightSwing{1,2}` and `kTripletSwing{2,3}` — never as a float.
- **Groove** is authored per-step displacement plus per-step velocity scaling,
  read from a table. It is a fixed, deliberate relationship: step 3 is always
  this far behind the grid.
- **Humanisation** is bounded *random* variation. It is a different stage, in a
  different coordinate (samples, not ticks), owned by the note-humaniser device
  and documented in
  [event-stream-timing-transform](../policies/event-stream-timing-transform.md).

A deliberate feel is **groove plus swing**. It is exact and repeatable. It is
not humanisation, and reaching for a humaniser's depth control to get it will
give you jitter that merely resembles it — a displacement that changes every
pass instead of a musical relationship that holds.

## Generating a pattern

`euclidean_pattern<MaxSteps>(steps, pulses, rotation)` distributes `pulses` as
evenly as possible across `steps`. The canonical rotation starts on an onset;
positive rotation delays every onset, negative advances it.

```cpp
#include <pulp/music/pattern.hpp>
using namespace pulp::music;

const auto tresillo = euclidean_pattern<16>(8, 3);   // x..x..x.
if (tresillo)
    const bool first_step = *tresillo.pattern.at(0);
```

Every operation returns a result value carrying a `PatternError` rather than
throwing, and `BinaryPattern<N>` is fixed-capacity — asking for more steps than
`N` returns `capacity_exceeded` instead of allocating.

**What an even distribution can and cannot give you.** Many named bell patterns
*are* even distributions and fall straight out, given the right rotation:
`euclidean_pattern<16>(12, 7, 5)` is the seven-onset bell over twelve steps
(`{0, 2, 4, 5, 7, 9, 11}`). Others are *not* even, and no rotation will produce
them. The 3-2 son clave (`{0, 3, 6, 10, 12}`) is asymmetric by construction, and
no rotation of the even five-in-sixteen pattern equals it — a fact the test
suite asserts over all sixteen rotations. Author an asymmetric figure directly:

```cpp
BinaryPattern<16> clave;
constexpr std::array<std::uint8_t, 16> bits{1,0,0,1,0,0,1,0, 0,0,1,0,1,0,0,0};
(void)clave.assign(bits);
```

`EuclideanPatternRecipe` is the persistence contract for a generated pattern —
`version`, `steps`, `pulses`, `rotation` as named scalar fields. Object bytes
are not a wire format: serialize the fields individually, and
`materialize_pattern()` rejects a version it does not understand with
`unsupported_recipe_version`.

## Relating one lane to another

`derive_rhythm_relationship(source, config)` builds a target lane from a source
lane. This is where interlocking and polymeter come from.

```cpp
#include <pulp/music/rhythm_relationship.hpp>

RhythmRelationshipConfig config;
config.target_steps = 16;
config.relationship = RhythmRelationship::complementary;
config.length_mapping = RhythmLengthMapping::wrap;
const auto answer = derive_rhythm_relationship(source.pattern, config);
```

The `relationship` decides which target steps are candidates:

- `coincident` — candidates where the source has an onset. Doubling, reinforcing.
- `complementary` — candidates where the source does *not*. This is the
  interlocking case: two parts that together fill the cycle and individually
  never collide, the shape gamelan-style paired parts and hocketed bell lines
  take.
- `independent` — every step is a candidate; the source only supplies the grid.

`RhythmLengthMapping` decides how a source of one length reads onto a target of
another, and it is the polymeter control:

- `wrap` repeats the source cycle across the target, so a 5-step source against
  a 16-step bar reads source step `target_step % 5`. The two only realign after
  five bars. That is a true 5-against-4.
- `proportional` scales the source cycle exactly once across the target length,
  preserving its shape at a different resolution rather than creating a
  polymeter.

`phase_steps` delays (positive) or advances (negative) the relationship on the
target grid. `RhythmCollisionPolicy::avoid_source_overlap` drops any candidate
that lands on a source onset — useful for guaranteeing two parts never sound
together. `RhythmDensityPolicy::exact_onsets` thins the candidate set down to
exactly `target_onsets`, and it needs a `RhythmDrawCoordinate` (`seed`, `cycle`,
`lane`) to decide which survive; that choice is a pure function of the
coordinate, so lane evaluation order cannot change the result, and it fails with
`insufficient_candidates` rather than silently under-filling.

## Placing the events

`TriggerGrid<MaxTracks, MaxSteps>` holds authored track×step cells and projects
them into a half-open tick window.

```cpp
#include <pulp/timebase/trigger_grid.hpp>
namespace tb = pulp::timebase;

constexpr std::int64_t sixteenth = tb::kTicksPerQuarter / 4;   // 176400
tb::TriggerGrid<4, 16> grid;
grid.configure(3, 16, tb::TickDuration{sixteenth});
grid.set_cell(0, 4, tb::TriggerCell{true, 110, {1, 1}, tb::TickDuration{-8820}});
```

`TriggerCell::microtiming` **is the per-lane displacement mechanism** — it is
authored per (track, step), so different lanes can sit at different distances
from the same grid line. It is bounded to `minimum_microtiming()` ..
`maximum_microtiming()` — `-step/2` and `(step - 1)/2`, so a sixteenth step of
176400 ticks allows -88200 .. +88199 — and that bound is what keeps adjacent
steps from swapping. Ask for the bound rather than assuming it: the two ends are
not symmetric. A cell outside it is rejected with `InvalidMicrotiming` and the
grid is left unchanged.

Two properties worth knowing before you consume the output:

- **Emission order is step-major then track-major, which is not position
  order.** A lane sitting early inside step 4 is still emitted after an on-grid
  lane at step 4. If you need chronological order, sort the window yourself.
- **Validation and counting finish before anything is written.** Every error
  leaves your output span untouched, and `OutputTooSmall` tells you the span was
  too small rather than truncating.

`project_window()` needs exactly one `std::uint64_t` per configured coordinate.
A cell authored at probability `{1, 1}` always fires and ignores its word; a
`TriggerProbability` of `{1, 2}` consults it.

## Determinism without a random number generator

`coordinate_random(seed, coordinate)` is a **pure hash** of a seed plus stable
musical coordinates — tick, lane, cycle, stream. It holds no state and is not a
generator: the same coordinates produce the same value forever, whatever order
lanes are evaluated in and however the host partitions its blocks.

```cpp
#include <pulp/timebase/coordinate_random.hpp>

const auto word = tb::coordinate_random(
    seed, tb::RandomCoordinate{tb::TickPosition{tick}, lane, cycle, stream});
```

That is the product feature, not an implementation detail. Feeding a grid's
probability draws from this means a projection over one window and the same
projection split into two windows produce the identical event stream — which the
test suite asserts, with a control proving a different seed selects differently.
Never replace it with a callback-local generator; doing so makes a ghost note
appear or vanish depending on the host's buffer size.

`coordinate_chance(seed, coordinate, numerator, denominator)` is the predicate
form, with no floating-point conversion anywhere. Zero means never, equal
numerator and denominator means always, and an invalid ratio returns
`ProbabilityError::InvalidRatio` rather than guessing.

## Quantizing existing material into a feel

The functions in `quantize.hpp` take material that is already straight and move
it. This is the path to use when you have authored or recorded positions and
want to impose a feel on them, rather than generating a new pattern.

```cpp
#include <pulp/timebase/quantize.hpp>

constexpr tb::TickDuration eighth{tb::kTicksPerQuarter / 2};
if (tb::valid_swing_grid(eighth) && tb::valid_swing_ratio(tb::kTripletSwing))
    position = tb::swing_position(position, eighth, tb::kTripletSwing);
```

**Validate the grid and the ratio.** An invalid grid or ratio makes
`swing_position()` and `unswing_position()` the identity — the caller validates,
and a bad setting must not silently move music. Guarding with
`valid_swing_grid()` / `valid_swing_ratio()` is how you tell "this setting is
straight" apart from "this setting was rejected".

Swing warps the whole passage, not only the notes that land exactly on an
off-beat. With a triplet ratio on an eighth grid, the four straight sixteenths
of a beat land on 0, 1/3, 2/3 and 5/6 of the quarter:

| authored tick | swung tick | where in the beat |
|---|---|---|
| 0 | 0 | on the beat |
| 176400 | 235200 | 1/3 |
| 352800 | 470400 | 2/3 |
| 529200 | 588000 | 5/6 |
| 705600 | 705600 | next beat |

`kStraightSwing` is the identity on every tick, bit for bit — not merely close
to it. `swing_displacement()` returns the delta on its own, so a higher-level
transform can combine it with other authored displacements before saturating the
final position.

### How exactly the round trip inverts

`unswing_position()` is the left inverse of `swing_position()` under the same
grid and ratio, but the two halves of a pair behave differently and it is worth
being precise about which.

Swing expands one half of each pair and compresses the other. The expanded half
gains ticks, so every position in it round-trips **exactly**. The compressed
half has fewer ticks to land on than it came from, so it is not injective and
the inverse can only recover a position to within the rounding that forces.

Measured over a full pair on an eighth grid at a triplet ratio, with every one
of the 705600 ticks round-tripped:

- expanded half — **0 mismatches**, exact for all 352800 ticks;
- compressed half — recovery is exact or **one tick late, never early**, with
  exactly 117600 of the 352800 ticks landing one tick late.

So the round trip is lossless on the expanded half and bounded by a single tick
on the compressed half, in a known direction. At `kTicksPerQuarter = 705600` a
tick is well under a microsecond at any musical tempo, but the bound is stated
because "approximately invertible" is not a contract and this is.

## Projecting a groove

The **canonical authored groove is `timeline::GrooveTemplate`**. It owns the
name, the persistence, the independent swing and table grids, the strengths, and
the 0..4x accent domain. That is the model a document saves.

`timebase::OrderPreservingGrooveKernel` is deliberately narrower: a
fixed-capacity, allocation-free **projection kernel** for the non-reordering
subset. It is not a template and has no persistence model of its own — it
projects values whose numeric domains match timeline's so authored settings can
feed it directly: **timing strength 0..1000, velocity scale 0..4000, at most
1024 steps**.

```cpp
#include <pulp/timebase/groove_kernel.hpp>

const std::array<tb::GrooveKernelStep, 1> table{
    {{tb::TickDuration{17640}, tb::kGrooveKernelUnitScale}}};
tb::GrooveKernelInput input;
input.table_grid = tb::TickDuration{sixteenth};
input.steps = table;
input.timing_strength = tb::kGrooveKernelUnitScale;   // 1000

const auto kernel = tb::OrderPreservingGrooveKernel::create(input);
if (kernel)
    const auto moved = kernel.value().apply_timing(tb::TickPosition{0});
```

**Timing strength scales both swing and table displacement, and zero is exact
identity — including swing.** That is a guarantee, not an approximation: at
strength 0 no position moves by a single tick, which is precisely what lets a
bypass setting be defined as the zero-strength result. Strength is exact in
between too: half strength on a 17640-tick offset is 8820 ticks, not 8820-ish.
Swing and table displacement add, so an off-beat eighth carried 117600 ticks by
a triplet ratio and a further 17640 by the table lands 135240 ticks late.

Where the kernel earns its name is `create()`. It walks every phase boundary of
the joint swing/table period and **rejects a table that would reorder events**
with `GrooveKernelError::ReordersEvents`. A table whose neighbouring steps carry
offsets near `+step` and `-step` is refused rather than quietly emitting two
events in the opposite order to the order they were authored in.

> **This is a real difference in what you get.** `GrooveTemplate` bounds each
> step offset independently and performs no joint validation across adjacent
> steps, so a reordering table is accepted by the authored model and plays. A
> displacement projected through the kernel therefore carries an ordering
> guarantee that the same displacement authored as a document groove does not.

**The kernel's table is indexed by the position you hand it, and there is one
table per kernel.** It is not per-lane. To give three lanes three different
feels, construct three kernels. And if you have already displaced a position —
with `TriggerCell::microtiming`, say — handing that displaced position to a
kernel can select a different table slot than the authored position would have.
Project from authored positions, or accept that the table indexes what you gave
it.

## Weighted transitions

`PreparedMarkovModel<MaxStates>` is a fixed-capacity transition table with the
expensive step separated from the cheap one: `prepare()` is the control-thread
operation that validates and builds cumulative weights, and `next()` is `const`,
bounded, allocation-free evaluation.

```cpp
#include <pulp/music/markov.hpp>

PreparedMarkovModel<4> model;
constexpr std::array<std::uint32_t, 16> weights{
    0, 3, 1, 0,  1, 0, 3, 1,  1, 1, 0, 3,  3, 1, 1, 0};
if (model.prepare(4, weights) == MarkovError::none)
    const auto next = model.next(current, word);
```

`prepare()` rejects a row whose weights sum to zero with
`empty_transition_row` rather than leaving a state that cannot transition, and a
weight count that does not match `state_count * state_count` with
`weight_count_mismatch`. `next()` takes the random word from the caller — pair
it with `coordinate_random()` and the sequence is reproducible.

## Worked example: composing one specific feel

Here is how you would put these together to get a particular timing feel — the
loose, deliberately uneven placement associated with J Dilla (James Dewitt
Yancey). The primitives are general; this is one arrangement of them.

The feel decomposes into four independent decisions:

1. hats stay exactly on the grid, so there is a straight reference to hear the
   other lanes against;
2. the snare lands slightly **early**;
3. the bass lands slightly **late**, so snare and bass pull apart rather than
   sharing a displacement;
4. a five-step cycle runs against the sixteen-step bar, so the pattern does not
   repeat where the bar does.

Ticks, at `kTicksPerQuarter = 705600`: a sixteenth is 176400. The snare sits
8820 ticks early and the bass 17640 late — a twentieth and a tenth of a
sixteenth, which at 120 BPM is 6.25 ms early and 12.5 ms late.

```cpp
// A five-step source across a sixteen-step bar: a 5-over-4 polymeter that
// only realigns with the bar after five bars.
const auto source = euclidean_pattern<16>(5, 2);          // x..x.

RhythmRelationshipConfig config;
config.target_steps = 16;
config.relationship = RhythmRelationship::coincident;
config.length_mapping = RhythmLengthMapping::wrap;
const auto bass_lane = derive_rhythm_relationship(source.pattern, config);
// onsets at steps 0, 3, 5, 8, 10, 13, 15

tb::TriggerGrid<4, 16> grid;
grid.configure(3, 16, tb::TickDuration{sixteenth});

for (std::size_t step = 0; step < 16; ++step)          // hats: straight
    grid.set_cell(0, step, tb::TriggerCell{true, 90, {1, 1}, tb::TickDuration{0}});
for (const std::size_t step : {std::size_t{4}, std::size_t{12}})   // snare: early
    grid.set_cell(1, step, tb::TriggerCell{true, 110, {1, 1}, tb::TickDuration{-8820}});
for (const std::size_t step : bass_steps)              // bass: late, 5-over-4
    grid.set_cell(2, step, tb::TriggerCell{true, 100, {1, 1}, tb::TickDuration{17640}});
```

Projecting one bar gives 25 events at exactly these ticks — nothing rounds,
because an integer displacement is added to an integer grid:

| lane | landings (ticks) |
|---|---|
| hats | 0, 176400, 352800, … 2646000 (every `step × 176400`) |
| snare | 696780, 2107980 (`step × 176400 − 8820`) |
| bass | 17640, 546840, 899640, 1428840, 1781640, 2310840, 2663640 |

The whole example is a real test case — *"sixteenth lanes take an exact per-lane
feel from authored microtiming"* in `test/test_music_generative.cpp` — which
asserts every one of those positions. The swing, round-trip, groove-projection
and partition-invariance claims on this page are asserted by the four cases
beside it.

Note what is *not* here: no humaniser, no random depth. Every displacement above
is an exact authored relationship that reproduces bit for bit on every pass.

## Reachability — what you can drive these from today

These are **installed-SDK C++ capabilities**. They are reachable from
`Processor` code and through the capability-binding path, where they are
advertised in `docs/status/agent-capabilities.json` with `status: usable`,
`contract_version 1.0` and a `sha256:` contract digest.

They are **not** exposed as timeline devices, CLI verbs, or MCP tools. The
built-in timeline device table has exactly two entries — `Pulp Note Humaniser`
and `Pulp Basic Instrument` (`core/host/src/timeline_device_resolver.cpp`).
There is no `pulp seq` verb that builds a Euclidean pattern, applies a swing
ratio, or projects a groove kernel, and no MCP tool that does. If you want this
behaviour in a project today, you write it in C++ against these headers.

Two further limits worth stating plainly:

- **`OrderPreservingGrooveKernel` has no non-test consumer under `core/`.**
  Nothing in the shipping compiler runs it. Its ordering guarantee is available
  to code that calls it, and is not something the timeline currently applies on
  your behalf — the stage that runs in the compiler is `GrooveTemplate`, which
  does not validate order.
- **There is no per-lane groove object.** Per-lane feel comes from
  `TriggerCell::microtiming`, or from one kernel per lane. A single kernel
  applied to several lanes gives them all the same displacement schedule.

The pipeline these sit in — which stage owns which coordinate, what is baked
into a compiled program and what is not — is specified in
[event-stream-timing-transform](../policies/event-stream-timing-transform.md).
That document also records the correspondence worth knowing if you are binding
these from outside: an external transform's `amount` is the groove kernel's
timing **strength**, which is why its range is 0..1000 and why bypass is exactly
the zero-strength result.

## See also

- [Timeline cookbook](timeline-cookbook.md) — recipes for the timeline itself
- [Event-stream timing transform](../policies/event-stream-timing-transform.md) — the full stage-by-stage timing contract
- [Modules reference](../reference/modules.md) — the `music` and `timebase` module surfaces
