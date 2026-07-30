---
name: motion
description: Debug or validate a Pulp animation / transition / scroll — runtime trace, fixture record/replay/assert, visual analysis, scrubber, cost attribution
---

Debug a motion behavior using fixture/replay or visual evidence. The live
inspector path is reserved but not wired into normal Pulp launches;
`PULP_MOTION_SERVER` is not implemented.

## Eight paths (pick by what you have)

| You have | Path | Tool |
|---|---|---|
| Custom source-checkout fixture + node id | **Experimental runtime trace** | Explicitly construct `InspectorServer`, then use `Motion.startTrace` |
| Just frames (no instrumentation) | **Visual analysis** | `python3 -m tools.motion.visual.analyze_sequence --frames-dir DIR` |
| A `.motion.jsonl` fixture | **Replay + assert** | `motion::replay_fixture` + `motion::assert_matches` |
| An interaction to record | **Input record/replay** | `motion::make_input_recorder` + `motion::replay_inputs` |
| A fixture to scrub | **Timeline scrubber** | `Motion.loadFixture` + `Motion.scrubTo` |
| "Which animation is expensive?" | **Cost attribution** | `Motion.enableCost` + `CostAttributor` |
| SwiftUI / UIKit / AppKit code path | **Swift facade** (Path G) | `View.pulpMotionTrace { Trace.* }` / `PulpMotionGeometryProbe` |
| Compose / Android View code path | **Kotlin facade** (Path H) | `Modifier.pulpMotionGeometry { +Trace.* }` / `View.pulpMotionTrace` |

## Experimental live path — custom fixture only

```bash
# Normal standalone and preview hosts do not start an inspector endpoint.
# A custom source-checkout fixture must construct and wire it first.

# 2a. Easiest: `pulp motion *` CLI subcommands (one verb per Motion.*
#     method, prints the trace_id, honors --port + $PULP_INSPECTOR_PORT).
pulp motion record --view Card --out card-fade.jsonl
# → trace started — trace_id=1
pulp motion stop --trace-id 1

# The transport is length-prefixed; raw line-delimited `nc` is not a client.

# 4. Other handy verbs:
pulp motion snapshot          # view tracing_enabled / active_traces / cost
pulp motion list-traces       # enumerate inspector-owned trace IDs
pulp motion load-fixture FOO.motion.jsonl && pulp motion scrub 30
pulp motion cost enable       # opt in to per-frame cost samples
```

## Useful env knobs in `pulp-ui-preview`

- `PULP_MOTION_LOG=1` — install the default `[PulpMotion]` log sink + enable tracing.
- `PULP_MOTION_SERVER=1` — reserved by older docs; currently does not start a server.
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
