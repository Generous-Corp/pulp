---
name: motion
description: Debug or validate Pulp animations / transitions / scroll behavior using the runtime motion-trace system. TRIGGER on phrases like "animation is wrong", "transition timing off", "fade too late", "card slides too far", "scroll jumps on restore", "scroll offset wrong on restore", "frame jump in the timeline", "easing looks wonky", "imported Figma motion doesn't match source", "settle time / overshoot / drift / monotonic", "what value does the knob reach at frame N", "record this animation so I can replay it", "this animation is expensive — which one and why", "reduced-motion broken", "scrub a captured fixture". Runs over the inspector wire (no source edits needed), captures fixtures (.motion.jsonl), and ships assertion helpers (is_monotonic / settling_time_seconds / overshoot / start_delay_seconds / final_value / local_step_outlier_ratio) and the `scroll_geometry` trace for `ScrollView` content-offset / visible-rect / content-size observability. Off by default in prod.
---

# Motion

Pulp's agent-first motion observability — sample view geometry / scalar
values / scroll state over time, emit epsilon-bounded events with monotonic
timestamps and burst framing, and route them to log lines + inspector
events + JSONL fixtures. **You are reading this skill because an agent
needs to debug, validate, or reproduce a motion behavior.**

## When to rope this skill in

Trigger this skill the moment a user (or your own reasoning) describes any
of these symptoms — don't reach for `grep` or `git log`, attach a trace:

- "this fade / slide / scale is too fast / too slow / starts late / ends early"
- "the knob value isn't reaching its target" / "what value at frame N?"
- "two elements drift apart during the transition"
- "scroll position jumps when I restore state"
- "imported design's motion doesn't match the source intent"
- "is this animation monotonic / does it overshoot / how long to settle?"
- "which animation is expensive and why?" → Path F (cost attribution)
- "reduced-motion path is broken" → Path B + assert under MotionPolicy
- "I want to scrub through a recorded fixture" → Path E (scrubber)

If the user says any of those, this skill applies. Don't suggest reading
source; **attach a trace and read the numbers**.

## Quick decision

Eight paths — pick by what you have, all eight terminate at the same
`motion::Coordinator` so fixtures, scrubber, cost, reduced-motion, and
provenance work identically across surfaces.

| You have | Path | Tool |
|---|---|---|
| A running app + a node id + a scalar / geometry of interest | **A — Runtime trace** | `Motion.startTrace` over the inspector wire |
| A `ScrollView` whose offset / visible rect / content size you need to observe | **A — Runtime trace (scroll)** | `Trace.scroll_geometry(name, scroll_view, props)` — emits `contentOffsetX/Y`, `visibleRect*`, `contentSize*`, `scrollableMax*`, `inset*` |
| A captured frame sequence (no app instrumentation available) | **B — Visual analysis** | `tools/motion/visual/analyze_sequence.py` |
| A previously recorded `.motion.jsonl` fixture | **C — Replay + assert** | `motion::replay_fixture` + `motion::assert_matches` |
| An interaction that drives the suspect motion | **D — Input record + replay** | `motion::make_input_recorder` + `motion::replay_inputs` |
| A fixture + an inspector client (design review / CI triage) | **E — Timeline scrubber** | `Motion.loadFixture` + `Motion.scrubTo` over the inspector wire |
| "Which animation is expensive and why?" | **F — Cost attribution** | `Motion.enableCost` + `CostAttributor` + `make_render_cost_probe` |
| SwiftUI / UIKit / AppKit / iOS / macOS / AUv3 host code path | **G — Swift native** | `View.pulpMotionTrace { Trace.* }` / `PulpMotionGeometryProbe` |
| Jetpack Compose or Android `View` code path | **H — Android native** | `Modifier.pulpMotionGeometry { +Trace.* }` / `View.pulpMotionTrace` |
| Imported design + intent doc (e.g. "fade in 350 ms ease-out") | **Both A + C** | Record a fixture from the import, assert timing / monotonicity |

## Path A — Runtime trace

The runtime path attaches a trace at runtime over the inspector wire, no
source instrumentation, no cleanup phase to forget.

### 1. Confirm the complaint as a measurable property

Translate "looks off" into a metric and a target. Example mappings:

- "fade is slow" → `opacity` scalar, settling time > X ms
- "card slides too far" → `frame` geometry, final `minY` mismatch
- "two cards drift" → two `frame` traces, deltas correlate
- "scroll jumps on restore" → child-of-ScrollView geometry, presentation source

### 2. Start the host with the motion server enabled

```bash
PULP_MOTION_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview
# Motion inspector listening on port 9147
```

### 3. Attach a trace

Send a `Motion.startTrace` request (TCP port 9147, line-delimited JSON):

```jsonc
{ "id": 1, "method": "Motion.startTrace",
  "params": {
    "view_name": "Card",
    "fps": 30,
    "metrics": [{
      "kind": "geometry",
      "name": "frame",
      "node_id": "card",
      "properties": ["minX","minY","width","height"],
      "space": "window",
      "source": "presentation"
    }]
  }
}
```

Response: `{ "trace_id": 1 }`.

Stream of events: `Motion.start`, `Motion.sample` (one per change), `Motion.end`
(with deltas).

### 4. Trigger the interaction

Drive the app — click, hover, type, fire whatever causes the suspected
animation. The motion server emits events as the values change.

### 5. Stop the trace

```jsonc
{ "id": 2, "method": "Motion.stopTrace", "params": { "trace_id": 1 } }
```

### 6. Compare against intent

Did the trace match? Use the dedicated assertion helpers — do not conflate
continuity, monotonicity, settling time, and overshoot into a single number.

```cpp
auto samples = motion::extract_scalar(events, "Card", "frame", "minY");
REQUIRE(motion::is_monotonic(samples));
REQUIRE(motion::settling_time_seconds(samples) < 0.7);
REQUIRE(motion::overshoot(samples) < 0.05);
REQUIRE(motion::final_value(samples) == Catch::Approx(120.0).margin(1.0));
// Local-step outlier check is a *separate* axis from monotonicity /
// timing — flags one-frame jumps (5x = "this one step is 5x larger
// than the median of its neighbors"). Use it alongside, not in place
// of, the helpers above.
REQUIRE(motion::local_step_outlier_ratio(samples) < 3.0);
```

## Path B — Visual analysis

Use when no scalar is observable (transitions, GPU filters, mask compositing,
opacity-only effects you suspect aren't rendering).

### 1. Capture a frame sequence

Any source: a host window-capture loop, an `ffmpeg` extraction from a screen
recording, scripted PNG dumps. Land sequential frames in a directory:

```
captures/card-open/
  frame_0000.png
  frame_0001.png
  …
```

### 2. Run the pipeline

```bash
pip install -r tools/motion/visual/requirements.txt
python3 -m tools.motion.visual.analyze_sequence \
    --frames-dir ./captures/card-open/ \
    --output     ./reports/card-open/ \
    --keyframes 2

# Recommended flags for design / motion review:
python3 -m tools.motion.visual.analyze_sequence \
    --frames-dir ./captures/card-open/ \
    --output     ./reports/card-open/ \
    --grid                      # alphanumeric cell overlay (A1..H12) on
                                # frames/diffs/sprite so claims can cite a cell
    --trim                      # drop idle prefix + suffix from the analysis
                                # window (frames stay on disk)
    --affine                    # estimate translation/rotation/scale first→last
                                # (uses opencv if installed, else PIL-FFT
                                # translation only — see requirements-optional.txt)
```

Tunables: `--grid-rows N` (default 8, A..Z), `--grid-cols N` (default 12),
`--grid-theme auto|light|dark` (default auto — samples corner luminance),
`--trim-threshold` (default 0.01 mean-diff luminance fraction).

### 2b. Motion-gated capture from a host source

When you don't already have a frame directory, capture one with the
gated helper. It only starts saving frames once real motion appears,
so a short pre-roll doesn't pollute the analysis window.

```bash
# macOS window region (requires --bounds X,Y,W,H)
python3 tools/motion/visual/capture_sim_frames.py \
    --source macos --bounds 0,0,800,600 \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60 \
    --gate-threshold 4.0 --gate-consecutive 1 \
    --idle-timeout 8

# Booted iOS Simulator
python3 tools/motion/visual/capture_sim_frames.py \
    --source simulator \
    --output-dir ./captures/card-open/ \
    --fps 30 --frame-count 60
```

The capture tool exits 3 (ctest SKIP) when neither `screencapture`
nor a booted simulator is available, so this composes cleanly with
CI lanes that lack the platform tooling.

### 3. Read the report

The pipeline writes `analysis.json` (machine-readable), `summary.md`
(agent-readable), `diff/diff_NNNN_NNNN.png` (pairwise heatmaps), and
`keyframes.png` (sprite). With `--grid`: `grid/frame_NNNN.png`,
`diff_grid/diff_NNNN_NNNN.png`, and `keyframes_grid.png` siblings.
With `--affine`: an `analysis.json#affine_first_to_last` block and a
`## Net motion` section in `summary.md`. With `--trim`:
`summary.trimmed_leading_frames` / `summary.trimmed_trailing_frames`.

The JSON carries `schema_version` — refuse unknown versions.

**Claim-evidence contract.** Every claim you make from this report
must cite: (1) pair index (`NN→NN+1`), (2) artifact type (`frames/`,
`diff/`, `grid/`, `diff_grid/`, `keyframes.png`, or
`affine_first_to_last`), and (3) a confidence score 0.0..1.0 from
`pairs[].confidence` / `summary.mean_confidence`. **Confidence < 0.7
means the analyzer is unsure** — escalate by re-running with
`--max-diff-frames 0` (all pairs), a longer capture window, or fall
back to a runtime trace (Path A) if instrumentation is possible. The
`summary.md` preamble carries the same contract verbatim.

## Path C — Fixtures (record / replay / assert)

A fixture is the on-disk form of a motion stream — a versioned JSONL file. Use
fixtures to:

- Land a golden alongside a feature; regress against it in CI.
- Validate that an imported design's motion matches its source intent.
- Reproduce a bug deterministically by replaying a captured run offline.

### Record

```cpp
auto sink = motion::make_fixture_sink("test/motion/goldens/card-open.motion.jsonl");
int sid = Coordinator::instance().add_sink(std::move(sink));
// ... run the animation ...
Coordinator::instance().remove_sink(sid);  // closes the file
```

### Replay

```cpp
std::vector<motion::SampleEvent> replayed;
motion::replay_fixture("captures/card-open.motion.jsonl",
                       motion::make_buffer_sink(&replayed));
```

### Assert

```cpp
auto golden   = motion::load_fixture("test/motion/goldens/card-open.motion.jsonl");
auto captured = motion::load_fixture("/tmp/run.motion.jsonl");
auto diff     = motion::assert_matches(golden, captured);
REQUIRE(diff.matches());  // or inspect diff.differences on failure
```

`FixtureMatchOptions { component_epsilon, timing_epsilon_seconds,
require_same_event_count }` controls tolerances.

## Path D — Input recording and replay

When the bug is "what the user did caused the wrong motion", record the
interaction alongside the motion stream so a fresh tree can replay the same
sequence deterministically.

```cpp
// Recording — paired with whatever motion sinks you already have.
{
    auto recorder = motion::make_input_recorder("/tmp/card-open.motion.jsonl");
    root.simulate_hover({150, 150});
    clock.tick(1.0f / 60.0f);
    root.simulate_click({150, 150});
    // ... drive your animation ...
}   // RAII: destructor closes the sink + flips recording off.

// Replay against a fresh tree on a fresh FrameClock.
motion::replay_inputs("/tmp/card-open.motion.jsonl", fresh_root, fresh_clock);
```

`make_input_recorder(path)` installs a `make_fixture_sink(path)` AND flips
the process-wide `input_recording_enabled()` flag. `View::simulate_*` checks
that flag (a single relaxed atomic load, off by default) and emits a
`SampleEvent::Kind::Input` carrying the `input_kind` ("click" / "drag" /
"hover"), the recorded target's `View::id()`, and the root-space coords on
the existing `components` map (sorted by name: `x`/`y` for click+hover;
`start_x`/`start_y`/`end_x`/`end_y`/`steps` for drag).

`replay_inputs(path, root, clock)`:

- Walks every `Input` event in fixture order.
- Advances `clock` to match the recorded `t_seconds` (first input anchors,
  subsequent inputs tick by the delta).
- Dispatches each input through `root` (not the recorded `view_id` — root
  coords with `hit_test` land on the same descendant).
- Returns the number of inputs replayed.

The motion stream that emerges — when paired with the same animation
primitives — matches the originally-recorded one within
`FixtureMatchOptions::timing_epsilon_seconds`. Use the ID-keyed
`assert_matches` for the comparison so reordered identical bursts don't
false-fail.
## Path E — Timeline scrubber (inspector replay)

`pulp::inspect::MotionScrubber` loads a `.motion.jsonl` fixture and
re-emits the prefix of events with `frame <= playhead` to caller
sinks and (when attached to an `InspectorServer`) to inspector clients
over the wire. The scrubber is passive — no clock is pumped, no
animation runs live; `play()` is a jump-to-end that emits every event.
Real-time pacing and live overlay drawing are intentionally Phase 11+.

Protocol surface (routed by `DomainHandler::handle_motion`):

| Method               | Params              | Response                                                |
|----------------------|---------------------|---------------------------------------------------------|
| `Motion.loadFixture` | `{ path }`          | `{ ok, event_count, max_frame, header: {version,policy,duration_scale} }` |
| `Motion.scrubTo`     | `{ frame }`         | `{ playhead_frame, emitted_count }` + broadcast events  |
| `Motion.play`        | `{}`                | `{ playing:true, emitted_count, playhead_frame }`       |
| `Motion.pause`       | `{}`                | `{ playing:false, playhead_frame }`                     |

Broadcast events reuse `MotionInspector`'s `Motion.start / .sample /
.end` shape, with an additional `"replay":true` marker so clients can
distinguish replayed bursts from live coordinator events on the same
wire.

Direct C++ usage:

```cpp
pulp::inspect::MotionScrubber scrub(/*server=*/nullptr);
std::vector<motion::SampleEvent> buf;
scrub.add_sink(motion::make_buffer_sink(&buf));
scrub.load_fixture("captures/card-open.motion.jsonl");
scrub.scrub_to(120);   // emits prefix with frame <= 120
scrub.scrub_to(0);     // emits only the frame-0 prefix (backwards scrub)
scrub.play();          // jump to max frame, emit everything
```

Gotchas:

- `load_fixture` is passive. Sinks see no events until `scrub_to` /
  `play` is called. Don't pre-clear UI overlays on `loadFixture` and
  expect a refill — wait for the first `scrub_to`.
- Backwards scrubs re-emit from frame 0. If your sink accumulates,
  clear it before each scrub or compare counts modulo the prefix size.
- Replayed event timestamps (`t_seconds`) are the recording's
  timestamps, not wall clock. Don't drive a live clock from them.
## Path F — Cost attribution

When the question is **"which animation is expensive and why?"**, switch
the cost channel on. It's off by default and runs on a separate stream
from the fixture format — cost samples don't appear in `*.motion.jsonl`.

### Enable + wire a probe (in-process)

```cpp
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/motion_cost_render.hpp>

auto& cost = pulp::view::motion::CostAttributor::instance();
cost.set_enabled(true);

// Optional but recommended: surface real render stats. Pointers may be
// null — defensive degradation returns 0 for the missing field.
cost.set_probe(pulp::view::motion::make_render_cost_probe(
    &render_pass_manager, &dirty_tracker));

// Sink: JSONL on disk for later analysis…
cost.add_sink(pulp::view::motion::make_cost_sink("/tmp/run.motion-cost.jsonl"));
// …or a buffer for in-test assertions:
std::vector<pulp::view::motion::CostSample> samples;
cost.add_sink(pulp::view::motion::make_cost_buffer_sink(&samples));
```

Each frame, the Coordinator's tick now emits one `CostSample` per active
sink with:

- `frame`, `t_seconds`
- `render_pass_duration_ms` — from `RenderPassManager::total_time_ms()`
- `dirty_rect_area_px`, `dirty_rect_count` — from `DirtyTracker::dirty_rects()`
- `active_trace_ids` — every `trace_id` that emitted on this frame
- `active_provenance` — Phase 9 envelopes for those traces, in the same
  order, so a reader can answer "this 12ms pass came from
  `figma:LevelMeter/Panel` (source_kind=`design-import`)" without
  cross-referencing the event fixture.

### Inspector domain

`Motion.enableCost` / `Motion.disableCost` toggle the channel; while
enabled, `Motion.cost` events broadcast per frame. `Motion.snapshot`
also reports `cost_enabled` and `cost_samples_emitted`.

### Notes

- Cost samples are a separate JSONL stream (`*.motion-cost.jsonl`) with
  its own version header (`{"motion_cost_version":1}`). Do not confuse
  with the fixture schema — they're independent.
- When the coordinator has no event sinks but cost is enabled, the
  attributor still emits cost samples — the render-cost timeline is
  useful by itself even without any motion trace activity.
- The render-cost probe is read outside the coordinator lock; keep
  implementations cheap.

## Path G — Swift native (iOS / AUv3 / macOS)

When the suspect motion lives in a pure SwiftUI / UIKit / AppKit code
path — an iOS AUv3 editor, a Swift host app, anything wired through
`apple/Sources/PulpSwift/` — use the Swift facade. Samples flow into
the same `motion::Coordinator` as the JS-bridge and design-import
paths, so fixtures, scrubber, cost attribution, reduced-motion gating,
and provenance envelopes all work identically.

### Quick attach (SwiftUI)

```swift
import PulpSwift
import SwiftUI

struct CardView: View {
    @State var opacity: Double = 1
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

The `pulpMotionTrace(_:fps:_:)` modifier:

- Registers a geometry trace stamped with `source_kind="swiftui"` /
  `source_id=<view name>` provenance on `onAppear`.
- Backs the view with a hidden `GeometryReader` that pushes every new
  global-space frame into `pulp_motion_update_geometry`.
- Detaches on `onDisappear` (RAII).
- Short-circuits when `PulpMotion.isTracingEnabled` is false. No
  registration, no probe, zero cost beyond a SwiftUI background view.

### Direct publish (no SwiftUI)

```swift
PulpMotion.publishValue(view: "Card", metric: "opacity", value: 0.5)
PulpMotion.publishComponents(view: "Card", metric: "frame",
                             components: [("x", x), ("y", y)])
```

Both are guarded by the backend's `isTracingEnabled()` so they cost a
single branch when motion is off.

### UIKit / AppKit / non-SwiftUI probe

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

### Host wiring (once at app launch)

`PulpSwift` is a pure-Swift package; the C bridge lives in
`apple/Sources/PulpSwift/PulpBridge.cpp` (excluded from SwiftPM and
linked by the AUv3 / standalone host). At launch the host installs a
`PulpMotionBackend` that forwards into the C ABI:

```swift
var backend = PulpMotionBackend()
backend.isTracingEnabled  = { pulp_motion_tracing_enabled() }
backend.publishValue      = { v, m, val, eps, p in
    v.withCString { vc in m.withCString { mc in
        pulp_motion_publish_value(vc, mc, val, eps, Int32(p))
    }}
}
// …same shape for publishComponents, ambient provenance,
//   registerGeometryTrace, updateGeometry, detachTrace.
PulpMotionRuntime.installBackend(backend)
```

In unit tests, install a recording backend instead — `swift test
--package-path apple` runs the facade with no C++ host linked.

### Off-by-default contract

Every Swift entry point is a no-op when the process-wide Coordinator
has tracing disabled. The C bridge double-checks
(`pulp_motion_tracing_enabled()`) so even a misconfigured Swift caller
cannot spam events in production.

### Files

- `apple/Sources/PulpSwift/PulpBridge.h` — C ABI (publish + provenance
  + register / update / detach geometry).
- `apple/Sources/PulpSwift/PulpBridge.cpp` — bridge to
  `pulp::view::motion`. Internal mutex-protected registry keeps the
  `TraceHandle` separate from the lambda-captured atomic rect so
  `Coordinator::reset()` cannot self-deadlock.
- `apple/Sources/PulpSwift/PulpMotion.swift` — `PulpMotion` facade,
  `Trace.*` factories, `MotionGeometryProperty`,
  `@MotionTraceBuilder`, `PulpMotionBackend` / `PulpMotionRuntime`.
- `apple/Sources/PulpSwift/PulpMotionProbe.swift` — SwiftUI
  `pulpMotionTrace(_:)` modifier + UIKit / AppKit
  `PulpMotionGeometryProbe`.
- `apple/Tests/PulpSwiftTests/PulpMotionTests.swift` — Swift facade
  XCTest coverage.
- `test/test_motion_swift_bridge.cpp` — Catch2 round-trip test for the
  C ABI shims and Coordinator integration.

## Path H — Android native (Kotlin / Compose / View)

When the suspect motion lives in a Kotlin / Compose / Android `View`
code path — a Pulp Android app screen, a TalkBack overlay, a
SurfaceView driver — use the Kotlin facade. Samples flow into the
same `motion::Coordinator` as the Swift, JS-bridge, and design-import
paths, so fixtures, scrubber, cost attribution, reduced-motion
gating, and provenance envelopes all work identically. The Android
bridge is the platform sibling of Path G and follows the same shape
(C ABI + closure-bag backend seam + probe wrapper).

### Quick attach (Jetpack Compose)

```kotlin
import androidx.compose.ui.Modifier
import com.pulp.motion.PulpMotion
import com.pulp.motion.Trace
import com.pulp.motion.pulpMotionGeometry

Box(
    Modifier
        .size(120.dp)
        .pulpMotionGeometry("Card") {
            +Trace.value("opacity", opacity.toDouble())
            +Trace.geometry("frame",
                properties = listOf(
                    MotionGeometryProperty.minX,
                    MotionGeometryProperty.minY,
                    MotionGeometryProperty.width,
                    MotionGeometryProperty.height,
                ))
        }
) { ... }
```

The `Modifier.pulpMotionGeometry(name, fps)` modifier:

- Registers a geometry trace stamped with `source_kind="android"`
  provenance (set on the C bridge).
- Plumbs `boundsInWindow()` deltas from `onGloballyPositioned` into
  `pulp_motion_update_geometry`.
- Detaches on `DisposableEffect.onDispose` (composition exit).
- Short-circuits when `PulpMotion.isTracingEnabled` is false. Zero
  Compose-side cost beyond a single branch in `composed { }`.

### Quick attach (View hierarchy)

For non-Compose UIs (XML layouts, `SurfaceView`, custom Views):

```kotlin
class CardView(context: Context) : View(context) {
    private val probe = pulpMotionTrace("Card") {
        +Trace.geometry("frame")
    }
}
```

`View.pulpMotionTrace(name, fps) { ... }` returns a
`PulpMotionGeometryProbe?` — `null` when tracing is disabled, an
`AutoCloseable` handle otherwise. The probe installs a
`ViewTreeObserver.OnPreDrawListener` (NOT `OnGlobalLayoutListener` —
PreDraw catches intra-frame scroll/translation GlobalLayout misses)
and pushes window-space rects via `getLocationInWindow()`. Auto-
detaches when the host View is removed from the window.

### Direct publish (no View / no Compose)

```kotlin
PulpMotion.publishValue(view = "Card", metric = "opacity", value = 0.5)
PulpMotion.publishComponents(
    view = "Card",
    metric = "frame",
    components = rect.toMotionComponents(),
)
```

Both short-circuit on `PulpMotion.isTracingEnabled` so they cost a
single branch when motion is off.

### Ambient provenance (scoped)

```kotlin
PulpMotion.withProvenance(kind = "android", id = "CardView") {
    PulpMotion.publishValue(view = "Card", metric = "opacity", value = 1.0)
}
```

`withProvenance` is **single-threaded by design** — the process-wide
ambient slot is not coroutine-safe. Do not call from suspending code
that may switch dispatchers inside the block. The Swift bridge ships
the same constraint; this is a deliberate limit, not a TODO.

### Host wiring (once at app launch)

`PulpApplication.onCreate`, after `System.loadLibrary("pulp")`:

```kotlin
if (nativeLoaded) {
    com.pulp.motion.PulpMotion.installNativeBackend()
}
```

`installNativeBackend()` wires every `PulpMotionBackend` closure to
the matching `external fun` on the internal `PulpMotionNative`
object. The C bridge double-checks
`motion::Coordinator::tracing_enabled()` so a misconfigured backend
cannot spam events in production.

### Testability (JVM unit tests, no NDK)

The closure-bag backend lets `gradle test` exercise the facade
without ever loading `libpulp.so`:

```kotlin
val recorder = RecorderBackend()
PulpMotionRuntime.installBackend(recorder.asBackend())
PulpMotion.publishValue("Card", "opacity", 0.5)
assertEquals(1, recorder.publishedValues.size)
```

See `android/app/src/test/kotlin/com/pulp/motion/PulpMotionTest.kt`
for the full pattern.

### Off-by-default contract

Every Kotlin entry point is a no-op when the process-wide Coordinator
has tracing disabled. The C bridge re-checks
`pulp_motion_tracing_enabled()` so even a misconfigured caller
cannot spam events in production. The View probe extension returns
`null` immediately when tracing is off — no listener installed, no
allocation.

### Files

- `core/platform/include/pulp/platform/android/motion_bridge.h` — C
  ABI declarations the JNI shims forward into (also pulled by the
  Catch2 host test).
- `core/platform/src/android/jni_motion.cpp` — bridge implementation:
  C ABI + `Java_com_pulp_motion_PulpMotionNative_*` JNI shims.
  Internal mutex-protected registry keeps the `TraceHandle`
  separate from the lambda-captured atomic rect so
  `Coordinator::reset()` cannot self-deadlock — same fix shape as
  the Swift bridge.
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotionNative.kt` —
  `internal object` with `external fun` JNI declarations.
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotionBackend.kt` —
  closure-bag backend + `PulpMotionRuntime.installBackend(...)`.
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotion.kt` —
  public facade (`publishValue`, `publishComponents`,
  `setAmbientProvenance`, `withProvenance`, register / update /
  detach, `installNativeBackend()`).
- `android/app/src/main/kotlin/com/pulp/motion/Trace.kt` — `Trace.*`
  factories, `MotionGeometryProperty`, `@MotionTraceBuilder`
  `motionTrace { }` block, `RectF`/`Rect.toMotionComponents()`.
- `android/app/src/main/kotlin/com/pulp/motion/MotionProbe.kt` —
  `View.pulpMotionTrace(...)` extension + `PulpMotionGeometryProbe`
  (`AutoCloseable`).
- `android/app/src/main/kotlin/com/pulp/motion/MotionCompose.kt` —
  `Modifier.pulpMotionGeometry(...)` modifier + `pulpMotionPublish(...)`
  composable.
- `android/app/src/test/kotlin/com/pulp/motion/PulpMotionTest.kt` —
  JVM facade unit tests (JUnit4 only, no Mockito, no native lib).
- `test/test_motion_android_bridge.cpp` — Catch2 round-trip test for
  the C ABI + the `Coordinator::reset()` deadlock regression.

## Agent contract

Apply these on every motion debugging run:

1. State the complaint as a measurable target before instrumenting.
2. Use runtime trace by default. Drop to visual analysis only when no scalar
   is observable.
3. Pick the metric, space, and source explicitly. Defaults are reasonable,
   but the intent should be obvious from your request.
4. Keep continuity, monotonicity, settling time, and overshoot as separate
   assertions. Do not write a single "smoothness" check that conflates them.
5. Cite evidence with frame indices, sample timestamps, and metric values when
   reporting. "Looks wrong" without evidence is not a finding.
6. Land a golden fixture when fixing a bug — the next regression should fail
   in CI, not after a user reports it.

## Env knobs (standalone `pulp-ui-preview`)

| Variable | Effect |
|---|---|
| `PULP_MOTION_LOG=1` | Install the default log sink + enable tracing |
| `PULP_MOTION_SERVER=1` | Start the Motion inspector server on port 9147 |
| `PULP_MOTION_FIREHOSE=1` | Broadcast every `publish_*` call to all sinks |

## Files this skill covers

- `core/view/include/pulp/view/motion.hpp` — public C++ API
- `core/view/src/motion.cpp` — Coordinator, geometry walker, fixture I/O, assertions
- `core/view/include/pulp/view/motion_cost.hpp` — `CostSample` / `CostAttributor` / cost JSONL
- `core/view/include/pulp/view/motion_cost_render.hpp` — `make_render_cost_probe` bridge
- `core/view/src/motion_cost.cpp` — attributor singleton + sinks + JSONL load
- `core/view/include/pulp/view/motion_preferences.hpp` — reduced-motion policy + duration_scale
- `core/view/src/motion_preferences.cpp` — singleton + override + OS readers
- `core/view/platform/mac/motion_preferences_mac.mm` — NSWorkspace reduced-motion query
- `core/view/platform/win/motion_preferences_win.cpp` — SPI_GETCLIENTAREAANIMATION query
- `inspect/include/pulp/inspect/motion_inspector.hpp` — Motion inspector bridge
- `inspect/src/motion_inspector.cpp` — protocol handler + event broadcaster
- `inspect/include/pulp/inspect/motion_scrubber.hpp` — timeline scrubber (Phase 7)
- `inspect/src/motion_scrubber.cpp` — passive fixture replay + scrubber dispatch
- `tools/motion/visual/analyze_sequence.py` — visual analysis CLI (grid overlay, --trim, --affine, claim-evidence contract)
- `tools/motion/visual/capture_sim_frames.py` — motion-gated capture from macOS region or booted simulator
- `tools/motion/visual/test_self_check.py` — pipeline self-check (baseline + claim-evidence assertions)
- `tools/motion/visual/test_grid_overlay.py` — visual-plus self-check (grid, trim, affine)
- `tools/motion/visual/test_capture_smoke.py` — gated-capture smoke (skip 3 without source)
- `tools/motion/visual/requirements-optional.txt` — opt-in deps (opencv-python for full affine)
- `examples/ui-preview/main.cpp` — env-knob wiring for the standalone host
- `apple/Sources/PulpSwift/PulpBridge.h` — Swift C ABI surface (Path G)
- `apple/Sources/PulpSwift/PulpBridge.cpp` — Swift bridge shims (Path G)
- `apple/Sources/PulpSwift/PulpMotion.swift` — Swift facade + Trace DSL (Path G)
- `apple/Sources/PulpSwift/PulpMotionProbe.swift` — SwiftUI / UIKit probe (Path G)
- `core/platform/include/pulp/platform/android/motion_bridge.h` — Android C ABI (Path H)
- `core/platform/src/android/jni_motion.cpp` — Android JNI bridge + C ABI (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotion.kt` — Kotlin facade (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotionBackend.kt` — closure-bag backend seam (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/PulpMotionNative.kt` — internal JNI declarations (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/Trace.kt` — Trace DSL + Rect helpers (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/MotionProbe.kt` — View probe + AutoCloseable handle (Path H)
- `android/app/src/main/kotlin/com/pulp/motion/MotionCompose.kt` — Compose modifier (Path H)
- `android/app/src/test/kotlin/com/pulp/motion/PulpMotionTest.kt` — JVM facade unit tests (Path H)
- `test/test_motion_android_bridge.cpp` — Catch2 round-trip for the C ABI (Path H)
- `docs/guides/motion-observability.md` — full guide

## Provenance

Every trace can carry a `Provenance` envelope that flows through the fixture
to agents reading a golden weeks later:

```cpp
auto handle = motion::Coordinator::instance()
    .trace("Card", { 60 })
    .with_provenance({ "tween", "Card.opacity", __FILE__, __LINE__ })
    .value("opacity", [&]{ return opacity; })
    .attach();
```

The envelope shows up on the trace's `TraceStarted` event and in the JSONL
fixture. When you read a fixture and the burst looks wrong, the provenance
tells you which file / Figma node / animator the trace was attached to —
without grepping.

### Adapter shortcuts (each animation surface stamps itself)

Direct `with_provenance(...)` is the bedrock; Phase 9 added per-surface
shortcuts so common cases don't require hand-building an envelope:

- **Tween** — `t.set_motion_provenance("tween", "knob-hover")`, then call
  `t.publish(view, metric)` each tick. The macro
  `PULP_MOTION_TWEEN("knob-hover", from, to, duration)` auto-fills
  `source_file` / `source_line` from `std::source_location::current()`.
- **AnimatorSetBuilder** — `.name("knob-glow")` on the builder; the resulting
  `Runner::publish(view, metric, value)` stamps `source_kind="animator-set"`,
  `source_id="knob-glow"`.
- **CSS TransitionSpec** — `parse_transition_shorthand_with_provenance(css,
  "/styles/card.css", line)` carries `source_file` / `source_line` through;
  `CssAnimation::publish(view, metric)` stamps
  `source_kind="css-transition"`, `source_id=<property name>`.
- **JS rAF** — `WidgetBridge::load_script(code, "my-script.js")` (or
  `set_active_script_id(...)`) records the script id; `__flushFrames__`
  sets the ambient envelope per callback so a `motion.publishValue` from
  inside an rAF body emits `source_kind="rAF"`,
  `source_id="my-script.js:<callback_id>"`.
- **JS user code** — `motion.publishValue(view, metric, value)` and
  `motion.setProvenance(kind, id, file?, line?)` are exposed on the
  `globalThis.motion` object the bridge installs. `motion.clearProvenance()`
  empties the slot. Explicit `PublishOptions::provenance` always wins over
  the ambient slot.
- **Design import** — `generate_pulp_js` emits a `motion.setProvenance(...)`
  line at the top of every bundle, tagged with vendor + root-node id
  (`figma:Card/Hover`, `stitch:Panel`, `claude:HeaderLayout`, …). Drop the
  generated JS into a bridge and any animation it drives inherits the
  envelope automatically.

## Reduced-motion policy

`pulp::view::MotionPreferences` (sibling of `AppearanceTracker`) reads the
OS reduced-motion accessibility setting on first use and exposes it as a
`MotionPolicy` (`Full` / `Reduced` / `Off`) plus a clamped `duration_scale`
(0.0–2.0, default 1.0). Animation primitives honor the policy on start:

| Policy | Tween / ValueAnimation | CssAnimation | AnimatorSet |
|---|---|---|---|
| Full | Animate as configured | Animate as configured | Animate as configured |
| Reduced | Scale duration × `duration_scale` | Same | Same (via each Tween) |
| Off | Jump to target on tick 0 | Complete on first `tick()` | Each Tween starts finished |

Tests get a per-process override that wins over the OS value:

```cpp
auto& prefs = pulp::view::MotionPreferences::instance();
prefs.set_override(pulp::view::MotionPolicy::Reduced);
prefs.set_duration_scale(0.5);
// …drive the animation…
prefs.reset_for_tests();   // clears override, re-reads OS
```

Fixtures recorded under a non-`Full` policy capture it on the v2 header:

```jsonc
{"motion_fixture_version":2,"policy":"reduced","duration_scale":0.5}
```

`load_fixture_header(path)` returns the policy + scale; the header-aware
`assert_matches(g_hdr, g_events, c_hdr, c_events, opts)` overload flags a
`"policy-mismatch"` diff item if the goldens were recorded under one policy
and the capture under another — no more silent comparisons of a Reduced
golden against a Full capture. `policy` / `duration_scale` are additive on
the header; v2 fixtures without them still load (defaulting to
`policy="full"`, `duration_scale=1.0`).

Note: `MotionPreferences` is a sibling of `AppearanceTracker`, not a
subclass. Use the OS reader on the platform that matters (macOS: NSWorkspace
`accessibilityDisplayShouldReduceMotion`; Windows: `SPI_GETCLIENTAREAANIMATION`)
or set an override for deterministic tests.

## Gotchas

- Visual-analysis Python deps (numpy, Pillow, scikit-image) are intentionally
  not bundled in plugin/app artifacts. The CTest entry exits 3 (Skipped) when
  they are missing — that's expected on bare CI runners.
- Fixture schema is at version 2. The loader rejects unknown versions; v1
  fixtures are rejected outright (no v1 producer ships in the framework). Bumping
  the schema is a deliberate break — write a migration tool, don't silently
  accept old goldens.
- `TraceStarted` is emitted once per trace registration on the first tick
  after attach. Tests that count events should either filter it out or pass
  through `data_event_count(buffer)`.
- Window / Screen geometry spaces currently collapse onto ViewGlobal; window-
  origin and screen-origin offsets land when the host surfaces them. Use
  `ViewGlobal` for portable code.
- ScrollView's `paint_all` override does not apply base View transforms when
  painting children. The presentation walker matches this quirk: a child of a
  ScrollView with `scale` set on the ScrollView itself reports as if the scale
  were not there. This is correct relative to what is painted.
- Fixture loaders reject unknown `motion_fixture_version` values. Bumping the
  schema is a deliberate break — write a migration tool, do not silently
  accept old goldens.
- Per-(view, metric) `epsilon` is sticky on the first publish; subsequent
  publishes inherit it. Pass `PublishOptions` only when you want to configure
  the threshold for that key.
- `MotionPolicy` is captured at animation start (constructor / `reset()` /
  `animate_to()` / first `tick()` for CssAnimation). Changing
  `MotionPreferences::set_override()` partway through a running animation
  does NOT retroactively re-scale it — the next animation that starts will
  pick up the new policy.
- Under `MotionPolicy::Off`, a Tween-driven "publish until finished" loop
  emits one final-value Sample and exits. That's the contract — Off is not
  "no observability", it's "single snap" — assertions like `is_monotonic`
  and `final_value` still work.
- Test fixtures use `MotionPreferences::instance().reset_for_tests()` to
  clear overrides between cases. Forgetting it leaks state into the next
  test (and surprises CI when run with `--shuffle`).
