# Tracing Guide

Pulp tracing is an off-by-default developer tool that records a **timeline of
where CPU/GPU time went** during a run — a Perfetto `.pftrace` you can open in
[ui.perfetto.dev](https://ui.perfetto.dev) or query with SQL. It answers *"why
is this slow?"*

> **Motion vs tracing, in one line.** Motion tells you **what changed** on
> screen (state timeline, JSONL, fixtures — the `motion` skill). Tracing tells
> you **where the time went** (span timeline, `.pftrace`, SQL). Reach for
> tracing when the question is "why is this slow"; reach for motion when it is
> "what moved." The two compose: a motion `trace_id` rides frame spans as an
> argument, so a slow frame in a motion capture links straight to its
> flamegraph.

Tracing is a **dev tool, never a shipping feature.** `PULP_TRACING` is OFF by
default; a default build links zero Perfetto symbols. See the gotchas box
before you touch it.

---

## The tiered experience (L0 / L1 / L2)

You do not need to know SQL, or even have an agent, to get an answer. There are
three tiers, and they all work identically under Claude Code, Codex, or any
MCP-speaking agent — because the whole experience is shipped `SKILL.md` skills
plus a CLI/MCP surface, not a vendor API.

| Tier | Who | Entry point | What you get |
|---|---|---|---|
| **L0** | Novice, **no agent** | `pulp trace slowest-frames`, `pulp trace xruns`, `pulp trace dsp-hotspots`, `pulp trace layout-vs-paint`, `pulp trace query --preset <name>` | A deterministic canned query over Pulp's categories → a plain table. Zero SQL, zero agent. |
| **L1** | Novice, **one-shot** | `pulp trace explain "<question>"` · MCP `pulp_trace_explain` · `/trace "<question>"` | **The headline.** An agent loads the `trace-analysis` skill, runs the full investigation autonomously, and returns a plain-English **root cause + chain of evidence + a concrete fix** — never raw SQL. |
| **L2** | Expert, **iterative** | `pulp trace query "<sql>"` with the `trace-analysis` + `trace-sql` skills loaded | The full hypothesis→query→drill-down loop for hard, multi-bottleneck cases. |

The L0 preset names map **1:1** onto the trace-stdlib SQL views shipped with the
`trace-sql` skill (`slowest-frames → pulp_slowest_frames`, `xruns →
pulp_xruns`, `dsp-hotspots → pulp_dsp_node_cost`, `layout-vs-paint →
pulp_layout_vs_paint`) — one definition, three surfaces.

### The L1 "explain" flow, spelled out

```
1. Capture   → pulp trace start --categories dsp   (or accept a given --trace FILE.pftrace)
             → reproduce → pulp trace stop  → /tmp/pulp-<ts>.pftrace
2. Ask       → pulp trace explain "why is my plugin using so much CPU?"
3. Agent     → loads the trace-analysis skill and runs its protocol autonomously:
               forms a hypothesis, queries via the trace-sql stdlib, checks
               wall-time vs CPU-time, follows blockers across threads, verifies
               exhaustively (not just the first bottleneck), builds a chain of
               evidence.
4. Answer    → plain English. No SQL surfaced.
```

---

## The two skills

Two skills under `.agents/skills/` (read by Claude Code **and** Codex) power the
agent tiers:

- **`trace-analysis`** — the investigation harness: the chain-of-evidence loop,
  the wall-time-vs-CPU-time rule, follow-the-blocker, exhaustive verification,
  and Pulp-specific domain hints (`references/hints_dsp.md`, `hints_frame.md`,
  `hints_js.md`, `hints_gpu.md`, `hints_crossplatform.md`).
- **`trace-sql`** — the SQL discipline (idempotent views, `GLOB` not `LIKE`,
  incomplete-slice handling, stable-key joins) plus the **Pulp trace-stdlib** of
  named query primitives.

The investigation methodology is adapted from Google's `android/skills`
(Apache-2.0, attributed in `NOTICE.md`); the Pulp domain content is authored
against Pulp's own seams.

---

## Category taxonomy

Every span is tagged with one fixed category — this is the query vocabulary:

| Category | Covers |
|---|---|
| `dsp` | per-block audio processing (offline path) |
| `dsp.node` | per-node cost in the signal graph (offline path) |
| `render` | the per-frame wrapper span + present bookkeeping |
| `layout` | Yoga layout pass |
| `canvas` | 2D drawing / dirty-walk |
| `text` | `TextShaper::prepare` / `layout` (the PreText cheap-vs-expensive split) |
| `js` | QuickJS bridge dispatch |
| `gpu` | Dawn submit / Graphite record / per-pass GPU time |
| `state` | parameter / state-store activity |
| `io` | file / resource I/O |

> DSP categories come from the **offline** render path only. Perfetto is never
> placed on the live audio thread — its `TRACE_EVENT` locks a mutex on chunk
> rollover, which is not real-time-safe. Live DSP observability is Pulp's own
> fixed-slot per-node telemetry, not tracing.

---

## Gotchas

> **Ring-buffer overflow → a silently empty or truncated trace.** The trace is a
> fixed-size in-memory ring. If it overflows during capture, older spans are
> dropped and the `.pftrace` comes back empty or partial — with no error. If a
> query returns nothing, suspect the capture before concluding the code was
> fast: `SELECT DISTINCT category FROM slice`. Fix by raising `--ring-mb` or
> shortening the capture window.
>
> **Traces flush only on graceful teardown or `stop`.** The in-memory ring is
> written to disk on `pulp trace stop`, on `StandaloneApp` shutdown, or on a
> plugin format adapter's destroy path (VST3 `terminate` / AU dealloc / CLAP
> `destroy`). **A host crash loses the ring** — inherent to the in-process
> backend. An empty trace after a crash is expected, not a bug.
>
> **Never ship with `PULP_TRACING` on.** It is enforced: a Catch2 guard fails if
> a default configure defines `PULP_TRACING`, and the `pulp ship` symbol check
> refuses to package a binary containing Perfetto symbols without an explicit
> `--allow-tracing`. An 80 MB ring and a `.pftrace` written inside a customer's
> DAW is a support incident.
>
> **Trace files are dev artifacts.** They contain parameter names, plugin names,
> and file paths. They are local-only and never auto-uploaded (consistent with
> Pulp's analytics posture).

---

## Worked use cases

Only reproducible examples ship here — a demo that cannot be reproduced would
embarrass us. All three below are deterministic (bounded startup, or an offline
render). Live in-DAW real-time-dropout profiling is an **advanced/expert** path
with real caveats (non-deterministic, gated on RT-safety, and the profiler can
drop spans under the exact load you are chasing) — it is not a one-line novice
promise.

### Use case 1 — "Why is my plugin slow to open?" (the flagship)

```bash
pulp trace start --categories render,gpu,text,js,layout
# ... open the plugin editor (or launch the standalone app) ...
pulp trace stop                                        # → /tmp/pulp-<ts>.pftrace
pulp trace explain "why is my plugin slow to open?"
```

One-shot, main-thread startup laid out on a timeline: Dawn/Graphite device
init, Skia font-atlas build, QuickJS script eval, first layout +
`TextShaper::prepare`. The agent names the biggest one-time cost and whether it
is re-paid on every open. Bounded, deterministic, no real-time hazard — the
example we are most confident shipping.

A representative L1 answer:

> **Root cause:** first editor-open spends ~2.4 s; ~1.9 s of it is one-time
> GPU/text setup on the UI thread before the first frame, none of it cached
> between opens.
> **Breakdown:** Dawn + Graphite init ~620 ms, Skia font-atlas build ~540 ms,
> QuickJS script eval ~410 ms, first Yoga layout + `TextShaper::prepare` ~330 ms.
> **Evidence:** (1) a `gpu`/`render` pair brackets the ~620 ms init on the main
> thread; (2) `text` spans show the ~540 ms atlas build before per-label
> prepare; (3) the `js` eval span is ~410 ms, single-shot; (4) a second open
> repeats every span identically — nothing is reused.
> **Fix:** warm the Dawn/Graphite context and font atlas once per process (not
> per open) and cache the compiled JS module. Re-opens should drop below 500 ms.

### Use case 2 — "Find the slowest frames / why does the UI stutter when I move a knob?"

```bash
pulp trace slowest-frames                              # L0 preset: frames over budget, worst first
# then, to explain a hitch during an interaction:
pulp trace start --categories render,layout,canvas,text,js,gpu
pulp motion record --view Knob --out knob.jsonl        # motion trace_id joins in
# ... sweep the knob ...
pulp trace stop
pulp trace query --preset layout-vs-paint              # or: explain "why did those frames blow the budget?"
```

The fat slices are `TextShaper::prepare` firing every frame — the knob's value
label re-shapes text on each update instead of reusing cached widths
(`text_shaper.hpp` is built to avoid exactly this). The motion `trace_id` on the
frame spans ties the hitch to the sweep gesture; `layout-vs-paint` gives the
one-row-per-stage cost split.

### Use case 3 — "Why is my plugin using so much CPU?" (run offline for a reproducible answer)

```bash
# Deterministic: render a fixed clip through the plugin OFFLINE (the
# offline_process() path examples/trace-demo uses) — no live DAW, no dropouts,
# reproduces exactly.
pulp trace start --categories dsp,dsp.node
# ... offline-render a fixed MIDI/audio clip through the plugin ...
pulp trace stop
pulp trace explain "why is my plugin using so much CPU?"
```

`AudioProcessLoadMeasurer` reports a calm ~40% average, but the flamegraph shows
one `dsp.node` (a per-voice oversampler) eating ~60% of every block — the mean
hid the per-node max. Because the render is offline and deterministic, the
answer reproduces exactly, so it is safe for docs. *(Live in-DAW DSP profiling
is supported as an advanced path with the caveats above.)*

---

## See also

- The `trace-analysis` and `trace-sql` skills under `.agents/skills/`.
- The `motion` skill and `docs/guides/motion-observability.md` — the
  what-changed complement to this where-time-went view.
