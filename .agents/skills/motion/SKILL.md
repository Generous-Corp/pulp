---
name: motion
description: Agent-first motion observability for Pulp views and animations. Single entry point for two paths — runtime scalar/geometry tracing (samples values + view geometry over time, attachable at runtime over the inspector wire, emits Start/End burst-framed events) and pixel-truth visual analysis (SSIM + diff heatmaps + keyframe sprites from frame sequences). Use when an animation timing / direction / easing / scroll / drift is suspect, when an imported design needs verification, or when a transition / GPU effect has no observable scalar.
---

# Motion

Diagnose animation, scroll, and transition behavior with the framework's
agent-first motion observability system. Two paths share one skill — pick
based on what's observable.

## Quick decision

| You have | Path | Tool |
|---|---|---|
| A running app + a node id + a scalar / geometry of interest | **Runtime trace** | `Motion.startTrace` over the inspector wire |
| A captured frame sequence (no app instrumentation available) | **Visual analysis** | `tools/motion/visual/analyze_sequence.py` |
| A previously recorded `.motion.jsonl` fixture | **Replay + assert** | `motion::replay_fixture` + `motion::assert_matches` |
| Imported design + intent doc (e.g. "fade in 350 ms ease-out") | **Both** | Record a fixture from the import, assert timing/monotonicity |

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
```

### 3. Read the report

The pipeline writes `analysis.json` (machine-readable), `summary.md`
(agent-readable), `diff/diff_NNNN_NNNN.png` (pairwise heatmaps), and
`keyframes.png` (sprite). The JSON carries `schema_version` — refuse unknown
versions.

Pair conclusions with evidence: cite pair index, SSIM, mean diff, and which
artifacts you read. Confidence below 0.7 means escalate (more pairs, longer
window, or fall back to a runtime trace if instrumentation is possible).

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
- `inspect/include/pulp/inspect/motion_inspector.hpp` — Motion inspector bridge
- `inspect/src/motion_inspector.cpp` — protocol handler + event broadcaster
- `tools/motion/visual/analyze_sequence.py` — visual analysis CLI
- `tools/motion/visual/test_self_check.py` — pipeline self-check
- `examples/ui-preview/main.cpp` — env-knob wiring for the standalone host
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
