---
name: motion
description: Debug or validate a Pulp animation / transition / scroll — runtime trace, fixture record/replay/assert, visual analysis, scrubber, cost attribution
---

Debug a motion behavior the framework's way: attach a runtime trace over
the inspector wire, capture a fixture, and read the numbers — don't
guess from source.

## Eight paths (pick by what you have)

| You have | Path | Tool |
|---|---|---|
| Running app + node id | **Runtime trace** | `Motion.startTrace` over the inspector wire (port 9147) |
| Just frames (no instrumentation) | **Visual analysis** | `python3 -m tools.motion.visual.analyze_sequence --frames-dir DIR` |
| A `.motion.jsonl` fixture | **Replay + assert** | `motion::replay_fixture` + `motion::assert_matches` |
| An interaction to record | **Input record/replay** | `motion::make_input_recorder` + `motion::replay_inputs` |
| A fixture to scrub | **Timeline scrubber** | `Motion.loadFixture` + `Motion.scrubTo` |
| "Which animation is expensive?" | **Cost attribution** | `Motion.enableCost` + `CostAttributor` |
| SwiftUI / UIKit / AppKit code path | **Swift facade** (Path G) | `View.pulpMotionTrace { Trace.* }` / `PulpMotionGeometryProbe` |
| Compose / Android View code path | **Kotlin facade** (Path H) | `Modifier.pulpMotionGeometry { +Trace.* }` / `View.pulpMotionTrace` |

## Fastest path — quick trace via the standalone host

```bash
# 1. Launch the host with the motion inspector server up.
PULP_MOTION_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview &

# 2. Attach a trace (TCP port 9147, line-delimited JSON).
echo '{"id":1,"method":"Motion.startTrace","params":{
  "view_name":"Card","fps":30,
  "metrics":[{"kind":"geometry","name":"frame","node_id":"card",
    "properties":["minX","minY","width","height"],
    "space":"window","source":"presentation"}]}}' \
  | nc -w 30 localhost 9147

# 3. Trigger the suspect interaction in the app; events stream as JSON.

# 4. Stop the trace when done.
echo '{"id":2,"method":"Motion.stopTrace","params":{"trace_id":1}}' \
  | nc -w 5 localhost 9147
```

## Useful env knobs in `pulp-ui-preview`

- `PULP_MOTION_LOG=1` — install the default `[PulpMotion]` log sink + enable tracing.
- `PULP_MOTION_SERVER=1` — start the `Motion.*` inspector server (port 9147).
- `PULP_MOTION_FIREHOSE=1` — broadcast every `publish_value` call to all sinks (dev-only).

## Assertion shape (use the dedicated helpers, never one global "smoothness")

```cpp
auto samples = motion::extract_scalar(events, "Card", "frame", "minY");
REQUIRE(motion::is_monotonic(samples));
REQUIRE(motion::settling_time_seconds(samples) < 0.7);
REQUIRE(motion::overshoot(samples) < 0.05);
REQUIRE(motion::final_value(samples) == Catch::Approx(120.0).margin(1.0));
```

See `docs/guides/motion-observability.md` for the full guide and
`.agents/skills/motion/SKILL.md` for the agent contract + per-path
playbooks.
