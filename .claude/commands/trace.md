---
name: trace
description: Profile a Pulp plugin/app with Perfetto, query a flushed .pftrace offline, and investigate why it is slow from trace evidence
---

Answer "why is this slow?" from Perfetto evidence. Offline `.pftrace` analysis
is usable today. Live lifecycle control is broker-authorized and fail-closed.

Tracing is a dev-only tool. Never ship a plugin with `PULP_TRACING` enabled.

## Four paths (pick by what you have)

| You have | Path | Tool |
|---|---|---|
| Authorized control session + want a DSP flamegraph | **Experimental live trace** | Start/stop through canonical control |
| A `.pftrace` + a question | **Query** | `pulp trace query "<sql>" --trace FILE.pftrace` |
| Want to hand an agent / human the raw file | **Return the path** | `pulp trace stop` prints it; open in ui.perfetto.dev |
| A UI hitch to correlate | **Frame trace + motion join** | trace `render,layout` while a motion trace runs; query on shared `trace_id` |

## Experimental live path

```bash
# The broker must authorize a trusted control session.

# 2. Start a session, reproduce the slow thing, then stop. Use the same exact
# broker-owned instance ID for both lifecycle calls when selecting explicitly.
pulp trace start --instance INSTANCE_ID --categories dsp,render
# ... trigger the suspect interaction / open the editor ...
pulp trace stop --instance INSTANCE_ID
# → /tmp/pulp-<ts>.pftrace

# 3. Query the flushed file. For a narrated answer, load trace-analysis and
# use its trace-sql queries over this same file.
pulp trace query "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20" \
  --trace /tmp/pulp-trace.pftrace

```

## Control methods

| `pulp trace <verb>` | Inspector method |
|---|---|
| `start [--instance ID]` | canonical `dev.pulp.trace/session-control@1` |
| `stop [--instance ID]` | canonical `dev.pulp.trace/session-control@1` |

`start` and `stop` use the canonical capability-control client. The broker owns
trusted target selection, consent, grants, and receipts. `--instance ID`
selects one exact broker-owned live instance; omitting it preserves fail-closed
unambiguous selection. Legacy `--session` / `--publication` and raw `--port`
selectors are rejected.
`--json` emits the canonical response. With no authorized control session the
operation fails closed and never falls back to Inspector discovery.

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
