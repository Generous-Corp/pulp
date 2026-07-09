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

<!-- Self-contained subsection; keep edits here local to minimize merge churn. -->
## Live DSP telemetry (fixed-slot, the RT-safe path)

Perfetto tracing is never placed on the live audio thread (its `TRACE_EVENT`
locks a mutex on chunk rollover). For **live** per-node DSP cost you want the
fixed-slot telemetry surface instead — a lock-free, allocation-free ring the
audio thread writes and an owner thread drains.

It is **disabled by default**; when off the audio path is a single
predicted-not-taken branch. Enable it on a `SignalGraph` and poll a snapshot
from the UI/owner thread:

```cpp
graph.set_live_dsp_telemetry_enabled(true);          // arm; no recompile needed
// ... the audio thread renders blocks ...
auto snap = graph.poll_live_dsp_telemetry();          // drain + read latest
for (const auto& node : snap.nodes) {
    // node.kind / node.name, node.p50_elapsed_ns, node.p95_elapsed_ns,
    // node.p99_elapsed_ns, node.jitter_ns (p95 - p50),
    // node.over_budget_attributions
}
```

Each block times every node into its persistent load measurer regardless of
whether the routed serial executor or the reference walk ran, so the numbers
are path-agnostic. `drain()` computes a rolling p50/p95/p99 per node, the
graph-level load, and attributes each over-budget block to its most expensive
node. The store lives on the compiled graph and resets when the topology
recompiles.

For a headless dump (agents, CI — no window), serialize the snapshot to a flat
JSON object:

```cpp
#include <pulp/audio/live_dsp_telemetry_json.hpp>
const std::string json = pulp::audio::live_dsp_telemetry_snapshot_to_json(snap);
```

The object carries the graph-level counters (`blocks_written` /
`blocks_drained` / `blocks_dropped`, `graph_over_budget_blocks`, last-block
context) plus a `nodes` array with each node's `kind` name, `sample_count`,
min/max/mean, `p50`/`p95`/`p99`, `jitter_ns`, and `over_budget_attributions`.

**RT-safety guarantee.** The write path (`begin_block` → per-node scopes →
`finish`) never allocates, locks, or makes a syscall. Two layers verify this:
`RtAllocationProbe` + `ScopedNoAlloc` assert no allocation across a burst of
blocks, and the advisory RealtimeSanitizer lane exercises the same path under
`-fsanitize=realtime` (via a `[[clang::nonblocking]]`-attributed wrapper) so a
later lock/syscall regression fails loudly.

---

<!-- Self-contained subsection; keep edits here local to minimize merge churn. -->
## Span names: static (default) vs dynamic

Span names are **static by default**. `PULP_TRACE_SCOPE(category)` derives the
name from the enclosing function at compile time, and
`PULP_TRACE_SCOPE_NAMED(category, "literal")` takes a string literal — both
intern the name once, so `GROUP BY name` presets stay cheap and low-cardinality.

When you need the name itself to carry a **runtime** value (a parameter name, a
graph node id, a file basename), use the per-call-site opt-in:

```cpp
PULP_TRACE_SCOPE_DYNAMIC("dsp.node", node.display_name());  // std::string / const char*
```

This is the only sanctioned way to get a dynamic span name. It wraps the value
in `perfetto::DynamicString` so Perfetto copies the bytes on every event.
Prefer a static name unless the runtime value is the thing you need to query on:
a dynamic name allocates/copies per event and multiplies `slice.name`
cardinality, which degrades the `GROUP BY name` presets. Like every macro here,
it compiles to nothing when `PULP_TRACING` is off.

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

### Use case 4 — "Which plugin in my chain is expensive?" (real plugins, and why the average lies)

`examples/trace-plugin-chain` is the runnable version of use case 3 on **real
example plugins**: it drives PulpGain → PulpEffect (a biquad filter) →
PulpCompressor through their actual `Processor::process()` code offline (via
`HeadlessHost`), one `dsp.node` span per plugin per block.

```bash
# Build the dev-only tracing examples and render the chain to a .pftrace.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_TRACING=ON
cmake --build build --target pulp-trace-plugin-chain -j"$(getconf _NPROCESSORS_ONLN)"
./build/examples/trace-plugin-chain/pulp-trace-plugin-chain 0.06 /tmp/chain.pftrace

# Attribute cost per plugin — the average and the steady state disagree:
pulp trace query \
  "select name as plugin, round(avg(dur)/1000.0,2) as avg_all_blocks, \
   round((sum(dur)-max(dur))/1000.0/(count(*)-1),2) as avg_steady_state \
   from slice where name in ('gain','biquad_filter','compressor') \
   group by name order by avg_all_blocks desc" \
  --trace /tmp/chain.pftrace
```

The trace catches a truth a scalar load meter inverts: `gain`'s *average* looks
like the worst offender, but nearly all of it is a one-time cold-start spike on
the first block (the first `process()` call warms caches and touches fresh
pages). In steady state the biquad filter is the real per-block hot node and the
gain is effectively free. The per-block trace separates one-time warmup from the
cost that actually recurs — the whole reason to reach for a trace over an
average.

---

## Keeping Perfetto current

The Perfetto SDK is a **dev-only, opt-in dependency** — fetched by pinned URL +
SHA-256 in `tools/cmake/PulpTracing.cmake` only when `PULP_TRACING=ON`, never
linked into a shipping build, never exported by `cmake --install`. You do not
need to do anything for a normal build; this section is for updating the pin.

**Moving to a newer Perfetto release.** Both the version tag and its hash are
CMake cache variables, so you override them from the command line without
editing the repo — pass a matched pair:

```bash
cmake -S . -B build -DPULP_TRACING=ON \
      -DPULP_PERFETTO_VERSION=v58.0 \
      -DPULP_PERFETTO_SHA256=<sha256 of that release's perfetto-cpp-sdk-src.zip>
```

The SHA is required alongside the version: bumping the version alone would keep
the old hash and `FetchContent` would fail the integrity check (by design — a
version/hash mismatch should never silently fetch unverified bytes). To build
against an already-unpacked local checkout instead of downloading, use CMake's
native escape hatch, no Pulp-specific flag needed:
`-DFETCHCONTENT_SOURCE_DIR_PERFETTO=/path/to/perfetto-sdk`.

**When Google ships a new Perfetto, does anything in Pulp break?** No — the pin
is exact, so an upstream release changes nothing until someone bumps the pin.
Perfetto is tracked in `tools/deps/manifest.json`, so
`python3 tools/deps/audit.py --check-upstream` flags the drift on the same
cadence as every other pinned dependency. Landing a new pin is the normal
dependency-update workflow: bump `PulpTracing.cmake` + `manifest.json`, and keep
`DEPENDENCIES.md` and `NOTICE.md` in sync (Perfetto is Apache-2.0; the
`melatonin_perfetto` prettifier port is MIT — both already carry their notices).

A developer who overrides the pin locally (the `-D` recipe above) is never
blocked by Pulp being behind: the override is theirs and works immediately,
whether or not the committed pin has caught up.

**The query tool is separate from the capture SDK.** Capture (above) compiles
the Perfetto SDK into a `PULP_TRACING=ON` build. *Querying* a `.pftrace` uses
Perfetto's prebuilt `trace_processor_shell` binary, which Pulp downloads and
SHA-256-verifies on first use — no `brew`/`apt`, nothing to install by hand. It
is a first-class entry in Pulp's cargo-like tool registry, so both of these
install the identical pinned artifact into `~/.pulp/tools/trace-processor/`:

```bash
pulp trace fetch                    # pre-fetch before an offline/air-gapped session
pulp tool install trace-processor   # same artifact, via the general tool surface
pulp tool info trace-processor      # version, install state, resolved path
pulp tool doctor trace-processor    # health check
```

The per-platform pins (version + SHA-256) live in
`experimental/pulp-rs/src/cmd/trace_fetch.rs`; the registry entry in
`tools/packages/tool-registry.json` mirrors the version + download URLs so
`pulp tool` can describe the tool without duplicating the hashes. A drift test
(`experimental/pulp-rs/tests/trace_processor_tool_test.rs`) fails if the two ever
disagree, so a Perfetto bump can't leave the tool surface advertising a stale
URL. `pulp trace query` auto-fetches on demand, so an explicit install is only
needed to pre-warm a machine that will later be offline.

## Never ship a traced build — and how the guard works

`pulp ship sign` and `pulp ship package` **refuse** an artifact built with
`PULP_TRACING=ON`. The runtime emits a retained sentinel byte-string into any ON
build; `pulp ship` scans the candidate bundle/binary for it and stops with a
clear error naming the offending file. `pulp ship release` inherits the guard
through its `package` step. Override deliberately with `--allow-tracing` (it
prints a loud warning and proceeds). This is the ship-time backstop to the
build-time guarantee that a default configure never defines `PULP_TRACING`.

---

## See also

- The `trace-analysis` and `trace-sql` skills under `.agents/skills/`.
- The `motion` skill and `docs/guides/motion-observability.md` — the
  what-changed complement to this where-time-went view.
