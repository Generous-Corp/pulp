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
live?" without grepping. The richer adapters that auto-populate provenance
from `Tween` / CSS `TransitionSpec` / `AnimatorSet` / JS rAF / design-import
land in a follow-up phase; today the envelope is opt-in via
`with_provenance(...)`.

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
