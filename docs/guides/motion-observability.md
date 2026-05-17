# Motion Observability Guide

Pulp Motion is the framework's agent-first motion observability system. It
turns "this animation feels wrong" into measurable, machine-readable evidence:
a time-ordered stream of geometry and value samples that an agent can read
from a fixture or stream over the inspector wire — instead of guessing from
source.

Trigger phrases an agent should reach for:

- "this animation feels slow / wrong / janky / late"
- "did this transition actually animate?"
- "scroll restore lands at the wrong place"
- "verify the imported Figma motion matches intent"
- "frames look fine but the rendered effect is off"

Mechanics: `Start` / `End` burst framing, monotonic timestamps, deterministic
capture on a scripted `FrameClock`, runtime-attachable over the inspector
wire, and a Python visual-analysis pipeline for pixel-truth fallback when no
scalar is observable.

The system is off by default — there is zero overhead when tracing is disabled.

## End-to-end flow

From "I have a bug" to "I have evidence":

1. **Define expected behavior in measurable terms** — settling time, monotonicity, final value, overshoot bound, scroll content offset.
2. **Locate the views or state values that drive the behavior.**
3. **Attach a focused trace** — `motion::trace(...).value(...)` for scalars, `.geometry(...)` for layout/presentation rects, `.scrollGeometry(...)` for `ScrollView` offset/visible-rect/content-size.
4. **Reproduce the issue and capture evidence**: (a) a live event stream over the inspector wire (port 9147), (b) a `.motion.jsonl` fixture for offline replay, or (c) frames + the visual analyzer.
5. **Read the numbers** — assert with `settling_time_seconds`, `overshoot`, `final_value`, `is_monotonic`, `local_step_outlier_ratio`. Cite frame indices and metric values in the report.
6. **Apply the fix, re-run validation, remove the temporary instrumentation.** Land a golden fixture so the next regression fails in CI, not after a user reports it.

## Tooling

Three discovery surfaces, pick by what's at hand. All three terminate at the
same `pulp::view::motion::Coordinator` so fixtures, the scrubber, cost
attribution, reduced-motion gating, and provenance envelopes work identically
regardless of which surface launched the trace.

- **`pulp` CLI** — the `/motion` Claude Code slash command surfaces the
  inspector-wire quick start. From any Pulp source tree:
  `pulp build && PULP_MOTION_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview`.
  Dedicated `pulp motion *` subcommands are planned (tracked by [#2153](https://github.com/danielraffel/pulp/issues/2153));
  today, the CLI's role is launching the host with the right env knobs.
- **`pulp-mcp` server** — the MCP server exposes `pulp_inspect_*` (live trees,
  screenshots, evaluate). A dedicated `pulp_motion_*` MCP wrapper is planned
  ([#2153](https://github.com/danielraffel/pulp/issues/2153)); today, agents
  that need motion traces from MCP can drive the inspector wire via
  `pulp_inspect_evaluate` or shell out to `nc localhost 9147`.
- **XcodeBuildMCP** — recommended companion on macOS and the iOS Simulator. Use
  its log-stream + screenshot tools to drive a simulator while the motion
  inspector records, and feed the captured frames into
  `tools/motion/visual/capture_sim_frames.py --source simulator`. XcodeBuildMCP
  is not required — agents can capture and read simulator logs directly with
  `xcrun simctl spawn booted log stream --predicate 'subsystem == "PulpMotion"'`
  — but it is the path of least friction for Apple workflows.

## Quick start

Three ways to land your first trace.

```bash
# 1. CLI + standalone host (fastest path).
PULP_MOTION_SERVER=1 PULP_MOTION_LOG=1 ./build/examples/ui-preview/pulp-ui-preview &

# 2. Inspector wire (TCP, line-delimited JSON, port 9147).
echo '{"id":1,"method":"Motion.startTrace","params":{
  "view_name":"Card","fps":30,
  "metrics":[{"kind":"geometry","name":"frame","node_id":"card",
    "properties":["minX","minY","width","height"],
    "space":"window","source":"presentation"}]}}' \
  | nc -w 30 localhost 9147

# 3. Stop the trace + read events as JSONL on the same socket.
echo '{"id":2,"method":"Motion.stopTrace","params":{"trace_id":1}}' \
  | nc -w 5 localhost 9147

# 4. /motion slash command in Claude Code — same workflow, links to this guide.
/motion
```

For the Swift, Kotlin / Compose, fixture-based, scrubber, and cost-attribution
paths see the dedicated sections below.

## When to use it

| Symptom | Reach for |
|---|---|
| "This fade looks slow / late / fast" | `motion::trace(...).value(...)` on the opacity source, then `settling_time_seconds` |
| "These two cards drift apart during a transition" | Two `motion::trace(...).geometry(...)` traces in the same coordinate space |
| "Did anything actually move on screen?" | `GeometrySource::Presentation` + `GeometrySpace::Window` |
| "ScrollView jumps when restored from state" | `motion::trace(...).geometry(...)` on a child of the scroll container — the walker honors the scroll offset |
| "Scroll restore lands at the wrong content offset" | `motion::trace(...).scroll_geometry(name, scroll_view, props)` — reads `ScrollView`'s offset, visible rect, content size, and scrollable max directly |
| "An imported Figma animation looks subtly off" | Record a fixture, assert against the imported intent (timing, monotonicity, settling time) |
| "Frames look fine, the value series is correct, but the rendered effect is wrong" | Capture frames, run `tools/motion/visual/analyze_sequence.py` |

## Visual analysis outputs

The visual-analysis pipeline produces a small fixed set of artifacts. Pick by
the question you need to answer:

| Artifact | What it shows | When to use it |
|---|---|---|
| `keyframes.png` | 5 strategically-picked frames laid out left-to-right, no overlays | Eyeball the overall motion progression in one image |
| `grid/frame_NNNN.png` (and `keyframes_grid.png`) | Same frames with alphanumeric cell labels (A1..H12) overlaid | Coordinate-aware position checks ("the card lands in F4 by frame 30") |
| `diff/diff_NNNN_NNNN.png` | Per-frame-pair pixel deltas | Confirm exactly which frames moved and which stayed still |
| `summary.md` (claim-evidence preamble) | Confidence score + supporting / contradicting evidence | Read first — the preamble names the contract every claim must satisfy and flags low-confidence runs that need a human |
| `analysis.json` | Per-frame SSIM, dominant motion vectors, keyframe indices, `mean_confidence`, optional `affine_first_to_last` | Machine-readable for downstream automation |

Recommended interpretation flow: keyframe sprite first, grid sequence for
coordinate checks, adjacent-pair diff for pixel-truth confirmation. The
`summary.md` preamble is the canonical claim-evidence contract — see Path B in
`.agents/skills/motion/SKILL.md` for the agent-side wording.

## Example scenarios

Concrete bug shapes Pulp Motion catches in real Pulp UIs:

- Detect unintended movement in UI that should remain still.
- Confirm expected motion actually happens when a state change should animate.
- Verify movement direction on each axis (up vs down, left vs right).
- Confirm two animations are truly staggered instead of firing together.
- Check relative positioning between two moving elements stays correct during a transition.
- Detect timing drift — late starts, early stops, incorrect duration.
- Catch easing or path issues — overshoot or reversal when smooth one-way motion is expected.
- Diagnose scroll jumps, failed restoration, content-offset desynchronization.

## Limitations

Honest list — what Pulp Motion does not see today:

- Animations driven entirely outside Pulp's view tree (e.g., host DAW chrome) are not observable — motion only sees what the Pulp Coordinator sees.
- `GeometrySpace::Window` and `GeometrySpace::Screen` currently collapse onto `ViewGlobal` until the host surfaces its window/screen offset to the view system. Planned: host adapter hook in 0.110.
- `ScrollView`'s base transform is applied before child layout; geometry traces on direct ScrollView children report post-transform coordinates. Planned: explicit `space: scroll-local` in 0.110.
- `MotionPolicy::Off` short-circuits `Tween` / `ValueAnimation` / `CssAnimation` / `AnimatorSet` at construction — they finish immediately. Off mode is binary, not per-animation.
- Cost attribution requires a render-side probe; if no `make_render_cost_probe(...)` is installed, the cost fields stay at 0.
- `.transition`-style visual-only behavior with no exposed scalar is observable only via the visual-analysis pipeline (Path B in the motion skill) — there is no value series to trace.

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

**Worked example.** A knob rotates via `transform: rotate(45deg)`. Layout
reports the static frame (`x=10, y=20, w=64, h=64`). Presentation reports
the rotated bounding box (`x≈-7, y≈3, w=90.5, h=90.5`). Layout sees what
the layout engine computed; Presentation sees what the user sees. Choose
by question: "did anything actually move on screen?" → Presentation. "Did
Yoga compute the right frame?" → Layout.

**Space picker.** `ViewLocal` is the view's own coordinate system (intra-view
scroll / transform checks). `ViewGlobal` is the root surface (cross-view
distance assertions). `Window` adds the host window's offset (true window-
relative movement, once the host hook lands — see Limitations). `Screen` adds
the screen offset on top of Window.

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
| `Motion.loadFixture` | `{path}` | `{ok, event_count, max_frame, header:{version, policy, duration_scale}}` |
| `Motion.scrubTo` | `{frame}` | `{playhead_frame, emitted_count}` (broadcasts Motion.start/.sample/.end with `"replay":true`) |
| `Motion.play` | `{}` | `{playing, emitted_count, playhead_frame}` |
| `Motion.pause` | `{}` | `{playing:false, playhead_frame}` |

The server broadcasts `Motion.start`, `Motion.sample`, and `Motion.end` events
to all connected clients as samples are emitted. Subscribing clients receive a
clean stream for the trace they registered — concurrent unrelated animations
do not bleed into the stream unless the firehose is on (see below).

The timeline scrubber methods (`Motion.loadFixture` / `.scrubTo` / `.play` /
`.pause`) load a `.motion.jsonl` fixture into memory and re-emit the prefix
of events with `frame <= playhead` to the same event channel as live traces.
Replayed events carry an additional `"replay":true` marker so clients can
distinguish them from live coordinator events. The scrubber is passive — no
clock is pumped, no animation runs live; this is sufficient for design review
and CI artifact triage (live overlay drawing is a future phase).

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

## Swift / iOS / macOS

The same publish channel is reachable from Swift via the `PulpSwift` package.
The Swift facade mirrors the trace-builder DSL so the API reads the same way
from either language, and every entry point is a no-op when tracing is off —
zero cost in production.

```swift
import PulpSwift
import SwiftUI

struct CardView: View {
    @State private var opacity: Double = 1
    var body: some View {
        Rectangle()
            .opacity(opacity)
            .pulpMotionTrace("Card") {
                Trace.value("opacity", opacity)
                Trace.geometry("frame",
                    properties: [.minX, .minY, .width, .height])
                Trace.scrollGeometry("scroll")
            }
    }
}
```

The `pulpMotionTrace(_:fps:_:)` modifier registers a Coordinator trace
stamped with `source_kind="swiftui"` provenance, backs the view with a
hidden `GeometryReader` probe that pushes every new global-space frame into
`pulp_motion_update_geometry`, and detaches on `onDisappear`. The modifier
short-circuits when `PulpMotion.isTracingEnabled` is false — zero cost
beyond a SwiftUI background view in production.

For non-SwiftUI code paths (UIKit, AppKit, plain audio-graph callbacks)
the `PulpMotionGeometryProbe` class exposes the same attach / update /
detach contract:

```swift
final class CardUIView: UIView {
    private let probe = PulpMotionGeometryProbe(view: "Card")
    override func layoutSubviews() {
        super.layoutSubviews()
        probe.update(minX: frame.minX, minY: frame.minY,
                     width: frame.width, height: frame.height)
    }
    // deinit auto-detaches.
}
```

Direct scalar / component publishes:

```swift
PulpMotion.publishValue(view: "Card", metric: "opacity", value: 0.5)
PulpMotion.publishComponents(view: "Card", metric: "frame",
                             components: [("x", x), ("y", y)])

PulpMotion.setAmbientProvenance(kind: "swiftui", id: "CardView")
defer { PulpMotion.clearAmbientProvenance() }
```

Bridging model: `PulpSwift` is a pure-Swift package; the C bridge lives
in `apple/Sources/PulpSwift/PulpBridge.{h,cpp}` and is linked by the
AUv3 / standalone host. At launch the host installs a
`PulpMotionBackend` whose closures forward into the C ABI
(`pulp_motion_publish_value`, `pulp_motion_register_geometry_trace`,
…). Unit tests install a recording backend instead so
`swift test --package-path apple` runs without a C++ host.

Files: `apple/Sources/PulpSwift/PulpMotion.swift` (facade + `Trace.*`
factories + `@MotionTraceBuilder`), `PulpMotionProbe.swift` (SwiftUI
modifier + UIKit / AppKit `PulpMotionGeometryProbe`), `PulpBridge.{h,cpp}`
(C ABI), `apple/Tests/PulpSwiftTests/PulpMotionTests.swift` (XCTest
coverage), `test/test_motion_swift_bridge.cpp` (Catch2 round-trip).

## Kotlin / Android (Compose + View)

The Android facade mirrors the Swift one — same publish channel, same
`Trace.*` DSL names, same off-by-default contract. Samples flow into the same
`pulp::view::motion::Coordinator`, so fixtures, scrubber, cost attribution,
reduced-motion gating, and provenance envelopes all work identically.
Compose uses a `Modifier.pulpMotionGeometry(...)`; classic Views use a
`View.pulpMotionTrace(...)` extension.

```kotlin
import androidx.compose.ui.Modifier
import com.pulp.motion.PulpMotion
import com.pulp.motion.Trace
import com.pulp.motion.pulpMotionGeometry

@Composable
fun CardPanel(opacity: Float) {
    Box(
        Modifier
            .size(120.dp)
            .pulpMotionGeometry("Card") {
                +Trace.value("opacity", opacity.toDouble())
                +Trace.geometry("frame")
            }
    ) { /* content */ }
}
```

The modifier registers a Coordinator trace stamped with
`source_kind="android"` provenance, plumbs `boundsInWindow()` deltas
into `pulp_motion_update_geometry`, and detaches on
`DisposableEffect.onDispose`. It short-circuits when
`PulpMotion.isTracingEnabled` is false — zero composition-side cost
beyond a single branch.

For non-Compose UIs:

```kotlin
class CardView(context: Context) : View(context) {
    private val probe = pulpMotionTrace("Card") {
        +Trace.geometry("frame")
    }
}
```

`View.pulpMotionTrace` installs a `ViewTreeObserver.OnPreDrawListener`
(NOT `OnGlobalLayoutListener` — PreDraw catches intra-frame scroll
and translation deltas GlobalLayout misses) and auto-detaches when
the View is removed from the window. The returned
`PulpMotionGeometryProbe` is `AutoCloseable` so it works inside
`Closeable.use { }` too.

Direct scalar / component publishes:

```kotlin
PulpMotion.publishValue(view = "Card", metric = "opacity", value = 0.5)
PulpMotion.publishComponents(
    view = "Card",
    metric = "frame",
    components = rectF.toMotionComponents(),
)

PulpMotion.withProvenance(kind = "android", id = "CardView") {
    PulpMotion.publishValue(view = "Card", metric = "opacity", value = 1.0)
}
```

`withProvenance { }` is single-threaded by design — the process-wide
ambient slot is not coroutine-safe. Do not call from suspending code
that may switch dispatchers inside the block.

Bridging model: the Android facade lives in
`android/app/src/main/kotlin/com/pulp/motion/`; the JNI shims and
the matching C ABI live in
`core/platform/src/android/jni_motion.cpp` (compiled into the
`pulp-jni` shared library). `PulpApplication.onCreate` calls
`PulpMotion.installNativeBackend()` once after
`System.loadLibrary("pulp")` to wire the closure-bag backend to
JNI. JVM unit tests install a recording backend instead, so
`gradle test` exercises the facade without ever loading
`libpulp.so`.

Files: `android/app/src/main/kotlin/com/pulp/motion/PulpMotion.kt` (facade),
`Trace.kt` (DSL + `Rect.toMotionComponents()` helpers), `MotionProbe.kt`
(`View.pulpMotionTrace` + `PulpMotionGeometryProbe`), `MotionCompose.kt`
(`Modifier.pulpMotionGeometry`), `PulpMotionBackend.kt` +
`PulpMotionNative.kt` (closure-bag seam + JNI declarations),
`core/platform/src/android/jni_motion.cpp` (C ABI + JNI shims),
`android/app/src/test/kotlin/com/pulp/motion/PulpMotionTest.kt` (JVM unit
tests, no native lib), `test/test_motion_android_bridge.cpp` (Catch2
round-trip).

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
| `local_step_outlier_ratio(samples, window_radius, epsilon)` | Max ratio of any step magnitude to the median step magnitude in a local window — flags one-frame jumps. **Separate from** `is_monotonic` / `overshoot` / timing helpers; does NOT conflate continuity, timing, or direction. |

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
  `mean_confidence`, `min_confidence`, optional `affine_first_to_last`,
  schema version
- `summary.md` — agent-readable summary with the claim-evidence preamble
- `diff/diff_NNNN_NNNN.png` — pairwise pixel-diff heatmaps (capped at
  `--max-diff-frames`)
- `keyframes.png` — keyframe sprite (first, mid, last, plus top-delta pairs)
- With `--grid`: `grid/frame_NNNN.png`, `diff_grid/diff_NNNN_NNNN.png`, and
  `keyframes_grid.png` siblings (alphanumeric cell overlay)

The schema is versioned (`REPORT_SCHEMA_VERSION`) so downstream consumers can
reject unknown formats. Dependencies (numpy, Pillow, scikit-image) are MIT /
BSD / Apache 2.0 only and are not redistributed in plugin or app artifacts.

### Recommended flags (visual+)

| Flag | Default | What it does |
|---|---|---|
| `--pattern` | `frame_*.png` | Glob for frame filenames |
| `--keyframes N` | `2` | Top-delta keyframes in addition to first / mid / last |
| `--max-diff-frames N` | `8` | Cap on emitted diff PNGs; `0` = unlimited (all pairs) |
| `--grid` | off | Emit alphanumeric grid overlays (A1..H12) on `grid/`, `diff_grid/`, and `keyframes_grid.png` so claims can cite a cell |
| `--grid-rows N` | `8` | Grid overlay rows (A..Z) |
| `--grid-cols N` | `12` | Grid overlay columns (1..N) |
| `--grid-theme` | `auto` | `auto` (samples corner luminance) \| `light` \| `dark` |
| `--trim` | off | Drop idle prefix + suffix from the analysis window (frames stay on disk; `summary.md` reports `trimmed_leading_frames` / `trimmed_trailing_frames`) |
| `--trim-threshold F` | `0.01` | Mean-diff luminance fraction for `--trim` |
| `--affine` | off | Estimate translation / rotation / scale first→last (opencv if installed, else PIL-FFT translation only — see `requirements-optional.txt`); emits `analysis.json#affine_first_to_last` and a `## Net motion` section in `summary.md` |

### Motion-gated capture from a host source

When you don't already have a frame directory, capture one with the gated
helper. It only starts saving frames once real motion appears, so a short
pre-roll doesn't pollute the analysis window:

```bash
# macOS window region (requires --bounds X,Y,W,H)
python3 tools/motion/visual/capture_sim_frames.py \
    --source macos --bounds 0,0,800,600 \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60 \
    --gate-threshold 4.0 --gate-consecutive 1 \
    --idle-timeout 8

# Booted iOS Simulator (works well alongside XcodeBuildMCP)
python3 tools/motion/visual/capture_sim_frames.py \
    --source simulator \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60
```

The capture tool exits 3 (CTest SKIP) when neither `screencapture` nor a
booted simulator is available, so it composes cleanly with CI lanes that lack
the platform tooling.

### Claim-evidence preamble

Every claim made from a visual-analysis report must cite (1) pair index
(`NN→NN+1`), (2) artifact type (`frames/`, `diff/`, `grid/`, `diff_grid/`,
`keyframes.png`, or `affine_first_to_last`), and (3) a confidence score
`0.0..1.0` from `pairs[].confidence` / `summary.mean_confidence`. **Confidence
< 0.7 means the analyzer is unsure** — escalate by re-running with
`--max-diff-frames 0` (all pairs), a longer capture window, or fall back to a
runtime trace (Path A in the motion skill) if instrumentation is possible. The
`summary.md` preamble carries the same contract verbatim so any downstream
consumer that reads the report gets the same rules.

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

## Cost attribution

When the question is "which animation is expensive and why?", the
fixture stream of values is not enough — you also want per-frame render
cost joined with the trace activity that drove it. The cost-attribution
channel does that.

It is **off by default** and runs on a stream separate from the fixture
format. Cost samples carry their own version header
(`{"motion_cost_version":1}`) and are written to `*.motion-cost.jsonl`,
not the regular `*.motion.jsonl` events.

```cpp
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/motion_cost_render.hpp>

auto& cost = pulp::view::motion::CostAttributor::instance();
cost.set_enabled(true);
cost.add_sink(motion::make_cost_sink("/tmp/run.motion-cost.jsonl"));

// Surface real render-pass + dirty-rect stats via the bridge probe.
// Either pointer may be null — defensive fields default to 0.
cost.set_probe(motion::make_render_cost_probe(
    &render_pass_manager, &dirty_tracker));
```

Each `CostSample` carries:

| Field | Source |
|---|---|
| `frame`, `t_seconds` | FrameClock at tick time |
| `render_pass_duration_ms` | `RenderPassManager::total_time_ms()` |
| `dirty_rect_area_px`, `dirty_rect_count` | `DirtyTracker::dirty_rects()` |
| `active_trace_ids` | every `trace_id` that emitted on this frame |
| `active_provenance` | per-trace Phase 9 envelope (parallel to `active_trace_ids`) |

Cost samples emit even when the coordinator has no event sinks — the
render-cost timeline is useful by itself even when no motion trace is
active.

The inspector exposes `Motion.enableCost` / `Motion.disableCost`
requests and broadcasts a `Motion.cost` event per frame while enabled.
`Motion.snapshot` reports `cost_enabled` and `cost_samples_emitted` so
a client can verify the channel is live without subscribing.

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
