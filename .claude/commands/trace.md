---
name: trace
description: Profile a Pulp plugin/app with Perfetto — start/stop a tracing session, run SQL over the .pftrace, run L0 presets, or ask "why is this slow?" for a one-shot narrated root cause
---

Answer "why is this slow?" the framework's way: attach a Perfetto tracing
session over the inspector wire, reproduce, and read where the time went —
don't guess from source. Motion tells you *what changed* on screen; tracing
tells you *where the time went* (span timeline, `.pftrace`, SQL).

Tracing is a dev-only tool. Never ship a plugin with `PULP_TRACING` enabled.

## Four paths (pick by what you have)

| You have | Path | Tool |
|---|---|---|
| Running app + want a DSP flamegraph | **Live trace** | `pulp trace start --categories dsp` → reproduce → `pulp trace stop` |
| A `.pftrace` + a question | **Query** | `pulp trace query "<sql>"` (or a `--preset`) |
| Want to hand an agent / human the raw file | **Return the path** | `pulp trace stop` prints it; open in ui.perfetto.dev |
| A UI hitch to correlate | **Frame trace + motion join** | trace `render,layout` while a motion trace runs; query on shared `trace_id` |

## Fastest path — quick trace via the standalone host

```bash
# 1. Launch the host with the tracing inspector server up.
PULP_TRACE_SERVER=1 ./build/examples/ui-preview/pulp-ui-preview &

# 2. Start a session, reproduce the slow thing, then stop.
pulp trace start --categories dsp,render
# ... trigger the suspect interaction / open the editor ...
pulp trace stop
# → /tmp/pulp-<ts>.pftrace

# 3a. Novice one-liner (L1): narrated root cause + fix, no SQL.
pulp trace explain "why is my plugin slow to open?"

# 3b. L0 preset tables (deterministic, no agent):
pulp trace slowest-frames
pulp trace xruns
pulp trace dsp-hotspots
pulp trace layout-vs-paint

# 3c. L2 raw SQL (JSON by default; --format table for humans):
pulp trace query "SELECT name, dur FROM slice WHERE category='dsp' ORDER BY dur DESC LIMIT 20"

# 4. Other handy verbs:
pulp trace snapshot           # tracing_active / categories / ring_bytes / out_path
pulp trace query --preset dsp-hotspots
```

## Inspector methods (each verb forwards to one)

| `pulp trace <verb>` | Inspector method |
|---|---|
| `start` | `Trace.startSession` |
| `stop` | `Trace.stopSession` |
| `query` / presets | `Trace.query` |
| `snapshot` | `Trace.snapshot` |
| `explain` | `Trace.explain` |

Every verb honors `--port N` / `$PULP_INSPECTOR_PORT` (default 9147) and
`--json` for the raw inspector response. If nothing is listening, the CLI
prints a clear "no inspector — launch with `PULP_TRACE_SERVER=1`" hint.

## Category taxonomy (the query vocabulary)

`dsp`, `dsp.node`, `render`, `layout`, `canvas`, `text`, `js`, `gpu`,
`state`, `io`.

## Gotchas

- Ring-buffer overflow → a **silently empty / truncated** trace. Raise
  `--ring-mb` or shorten the capture window.
- Traces only flush on graceful teardown or `pulp trace stop` — a host crash
  loses the in-memory ring.
- Live in-DAW DSP profiling is an advanced, non-deterministic path; the
  reproducible DSP reveal runs offline. Startup and frame captures are the
  safe, deterministic examples.

See `docs/reference/cli.md#trace` for the full subcommand reference.
