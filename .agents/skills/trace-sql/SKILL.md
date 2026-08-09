---
name: trace-sql
description: SQL discipline for querying Pulp Perfetto traces (.pftrace) with trace_processor — idempotent CREATE OR REPLACE PERFETTO views, GLOB not LIKE, dur = -1 incomplete-slice handling, EXTRACT_ARG for span args, joining on stable utid/upid, SPAN_JOIN PARTITIONED, and the draft→validate→execute loop. Ships the Pulp trace-stdlib (pulp_slowest_frames, pulp_dsp_node_cost, pulp_frames_over_budget, pulp_xruns, pulp_layout_vs_paint, pulp_motion_join). TRIGGER when writing or debugging SQL over a .pftrace, when `pulp trace query` returns wrong/empty rows, or when the trace-analysis skill needs a query primitive.
requires:
  - .agents/skills/trace-sql/pulp_slowest_frames.sql
  - .agents/skills/trace-sql/pulp_dsp_node_cost.sql
  - .agents/skills/trace-sql/pulp_frames_over_budget.sql
  - .agents/skills/trace-sql/pulp_xruns.sql
  - .agents/skills/trace-sql/pulp_layout_vs_paint.sql
  - .agents/skills/trace-sql/pulp_motion_join.sql
---

# trace-sql — querying Pulp traces with `trace_processor`

Pulp's tracing subsystem writes Perfetto `.pftrace` files; this skill is the
SQL discipline for turning one into an answer. It is the query substrate under
the `trace-analysis` harness and under `pulp trace query` / `pulp trace
<preset>`. **You are reading this because you are writing SQL over a `.pftrace`,
a query came back empty or wrong, or the investigation harness needs a named
primitive to query.**

This skill is provider-agnostic by construction: it is a plain `SKILL.md` read
identically by Claude Code and Codex from `.agents/skills/`. Nothing here
depends on a specific agent, MCP server, or vendor.

> **Attribution.** The SQL discipline below (the draft→validate→execute loop,
> GLOB-over-LIKE, incomplete-slice handling, stable-key joins, `SPAN_JOIN`
> guidance) is **adapted from Google's `android/skills` `perfetto-sql`
> (Apache-2.0)**. The methodology is reused; the Android domain content
> (SurfaceFlinger, binder, ftrace, cpu governor) is not — every query here is
> authored against Pulp's own category taxonomy. See NOTICE.md.

---

## The tool: `trace_processor`

Queries run through the `trace_processor` wrapper (a small Python launcher that
resolves the precompiled `trace_processor_shell` binary). SQL is strictly
offline: `pulp trace query` requires `--trace FILE.pftrace` and shells to
`trace_processor`; it never forwards SQL to a live inspector:

```bash
# Offline SQL against a saved capture (no live inspector needed):
pulp trace query "SELECT name, dur FROM slice ORDER BY dur DESC LIMIT 20" \
  --trace /tmp/pulp-<ts>.pftrace
```

Pulp resolves the binary via `$PULP_TRACE_PROCESSOR` → the pinned Pulp-fetched
build → `$PATH`; `pulp trace doctor` reports which. For zero-install, run
`pulp trace fetch` once: it downloads the pinned `trace_processor_shell`
(Perfetto v57.2, matching the tracing SDK), SHA-256-verified per platform, into
`$PULP_HOME` — no manual install, no surprise download inside `query`.
`trace_processor` is also a first-class entry in Pulp's tool registry, so
`pulp tool install trace-processor` (and `pulp tool info/doctor trace-processor`)
fetch the identical pinned artifact — same code path as `pulp trace fetch`. To
drive `trace_processor` directly:

```bash
# Interactive SQL over a capture:
./trace_processor --query-string \
  "SELECT name, dur FROM slice WHERE category='dsp.node' ORDER BY dur DESC LIMIT 20" \
  /tmp/pulp-<ts>.pftrace

# Run a file of view definitions, then query (trace as the trailing positional):
./trace_processor -q .agents/skills/trace-sql/pulp_dsp_node_cost.sql /tmp/x.pftrace
```

**Before using it, check it exists** (skill-versioning rule): confirm the
`trace_processor` wrapper is on `PATH` or in the project's tracing cache. If it
is absent, run the explicit `pulp trace fetch`; query does not silently download
or degrade to returning a trace path.

`pulp trace start|stop --instance ID` selects one exact broker-owned live
instance for lifecycle control. The selector is transported outside lifecycle
method parameters and is rejected by offline `query` and other non-lifecycle
verbs; omission preserves the canonical safe no-selector behavior.

**Output format.** Offline query emits `trace_processor`'s native table by
default; `--format table` is the only accepted explicit format. The global
`--json` flag wraps that table as an escaped `output` string for agents;
`--format json|csv` is rejected rather than mislabeling native output.

---

## The Pulp trace-stdlib (query named primitives, not re-derived SQL)

Six authored `CREATE OR REPLACE PERFETTO` definitions ship next to this skill.
Load the ones you need, then `SELECT` from them — do not re-derive the joins by
hand each time. Each `.sql` file carries a header comment explaining its shape.

| View / function | Answers | L0 preset |
|---|---|---|
| `pulp_slowest_frames` | frame spans, slowest first | `pulp trace slowest-frames` |
| `pulp_dsp_node_cost` | per-node DSP cost (count/total/mean/**max**) | `pulp trace dsp-hotspots` |
| `pulp_frames_over_budget` | frames past the vsync budget (fn takes a budget) | `--preset frames-over-budget` |
| `pulp_xruns` | xrun / deadline-miss instant events | `pulp trace xruns` |
| `pulp_layout_vs_paint` | frame-pipeline cost split, one row per stage | `pulp trace layout-vs-paint` |
| `pulp_motion_join` | frames joined to their motion `trace_id` | `--preset motion-join` |

**One definition, three surfaces.** The L0 CLI preset names map **1:1** onto
these views: `slowest-frames → pulp_slowest_frames`, `xruns → pulp_xruns`,
`dsp-hotspots → pulp_dsp_node_cost`, `layout-vs-paint → pulp_layout_vs_paint`.
The same view is the preset verb, the agent's query primitive, and the human's
`SELECT` target. When you extend the stdlib, keep that mapping intact.

Load and query in one shot:

```bash
./trace_processor \
  -q .agents/skills/trace-sql/pulp_slowest_frames.sql \
  --query-string "SELECT * FROM pulp_slowest_frames LIMIT 10" \
  /tmp/x.pftrace
```

The category vocabulary the views key off is the fixed taxonomy: `dsp`,
`dsp.node`, `render`, `layout`, `canvas`, `text`, `js`, `gpu`, `state`, `io`.

---

## The draft → validate → execute loop

Do not fire a hand-written query blind against a large trace. Iterate:

1. **Draft** the query from the question and the category taxonomy.
2. **Validate cheaply** — append `LIMIT 5` (or wrap in `SELECT COUNT(*)`) and
   run it. Confirm the columns, the category filter, and that rows come back at
   all. Fix schema/typo/empty-result problems here.
3. **Execute** the full query only once the shape is proven.

Cap validation at **≤ 3 iterations**. If three drafts still return nothing,
the problem is usually not the SQL — it is the capture (wrong categories
traced, ring overflowed to empty, or the span simply was not emitted). Switch
to diagnosing the trace, not the query.

---

## SQL discipline (the rules that bite)

**Idempotency — always `CREATE OR REPLACE PERFETTO`.** Views/functions must be
re-runnable in the same `trace_processor` session without a "already exists"
error. Every stdlib file uses `CREATE OR REPLACE PERFETTO VIEW` /
`... FUNCTION`. Never a bare `CREATE VIEW`.

**`GLOB`, not `LIKE`.** PerfettoSQL `GLOB` is case-sensitive and uses
`*` / `?` — the behavior trace tooling expects (`name GLOB 'xrun*'`). `LIKE` is
case-insensitive and uses `%` / `_`; reaching for it silently changes matching
semantics and folds unrelated spans in. Use `GLOB` for name patterns.

**`dur = -1` means incomplete, not instant.** A slice still open when the ring
flushed (or an unterminated `PULP_TRACE_BEGIN`) has `dur = -1`. It is *not* a
zero-length event. Every duration query filters `WHERE dur >= 0` (or
`dur != -1`). A negative duration leaking into a `SUM`/`ORDER BY` corrupts the
result. Instant events (xruns) genuinely have `dur = 0` — key those off the
name, not the duration.

**`EXTRACT_ARG` for span arguments — mind the `debug.` prefix.** Typed args
(frame index, block index, `motion.trace_id`, sample position) live in the arg
set, not as columns. Pulp emits them as TRACE_EVENT debug annotations, so
`trace_processor` keys them **`debug.<name>`**, not bare:
`EXTRACT_ARG(s.arg_set_id, 'debug.frame_index')`, not `'frame_index'`. A missing
arg (or a wrong/bare key) yields `NULL` silently — this is the number-one reason
a preset returns empty rows. Confirm the real key with
`SELECT DISTINCT key FROM args` against the trace before trusting a query, and
filter `IS NOT NULL` when the arg is a join key.

**Join on stable identity — `utid` / `upid`, never `tid` / `pid`.** OS thread
and process ids are recycled within a trace; Perfetto's `utid` (unique thread
id) and `upid` (unique process id) are stable for the whole capture. Join
`slice → thread_track → thread` on `utid`, and reach the process via
`thread.upid`. Joining on raw `tid`/`pid` will cross-link two threads that
reused an id.

**`SPAN_JOIN` needs `PARTITIONED`.** To intersect two span tables on the time
axis (e.g. correlate `dsp` slices with a concurrent `render` window), use
`SPAN_JOIN` and **partition by the stable key** so spans are only joined within
the same thread/track:

```sql
CREATE VIRTUAL TABLE dsp_x_render USING SPAN_JOIN(
  dsp_spans   PARTITIONED utid,
  render_spans PARTITIONED utid
);
```

Omitting `PARTITIONED` cartesian-joins across unrelated threads and both
explodes the row count and produces meaningless overlaps.

**Percentiles.** SQLite has no percentile builtin. The stdlib views expose
`mean_ms` alongside `max_ms` precisely so the mean-vs-max gap flags a
per-block spiker without a percentile. When the `trace-analysis` harness needs
true p95/p99 tail latency, compute it over the raw `slice` rows with a
window/rank approach (`NTILE`, or `ROW_NUMBER() ... ORDER BY dur` over the
node's slices) rather than trusting the mean.

---

## Common query shapes

```sql
-- Slowest slices in a category (the flamegraph's fattest bars):
SELECT name, dur/1e6 AS dur_ms
FROM slice
WHERE category = 'dsp.node' AND dur >= 0
ORDER BY dur DESC LIMIT 20;

-- Which thread did a span run on:
SELECT s.name, thread.name AS thread, s.dur/1e6 AS dur_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread ON thread.utid = tt.utid
WHERE s.category = 'text' AND s.dur >= 0
ORDER BY s.dur DESC LIMIT 20;

-- Count of a span by name (is TextShaper::prepare firing every frame?):
SELECT name, COUNT(*) AS n, SUM(dur)/1e6 AS total_ms
FROM slice
WHERE category = 'text' AND dur >= 0
GROUP BY name ORDER BY n DESC;

-- Steady-state vs whole-run average per node (drop the single cold-start
-- outlier). A per-node average is often dominated by the FIRST block, where
-- the first process() call warms caches / touches fresh pages — a one-time
-- cost, not a per-block cost. Subtracting the per-node max isolates it:
SELECT name,
       ROUND(AVG(dur)/1e3, 2)                          AS avg_all_us,
       ROUND((SUM(dur) - MAX(dur)) / 1e3 / (COUNT(*) - 1), 2) AS avg_steady_us
FROM slice
WHERE category = 'dsp.node' AND dur >= 0
GROUP BY name ORDER BY avg_all_us DESC;
```

## Gotchas

- **Empty result ≠ zero cost.** An empty query often means the category was not
  traced, or the ring overflowed and the trace is silently truncated. Check
  `SELECT DISTINCT category FROM slice` before concluding "nothing was slow."
- **Wall time, not CPU time.** `slice.dur` is wall-clock span duration. A long
  slice may be *blocked*, not computing — the `trace-analysis` skill's
  wall-time-vs-CPU-time rule (join to `thread_state`) decides which.
- **Category is a string column.** `WHERE category = 'dsp'` does not match
  `'dsp.node'` — they are distinct taxonomy entries. Use `GLOB 'dsp*'` only
  when you deliberately want both.
- **Dynamic span names don't aggregate under `GROUP BY name`.** Most spans use a
  static name (the default), so `GROUP BY name` collapses them into one row per
  callsite — that is what makes "is `TextShaper::prepare` firing every frame?"
  answerable. A span emitted through `PULP_TRACE_SCOPE_DYNAMIC(cat, expr)` carries
  a *runtime-computed* name (a node id, a parameter name), so each distinct value
  is its own `slice.name` and a `GROUP BY name` fragments instead of aggregating.
  If a per-name rollup looks unexpectedly scattered, check whether the callsite is
  dynamic (`SELECT name, COUNT(*) FROM slice WHERE category=... GROUP BY name` will
  show the high-cardinality spread); group by `category` or a name prefix
  (`substr(name, 1, instr(name,'_'))`) instead when you need the aggregate.

## Files this skill covers

- `.agents/skills/trace-sql/pulp_slowest_frames.sql`
- `.agents/skills/trace-sql/pulp_dsp_node_cost.sql`
- `.agents/skills/trace-sql/pulp_frames_over_budget.sql`
- `.agents/skills/trace-sql/pulp_xruns.sql`
- `.agents/skills/trace-sql/pulp_layout_vs_paint.sql`
- `.agents/skills/trace-sql/pulp_motion_join.sql`
- `core/runtime/include/pulp/runtime/trace.hpp` — the macro surface + category taxonomy
- `docs/guides/tracing.md` — the guide, tiers, and worked use cases

## Match `trace_processor` to the SDK's Perfetto pin

`PulpTracing.cmake` pins the Perfetto SDK (`PULP_PERFETTO_VERSION`, v57.2 at time
of writing). Query with the **same** version — `pulp trace fetch`, or
`curl -sSL -o trace_processor https://get.perfetto.dev/trace_processor` which
self-reports its version on `--version`.

The capture producer and query tool are separate compatibility surfaces. An
SDK-built plug-in must link the installed SDK's exported Perfetto support from
the same `PULP_TRACING=ON` configuration; a consumer-side define cannot add
targets or headers omitted at SDK install time. Once a capture exists, query it
with the trace processor matching that SDK pin. When diagnosis starts with “no
rows,” first establish whether a `.pftrace` was actually flushed; do not debug
SQL against a missing producer.

## Cost that lives BETWEEN slices

`pulp_layout_vs_paint` and friends aggregate slice durations, so they cannot see
a stall that happens inside a parent span but outside every child — the classic
case being a blocking swapchain acquire. Two queries catch it:

```sql
-- 1. unaccounted time inside a frame: children summing to far less than the parent
SELECT s.id, s.dur/1e6 AS frame_ms,
       (SELECT SUM(c.dur)/1e6 FROM slice c WHERE c.parent_id = s.id) AS children_ms
FROM slice s WHERE s.name = 'frame' AND s.dur >= 0
ORDER BY (s.dur - IFNULL((SELECT SUM(c.dur) FROM slice c WHERE c.parent_id = s.id),0)) DESC
LIMIT 20;

-- 2. inter-frame gaps: idle or blocked BETWEEN frames
WITH f AS (SELECT ts, dur, LEAD(ts) OVER (ORDER BY ts) AS nts
           FROM slice WHERE name = 'frame' AND dur >= 0)
SELECT COUNT(*) AS gaps, AVG((nts-(ts+dur))/1e6) AS mean_gap_ms,
       MAX((nts-(ts+dur))/1e6) AS max_gap_ms
FROM f WHERE nts IS NOT NULL;
```

Query 1 is what identified the Windows knob-drag regression: frames of 19-45 ms
whose children summed to ~2 ms. Small gaps plus large unaccounted time means the
thread is blocked *inside* the frame, not idle between frames.

## An empty capture may be a missing attachment, not a bad query (WAH-4)

Before reaching for `trace_processor`, confirm the session actually recorded.
Tracing attach/detach was wired into **VST3 only** until WAH-4; a capture of a
CLAP, AU v2, AU v3, AAX or Standalone session produced an empty `.pftrace`
while every command looked correct. All six formats now attach via
`runtime::ScopedTracingAttachment`.

Two other capture-shaping facts worth knowing before you blame a query:

- **The trace is written by the FINAL detach.** A leaked attachment means it is
  never written at all. Let the host finish unloading the plug-in rather than
  killing the process.
- **`PULP_TRACE_SECONDS` timeouts are tagged with a session generation.** They
  used to be untagged, so closing and reopening an editor inside the window let
  the FIRST session's timer stop the SECOND one — a capture that looks
  mysteriously truncated mid-gesture. A stale timer is now a no-op.
- **Zero rows for `frame`/`gpu_*` can mean the host never emitted them.** A
  query over render spans returning nothing is not automatically a bad query or
  a bad capture: an editor that is neither scripted nor declares
  `requires_gpu_host()` runs on CPU raster and emits none of them, and
  host-level `frame`/`paint` exist only on the Windows plug-in editor and the
  macOS standalone host. Check the shape of what you DID capture before
  rewriting SQL —

  ```sql
  select name, count(*) from slice group by name order by 2 desc;
  ```

  A result of only `layout_children` + `wm_mousemove` is the signature. The
  `trace-analysis` skill has the coverage matrix and the `[plugin-gpu-host]
  … mode=` line that names the host you actually got.

## GPU render time is now OPT-IN (WAH-13)

`SkiaSurface::gpu_render_timing_available()` reporting false is no longer
evidence of an adapter that lacks `timestamp-query`. Timestamps are requested
only when the host asks, via `PluginViewHost::Options::enable_gpu_timing`
(default OFF), rather than whenever the adapter advertises the feature.

That default is deliberate and worth understanding before you "fix" it: Dawn
gates `writeTimestamp` behind the `allow_unsafe_apis` toggle on every backend,
so requesting the feature forces that toggle on — and it applies to the DEVICE,
not to the diagnostic. Ordinary rendering was silently running with relaxed
validation on every machine whose adapter happened to offer timestamps.

If you need per-recording GPU time in a capture, enable it explicitly on the
host's Options. If a trace shows no `gpu_render_time`, check that flag before
suspecting the adapter.
