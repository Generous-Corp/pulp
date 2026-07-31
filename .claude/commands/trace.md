---
name: trace
description: Profile a Pulp plugin/app with Perfetto, query a flushed .pftrace offline, and investigate why it is slow from trace evidence
---

Answer "why is this slow?" from Perfetto evidence. Offline `.pftrace` analysis
is usable today. The live inspector path is reserved but not wired into normal
Pulp launches; `PULP_TRACE_SERVER` is not implemented.

Tracing is a dev-only tool. Never ship a plugin with `PULP_TRACING` enabled.

## Four paths (pick by what you have)

| You have | Path | Tool |
|---|---|---|
| Custom source-checkout fixture + want a DSP flamegraph | **Experimental live trace** | Explicitly construct the inspector server, then start/stop |
| A `.pftrace` + a question | **Query** | `pulp trace query "<sql>" --trace FILE.pftrace` |
| Want to hand an agent / human the raw file | **Return the path** | `pulp trace stop` prints it; open in ui.perfetto.dev |
| A UI hitch to correlate | **Frame trace + motion join** | trace `render,layout` while a motion trace runs; query on shared `trace_id` |

## Experimental live path — custom fixture only

```bash
# Normal standalone and preview hosts do not start this endpoint.
# A custom source-checkout fixture must construct and wire it first.

# 2. Start a session, reproduce the slow thing, then stop.
pulp trace start --categories dsp,render
# ... trigger the suspect interaction / open the editor ...
pulp trace stop --session SESSION --instance INSTANCE --publication PUBLICATION
# → /tmp/pulp-<ts>.pftrace

# 3. Query the flushed file. For a narrated answer, load trace-analysis and
# use its trace-sql queries over this same file.
pulp trace query "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20" \
  --trace /tmp/pulp-trace.pftrace

# 4. Other handy verbs:
pulp trace snapshot           # tracing_active / categories / ring_bytes / out_path
```

## Inspector methods (each verb forwards to one)

| `pulp trace <verb>` | Inspector method |
|---|---|
| `start` | `Trace.startSession` |
| `stop` | `Trace.stopSession` |
| live `query` / presets | `Trace.query` (reserved; returns `capability_unavailable`) |
| `snapshot` | `Trace.snapshot` |
| `explain` | `Trace.explain` (reserved; returns `capability_unavailable`) |

Every live verb uses authenticated ephemeral discovery by default and honors
`--session ID --instance ID --publication ID` as an exact-publication selector,
`--port N` / `$PULP_INSPECTOR_PORT` as an explicit port filter, plus
`--json` for the raw inspector response. Use the same exact selector for
`start` and `stop` so a replacement process cannot inherit the operation. If
no session is discoverable, the CLI
prints a legacy no-inspector hint; that environment variable does not currently
activate a server.

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
