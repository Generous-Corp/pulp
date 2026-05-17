# Motion Observability Guide

Pulp Motion is the framework's agent-first motion observability system. It turns
"this animation feels wrong" into measurable, machine-readable evidence: a
time-ordered stream of geometry and value samples with `Start` / `End` burst
framing, monotonic timestamps, deterministic capture, runtime-attachable over
the inspector wire, and a Python visual-analysis pipeline for pixel-truth
fallback.

The system is off by default — there is zero overhead when tracing is disabled.

## When to use it

| Symptom | Reach for |
|---|---|
| "This fade looks slow / late / fast" | `motion::trace(...).value(...)` on the opacity source, then `settling_time_seconds` |
| "These two cards drift apart during a transition" | Two `motion::trace(...).geometry(...)` traces in the same coordinate space |
| "Did anything actually move on screen?" | `GeometrySource::Presentation` + `GeometrySpace::Window` |
| "ScrollView jumps when restored from state" | `motion::trace(...).geometry(...)` on a child of the scroll container — the walker honors the scroll offset |
| "An imported Figma animation looks subtly off" | Record a fixture, assert against the imported intent (timing, monotonicity, settling time) |
| "Frames look fine, the value series is correct, but the rendered effect is wrong" | Capture frames, run `tools/motion/visual/analyze_sequence.py` |

## Core concepts

### Coordinator

`pulp::view::motion::Coordinator` is a process-wide singleton bound to a
`FrameClock`. It owns one tick subscription and dispatches sample events to all
registered sinks.

```cpp
#include <pulp/view/motion.hpp>
using namespace pulp::view::motion;

FrameClock clock;
Coordinator::instance().bind(clock);
Coordinator::instance().set_tracing_enabled(true);
int log_sink = Coordinator::instance().install_default_log_sink();
```

When tracing is disabled (the default) the `FrameClock` callback is a single
branch and returns immediately.

### Trace builder DSL

A trace declares one or more metrics on a logical "view" name. Metrics are
sampled on each tick at the trace's configured FPS.

```cpp
double opacity = 1.0;
auto handle = Coordinator::instance()
    .trace("Card", { /*fps=*/30 })
    .value("opacity", [&] { return opacity; })
    .geometry("frame", card_view,
              { GeometryProperty::MinX, GeometryProperty::MinY,
                GeometryProperty::Width, GeometryProperty::Height },
              GeometrySpace::Window, GeometrySource::Presentation)
    .attach();
```

`TraceHandle` is RAII — when it goes out of scope the trace detaches. The
metric label appears in log output and in inspector events.

### Geometry source: Layout vs Presentation

`GeometrySource::Layout` reports the Yoga-computed frame, ignoring paint-time
transforms. `GeometrySource::Presentation` mirrors `View::paint_all()` exactly
— transform-origin, translate / rotate / scale, the full 2D affine matrix, and
ScrollView ancestor scroll offsets — and returns the axis-aligned bounding box
of where the view actually renders.

| Question | Source | Space |
|---|---|---|
| "Did Yoga move it?" | `Layout` | `ViewGlobal` |
| "Did anything visible move on screen?" | `Presentation` | `Window` |
| "Is it positioned correctly within its container?" | `Layout` | `ViewLocal` |

### Sample event shape

Each tick produces zero or more `SampleEvent`s. The lifecycle for a single
metric:

1. **`TraceStarted`** once per trace registration (first tick after attach).
   Carries the trace's `Provenance` envelope and the stable `trace_id`.
2. **`Baseline`** on the first tick — initial value sample.
3. **`Start`** on the first tick after a baseline where the value changed
   beyond the metric's epsilon. Each burst gets a new `burst_id`.
4. **`Sample`** while the value continues changing.
5. **`End`** on the first stable tick after a change burst, carrying
   per-component deltas from the start of the burst.

Every event carries stable identifiers (`trace_id`, `metric_id`, `burst_id`)
so a fixture comparison can align bursts by identity rather than position —
reordered identical bursts no longer false-fail an `assert_matches`.

Timestamps are monotonic from `FrameClock::time()` and `FrameClock::frame()` —
deterministic on the scripted-capture path, no wall-clock drift in CI.

### Provenance

A `Provenance` envelope describes where a trace came from — the animation
primitive that emits it, the source file and line that registered it, or the
imported-design node that authored it. Attach it on the builder:

```cpp
auto handle = Coordinator::instance()
    .trace("Card", { 60 })
    .with_provenance({ /*source_kind=*/"tween",
                       /*source_id=*/"Card.opacity",
                       /*source_file=*/__FILE__,
                       /*source_line=*/__LINE__ })
    .value("opacity", [&] { return opacity; })
    .attach();
```

The envelope appears on the `TraceStarted` event and round-trips through
fixtures. Agents reading a fixture can resolve "where does this animation
live?" without grepping.

#### Adapters that auto-fill provenance

Phase 9 wires each animation surface so the envelope is populated without
the caller hand-building one. Off by default — pre-Phase-9 callers continue
emitting unstamped events:

| Surface | How to attach | Resulting `source_kind` |
|---|---|---|
| `Tween` | `t.set_motion_provenance("tween", "knob-hover")` then `t.publish(view, metric)` | `"tween"` |
| `Tween` (macro) | `PULP_MOTION_TWEEN("knob-hover", 0.0f, 1.0f, 0.2f)` (auto-fills `source_file`/`source_line` via `std::source_location`) | `"tween"` |
| `AnimatorSetBuilder` | `.name("knob-glow")` then `runner.publish(view, metric, value)` | `"animator-set"` |
| CSS `TransitionSpec` | `parse_transition_shorthand_with_provenance(css, "/styles/card.css", 17)` then `anim.publish(view, metric)` | `"css-transition"` |
| JS rAF | `bridge.load_script(code, "my-script.js")` — `__flushFrames__` stamps each callback as `"<script_id>:<callback_id>"` automatically | `"rAF"` |
| JS user code | `motion.setProvenance("design-import", "figma:Card/Hover")` then `motion.publishValue(view, metric, value)` | whatever the caller passed |
| Design import | `generate_pulp_js` emits a `motion.setProvenance('design-import', '<vendor>:<root-name>')` line at the top of every bundle | `"design-import"` |

The publish channel also has an **ambient provenance** slot for surfaces
that can't easily thread `PublishOptions` through every call site:

```cpp
motion::set_ambient_provenance({ "rAF", "my-script.js:42", "", 0 });
// any publish_value / publish_components calls now inherit the envelope
motion::clear_ambient_provenance();
```

Explicit `PublishOptions::provenance` always wins over the ambient slot.

### Sinks

Sinks consume sample events. Three built-in sinks:

| Sink | Purpose |
|---|---|
| `make_log_sink()` | Writes one `[PulpMotion][view][metric] key=value ...` line per event via `pulp::runtime::log_debug` |
| `make_buffer_sink(buffer*)` | Appends events to a caller-owned `std::vector<SampleEvent>` (tests, in-process consumers) |
| `make_fixture_sink(path)` | Writes a versioned JSONL fixture to disk — the artifact format |

Custom sinks are any `std::function<void(const SampleEvent&)>`:

```cpp
Coordinator::instance().add_sink([](const SampleEvent& e) {
    if (e.kind == SampleEvent::Kind::End && e.metric_name == "opacity") {
        send_alert("fade burst ended: " + format_line(e));
    }
});
```

## Runtime attach via the inspector

The `Motion.*` inspector domain lets agents attach traces over TCP without
editing source. The standalone preview app exposes it behind
`PULP_MOTION_SERVER=1`:

```bash
PULP_MOTION_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview
# "Motion inspector listening on port 9147"
```

Protocol requests:

| Method | Params | Result |
|---|---|---|
| `Motion.startTrace` | `{view_name, fps, metrics:[{kind:"geometry",name,node_id,properties,space,source}]}` | `{trace_id}` |
| `Motion.stopTrace` | `{trace_id}` | `{removed}` |
| `Motion.snapshot` | `{}` | `{tracing_enabled, firehose, active_traces, inspector_traces, emitted_events}` |
| `Motion.listTraces` | `{}` | `{trace_ids:[…]}` |

The server broadcasts `Motion.start`, `Motion.sample`, and `Motion.end` events
to all connected clients as samples are emitted. Subscribing clients receive a
clean stream for the trace they registered — concurrent unrelated animations
do not bleed into the stream unless the firehose is on (see below).

### Env knobs in `pulp-ui-preview`

| Variable | Effect |
|---|---|
| `PULP_MOTION_LOG=1` | Install the default log sink and enable tracing |
| `PULP_MOTION_SERVER=1` | Start the inspector server + Motion domain on port 9147 |
| `PULP_MOTION_FIREHOSE=1` | Enable the publish firehose so every `publish_value`/`publish_components` call broadcasts to all sinks |

## The publish channel

Animation primitives that already advance once per frame can publish their
intermediate values without taking a sampler subscription:

```cpp
#include <pulp/view/motion.hpp>

// Inside an animation's per-frame advance:
pulp::view::motion::publish_value("Card", "opacity", current_opacity);
```

When `tracing_enabled` is false (default) `publish_value` is a single branch and
returns. When `tracing_enabled` is true AND `firehose` is on, every publish
fans out as a `SampleEvent` to all sinks. Filter-scoped subscriptions (publishes
that route only to a single subscriber) are a future addition.

Per-(view, metric) `precision` and `epsilon` are sticky — set on the first
publish and inherited on subsequent calls so hot-path callers can omit
`PublishOptions` every tick.

## Fixtures (record / replay / assert)

A fixture is the on-disk form of a motion stream — a versioned JSONL file that
captures a complete run for replay, comparison, or CI gating.

### Recording

```cpp
auto fixture_sink = pulp::view::motion::make_fixture_sink("/tmp/card-open.motion.jsonl");
int sink_id = Coordinator::instance().add_sink(std::move(fixture_sink));
// ... drive the animation ...
Coordinator::instance().remove_sink(sink_id);  // closes the file
```

### Replay

```cpp
std::vector<SampleEvent> replayed;
int n = pulp::view::motion::replay_fixture(
    "/tmp/card-open.motion.jsonl",
    pulp::view::motion::make_buffer_sink(&replayed));
```

### Compare

```cpp
auto golden   = pulp::view::motion::load_fixture("test/motion/goldens/card-open.motion.jsonl");
auto captured = pulp::view::motion::load_fixture("/tmp/card-open.motion.jsonl");

auto diff = pulp::view::motion::assert_matches(golden, captured);
if (!diff.matches()) {
    for (const auto& it : diff.differences) {
        std::cerr << it.kind << " " << it.view_name << "." << it.metric_name
                  << "." << it.component_name
                  << " expected=" << it.expected << " observed=" << it.observed
                  << " (" << it.detail << ")\n";
    }
}
```

`FixtureMatchOptions` controls per-component epsilon, timing epsilon, and
whether event counts must match exactly.

## Input recording and replay

`View::simulate_click`, `simulate_drag`, and `simulate_hover` can be recorded
into the same fixture format that carries motion samples. The recorded
interactions can later be replayed against a fresh view tree on a scripted
`FrameClock` to reproduce the original motion stream deterministically. Off by
default everywhere — recording is opt-in.

```cpp
#include <pulp/view/motion.hpp>

// Recording — paired with whatever motion sinks you already have.
{
    auto recorder = pulp::view::motion::make_input_recorder(
        "/tmp/card-open.motion.jsonl");

    root_view.simulate_hover({150, 150});
    clock.tick(1.0f / 60.0f);
    root_view.simulate_click({150, 150});
    // ... drive your animation ...
}   // recorder destructor closes the fixture sink + flips recording off.

// Replay — into a brand-new view tree and clock.
pulp::view::motion::replay_inputs(
    "/tmp/card-open.motion.jsonl", fresh_root_view, fresh_clock);
```

A recorded `Input` event rides in the same JSONL alongside `baseline` /
`sample` / `start` / `end`, with two extra fields:

```jsonc
{"kind":"input","view":"input","metric":"click",
 "t":0.05,"frame":3,"precision":3,"trace_id":0,"metric_id":0,"burst_id":0,
 "components":{"x":150,"y":150},"deltas":{},
 "input_kind":"click","view_id":"target"}
```

`view_id` is the recorded target's `View::id()` (empty when the event didn't
land on an id-bearing view). `components` carries root-space coordinates: `x`
/ `y` for click and hover; `start_x` / `start_y` / `end_x` / `end_y` / `steps`
for drag. Existing motion lines are untouched — input fields are only
serialized when `kind == Input` so pre-Phase-10 fixtures round-trip
byte-for-byte.

`replay_inputs` walks the fixture, advances the supplied `FrameClock` to each
input's recorded timestamp (delta-based: the first input anchors on its
recorded `t`, subsequent inputs tick by the delta), resolves the target by
`view_id` for diagnostic continuity, and dispatches through `root_view` so
its `hit_test()` lands on the same descendant. Sinks installed on the
Coordinator (typically the same `make_fixture_sink` paired with the
recorder) re-capture the motion stream the replayed inputs produce, so a
recorded fixture replayed against a fresh tree yields a byte-equivalent
motion fixture (modulo timing tolerance from `FixtureMatchOptions`).

The non-recording cost is a single relaxed atomic load
(`input_recording_enabled()`) on each `simulate_*` call, so leaving the
hooks in production code is free.

## Assertion helpers

Use these for unit tests and for the assertion CLI. Each takes a `ScalarSample`
series extracted with `extract_scalar(events, view, metric, component)`.

| Helper | What it measures |
|---|---|
| `is_monotonic(samples, epsilon)` | Strict monotonicity — direction inferred from first non-zero change |
| `settling_time_seconds(samples, epsilon)` | Time from first change to last change |
| `start_delay_seconds(samples, epsilon)` | Time from t0 to first change above epsilon |
| `overshoot(samples, epsilon)` | Peak excursion beyond final value, as a fraction of total displacement |
| `frame_jitter_seconds(samples)` | Stddev of inter-sample intervals — frame-pacing jitter at fixed FPS |
| `final_value(samples)` | Last sample value, NaN when empty |

Helpers are split deliberately — combining continuity, monotonicity, and timing
into a single heuristic obscures which property failed. Pin each concern in its
own assertion.

```cpp
auto samples = extract_scalar(events, "Card", "opacity", "value");
REQUIRE(is_monotonic(samples));
REQUIRE(settling_time_seconds(samples) == Catch::Approx(0.6).margin(0.05));
REQUIRE(overshoot(samples) < 0.05);
```

## Visual-analysis pipeline

When a behavior is only observable in pixels (transitions, GPU filters, mask
compositing), capture a frame sequence and run the Python pipeline:

```bash
pip install -r tools/motion/visual/requirements.txt
python3 -m tools.motion.visual.analyze_sequence \
    --frames-dir ./captures/card-open/ \
    --output     ./reports/card-open/
```

Outputs:

- `analysis.json` — per-frame metrics, pairwise SSIM + pixel diff, keyframes,
  schema version
- `summary.md` — agent-readable summary
- `diff/diff_NNNN_NNNN.png` — pairwise pixel-diff heatmaps (capped at
  `--max-diff-frames`)
- `keyframes.png` — keyframe sprite (first, mid, last, plus top-delta pairs)

The schema is versioned (`REPORT_SCHEMA_VERSION`) so downstream consumers can
reject unknown formats. Dependencies (numpy, Pillow, scikit-image) are MIT /
BSD / Apache 2.0 only and are not redistributed in plugin or app artifacts.

## Reduced-motion policy

Pulp animation primitives honor the system reduced-motion preference via
`pulp::view::MotionPreferences`, a sibling of `AppearanceTracker`:

```cpp
#include <pulp/view/motion_preferences.hpp>

auto& prefs = pulp::view::MotionPreferences::instance();
// OS-driven by default. Test / JS override that wins over the OS:
prefs.set_override(pulp::view::MotionPolicy::Reduced);
prefs.set_duration_scale(0.5);   // halves the configured duration
// Revert to OS:
prefs.set_override(std::nullopt);
```

`MotionPolicy` has three values:

| Policy | Effect on animation primitives |
|---|---|
| `Full` (default) | Animate over the configured duration. |
| `Reduced` | Scale the configured duration by `duration_scale` (0.0–2.0). |
| `Off` | Jump straight to the target value; no intermediate samples. |

The policy is read once at animation start (`Tween` / `ValueAnimation`
constructor, `Tween::reset()`, `ValueAnimation::animate_to()`, and
`CssAnimation`'s first `tick()`). Changes to `MotionPreferences` between
two animations affect only the next animation that starts.

Platform readers:

- **macOS** — `[NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion`
- **Windows** — `SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, ...)`
- **Other** — defaults to `Full`.

Fixtures recorded under a non-`Full` policy capture both `policy` and
`duration_scale` on the v2 header so goldens recorded with reduced motion
cannot silently compare against `Full` captures:

```jsonc
{"motion_fixture_version":2,"policy":"reduced","duration_scale":0.5}
```

The fields are additive — pre-Phase-8 v2 fixtures without them still load
(defaulting to `"full"` / `1.0`). To compare two fixtures' headers, use the
header-aware `assert_matches` overload:

```cpp
auto g_hdr = motion::load_fixture_header("golden.jsonl");
auto c_hdr = motion::load_fixture_header("captured.jsonl");
auto g = motion::load_fixture("golden.jsonl");
auto c = motion::load_fixture("captured.jsonl");
auto diff = motion::assert_matches(g_hdr, g, c_hdr, c);
// Adds a `"policy-mismatch"` Item to diff.differences when policy or
// duration_scale differ.
```

## Architectural guarantees

- **Off by default** — every entry point gates on `tracing_enabled()`. No cost
  in shipping builds unless explicitly enabled.
- **One sampler subscription** — the Coordinator holds a single `FrameClock`
  subscription; per-trace accumulators decide when to sample.
- **Monotonic timestamps** — events stamp `FrameClock::time()` and `frame()`,
  not wall-clock, so CI runs on the scripted-capture path are deterministic.
- **No silent data loss** — fixture loaders reject unknown schema versions
  instead of accepting them.
- **No external dependencies in the C++ layer** — the fixture parser is
  hand-rolled to keep `core/view/motion` free of new JSON deps.

## See also

- [Animation Guide](animation.md) — `Tween`, `ValueAnimation`, CSS transitions
- [Design Debugging](design-debugging.md) — visual regression workflows
- [Custom Rendering](custom-rendering.md) — Skia / Dawn pipeline that produces
  the frames the visual analyzer consumes
