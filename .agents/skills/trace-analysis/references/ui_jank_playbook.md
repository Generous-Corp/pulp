# Using Perfetto to find UI jank in a live editor

A copy-pasteable workflow for "the editor feels sluggish while I drag / zoom /
while audio plays". It turns a capture into four answers, in this order:

1. **Is this capture valid?** (health + positive controls)
2. **When does it feel bad?** (frame-interval stats per phase — mid-stroke,
   the 2 s after release, steady)
3. **What blocked the worst frames?** (each worst gap classified by its
   culprit slice, the JS entry point, and GPU wait)
4. **What is the editor thread spending its time on in that phase?**

Read the "Measurement mistakes" table at the top of `SKILL.md` first. Every
one of those mistakes produced a clean-looking number from this same workflow.

The SQL lives in [`ui_jank.sql`](ui_jank.sql) (definitions only; idempotent
`CREATE OR REPLACE PERFETTO`, `GLOB` not `LIKE`, `dur = -1` excluded — the
`trace-sql` skill's rules).

---

## 1. Capture

Build with tracing (`pulp build --trace`) and put everything in the launched
standalone's environment:

```bash
PULP_TRACE_PATH=/tmp/run.pftrace \
PULP_TRACE_SECONDS=60 \
PULP_TRACE_RING_KB=1572864 \
PULP_TEST_SIGNAL=noise \
PULP_TEST_POINTER_DRAG='rect:0.20,0.50,0.80,0.50,120,6' \
  ./build-trace/.../YourPlugin.app/Contents/MacOS/YourPlugin
```

| Variable | Why this value |
|---|---|
| `PULP_TRACE_RING_KB` | KB. 60 s of a scripted editor **with audio** writes ~700 MB; 1.5 GB (`1572864`) holds it with margin. The 80 MB default and even 512 MB wrap. Shorten `PULP_TRACE_SECONDS` rather than accept a wrap. |
| `PULP_TRACE_SECONDS` | Let it elapse *and* flush before ending the process; a kill before the flush writes no file. |
| `PULP_TEST_SIGNAL` | `noise` or `sine` (anything else disables it, with a warning on stderr). Real audio through the plugin, so meters, analyzers and signal-scaled work run at in-use cost. `PULP_TEST_SIGNAL_AMPLITUDE` is linear 0–1. |
| `PULP_TEST_POINTER_DRAG` | macOS GPU window host only. `rect:X0,Y0,X1,Y1,N[,R]` — normalized top-left coordinates, N steps, R repeats — injected into the window's own mouse path from its display-link tick. It cannot move the real cursor or touch another app's window. |

**Per-run validity gates.** Record these for every run and throw away any run
that fails one — do not average it in:

- screen unlocked before *and* after the run;
- the app's pid is the frontmost pid for the whole run;
- a window id exists and a screenshot of that window, taken mid-run, shows the
  editor drawing — and someone actually looked at it;
- the trace passes the health check and positive controls below;
- the host load average at start and end (compare runs only at similar load).

Compare builds by **interleaving** runs in one session (A, B, A, B, …) with
the same signal and gesture — never against a number from another session.

## 2. Running the queries

Perfetto v57.2 accepts only one row-returning statement per `-q` file, so
append one final `SELECT` to a copy of the definitions:

```bash
TP="$HOME/.pulp/tools/trace-processor/v57.2/mac-arm64/trace_processor_shell"  # pulp trace fetch
DEFS=.agents/skills/trace-analysis/references/ui_jank.sql
jank() {  # jank <trace> '<final SELECT>'
  local q; q=$(mktemp)
  { cat "$DEFS"; printf '%s\n' "$2"; } > "$q"
  "$TP" -q "$q" "$1" 2>/dev/null | grep -v -e '^column ' -e '^$'
  rm -f "$q"
}
```

Each call reloads the trace (~20 s for 500 MB); run independent calls in
parallel with `&` … `wait`.

## 3. Health first — then positive controls

```bash
jank /tmp/run.pftrace 'SELECT * FROM jank_health;'
```

**Any row condemns the capture.** It lists `data_loss`-severity stats, ring
wraps (`traced_buf_write_wrap_count`), dropped sequences
(`traced_buf_incremental_sequences_dropped` — a whole thread, often the main
thread, gone) and `packet_skipped_seq_needs_incremental_state_invalid`.
`trace_processor` also prints "Trace health issues: Data losses" on load, and a
file whose size is close to the ring size is the same warning. Re-capture with
a bigger ring.

An empty health result only proves nothing overflowed. Now prove something
recorded:

```bash
jank /tmp/run.pftrace 'SELECT * FROM jank_controls;'
```

| Column | Must be | If zero |
|---|---|---|
| `frames` | ≈ 60 × seconds | display link never ran: locked screen, window never attached |
| `paints`, `gpu_acquires` | ≈ `frames` | frames came from something other than the window's paint path |
| `drag_events` | > 0 for a gesture run | the drive never reached the window (macOS host only) |
| `bridge_calls` | large | not a tracing build, or no script editor loaded |

Add your app's own data-delivery slice as a control (for example, analyzer
frames reaching the page). A run with `frames` but no data delivery measured an
idle editor.

## 4. Frame intervals per phase

```bash
jank /tmp/run.pftrace 'SELECT * FROM jank_phase_stats ORDER BY phase;'
```

The editor thread is the one that owns the most `frame` slices
(`thread.is_main_thread` can be NULL in an in-process capture). Strokes are
`native_drag_dispatch` clusters: a gap over 150 ms starts a new stroke, and
clusters of 10 events or fewer are ignored. Phases:

| Phase | Window | What lands there |
|---|---|---|
| `warmup` | first 8 s | startup; excluded from verdicts |
| `mid_stroke` | stroke start + 300 ms → last drag event | per-move handler cost |
| `after_release` | last drag event → +2 s | commits at `pointerup`, data backlog draining, deferred relayout, animation resuming |
| `steady` | 3 s after the last stroke → end | the always-on cost of meters / analyzer / modulation |
| `all` | everything after warmup | gaps no named phase owns |

Report `n`, `avg_ms`, `p50_ms`, `p95_ms`, `max_ms`, `over_50ms`, `over_100ms`
for each phase. **Not the average alone:** one measured editor averaged
16.7 ms with a 145 ms max and felt sluggish; another averaged 32 ms after
release with p95 120 ms and 43 gaps over 100 ms in 12 s, while its mid-stroke
p95 was 51 ms. The release window is where regressions hide.

`stroke` boundaries themselves: `SELECT * FROM jank_stroke;`.

## 5. Classify the worst frames

```bash
jank /tmp/run.pftrace 'SELECT * FROM jank_worst_frames;'
```

For the 15 worst intervals over 33 ms after warmup it gives: the phase, the
longest depth ≤ 1 editor-thread slice overlapping the gap (excluding the
`frame` and `frame_callback_pump` wrappers), that slice's `debug.fn` if it is a
`js_native`, the longest `js_native` call in the gap and its `debug.fn`, the
longest `gpu_acquire`, and `ui_busy_ms` — the editor thread's top-level slice
time inside the gap.

Read `ui_busy_ms` first:

- **`ui_busy_ms` ≈ `gap_ms`, `culprit_ms` ≈ `gap_ms`** — one slice blocked the
  frame. Use the signature table.
- **`ui_busy_ms` ≈ `gap_ms`, `culprit_ms` ≪ `gap_ms`** — a pile-up: several
  medium slices back to back. One measured 219 ms gap was a 38 ms
  `__flushTimers__` followed by eight ~23 ms `raf_flush` pumps with no `frame`
  between them — the thread ran rAF callbacks back to back and never got to
  present. Fix the per-call cost *and* the call count; list the slices in that
  window (query below).
- **`ui_busy_ms` ≪ `gap_ms`** — the thread was not inside any slice. It was
  waiting (GPU back-pressure, a lock, the display link not firing) or doing work
  that emits no span. Follow the blocker (SKILL.md step 4); a userspace-only
  capture cannot tell waiting from unspanned work.

### Signature → usual cause → fix

| Culprit / `debug.fn` | Usual cause | Fix direction |
|---|---|---|
| `js_native` · `__flushTimers__` | a JS timer callback (`setTimeout`/`setInterval`) did heavy work — typically a framework commit scheduled in a timeout, or a debounce firing after release | move the work out of the timer or make it incremental; never commit React from a timer on an interactive path (`view-bridge` checklist) |
| `raf_flush`, or `js_native` · `__flushFrames__` | `requestAnimationFrame` callbacks: paint cost per call, or too many callbacks per frame | cut draw cost (cache static layers, skip unchanged regions); coalesce to one rAF per frame |
| `js_native` · `getLayoutBoxMetrics` (or other layout reads) | layout reads forcing measurement inside handlers / per frame | read geometry once per gesture or on resize, not per move |
| `dom_event_dispatch` / `dom_event_evaluate` | the event handler itself: commits on `pointermove`/`pointerdown`/`pointerup` | keep gesture state in refs and draw from them; commit only for structural changes |
| app-named projection / state-sync slices (Spectr: `spectr_host_param_sync`, `spectr_replace_processing_state`, …) | a full rebuild of derived state on every host/state sync | project incrementally; do not rebuild everything for one changed value |
| `gpu_acquire` large (≫ 2 ms) | GPU back-pressure: too many frames in flight, offscreen layers / blur, oversize surfaces | reduce offscreen passes and layer count; check frames-in-flight; see `hints_gpu.md` |
| `pointer_coalescer_flush` large | pointer delivery work per coalesced batch | the handler cost, as `dom_event_*` above |

Slices named here that Pulp core emits (verified on `origin/main`): `frame`
(`render`), `paint` (`canvas`), `gpu_acquire` (`gpu`) in the window host;
`frame_callback_pump`, `raf_flush` (`js`) in the widget bridge; `js_native`
(`js`, arg `debug.fn`) around every JS→C++ bridge call;
`dom_event_dispatch`/`dom_event_evaluate` (`js`) in bridge dispatch;
`native_drag_dispatch`/`pointer_coalescer_flush` (`state`) in the macOS window
host (`pointer_coalescer_flush` also in `host_drag_coalescer.cpp`). `spectr_*`
slices are emitted by the Spectr app, not by Pulp. `js_native` exists only in
tracing builds and its name is the literal `js_native` — filter on
`name = 'js_native'` and read `EXTRACT_ARG(arg_set_id, 'debug.fn')`; an older
`js_native:<fn>` naming is gone, so `GLOB 'js_native:*'` returns zero rows.

To list what ran inside one gap (take `at_s` from the worst-frames row):

```bash
jank /tmp/run.pftrace "
SELECT ROUND((s.ts - trace_start()) / 1e9, 3) AS at_s, s.depth, s.name,
       EXTRACT_ARG(s.arg_set_id, 'debug.fn') AS fn, ROUND(s.dur / 1e6, 1) AS ms
FROM slice s JOIN thread_track tt ON s.track_id = tt.id
WHERE tt.utid = (SELECT utid FROM jank_ui_thread) AND s.dur >= 0 AND s.depth <= 2
  AND s.ts BETWEEN trace_start() + CAST(25.68e9 AS INT) AND trace_start() + CAST(25.90e9 AS INT)
  AND s.dur > 1000000
ORDER BY s.ts;"
```

## 6. What the editor thread does in a phase

```bash
jank /tmp/run.pftrace "SELECT * FROM jank_ui_breakdown
  WHERE phase = 'after_release' ORDER BY ms_per_s DESC LIMIT 12;"
```

Top-level (depth 0) editor-thread slices, as milliseconds of wall time per
second of phase. 1000 ms/s means the thread never idled. Compare the same
phase across builds; a name that appears only after release is usually the
commit or backlog you are looking for. Wall time is not CPU time — a
userspace-only capture has no `thread_state`, so read a large number as
"occupied", not "computing".

## 7. Content cadence is not frame rate

A steady 60 fps can draw content that only changes 23 times a second (an
analyzer's hop size), and a reset or dropped delivery shows as a visual hitch
with perfect frame pacing. Measure the content separately:

```bash
jank /tmp/run.pftrace "SELECT * FROM jank_cadence('spectr_*') ORDER BY n DESC;"
```

Pass a `GLOB` over your app's delivery slice names. Report `per_s`,
`max_gap_ms` and `gaps_over_150ms`, plus any app-side counters (resets, drops)
the log exposes. A silent input still produces analyzer frames in many
editors — "no input" does not mean "analyzer idle".

## 8. Writing it up

Per phase: `n`, p50/p95/max, counts over 50/100 ms; content cadence; the top
three worst-frame signatures with their fix direction; load average and
validity-gate results per run; build type (a tracing build wraps every
JS→native call in a slice, so absolute costs are inflated — compare deltas
between tracing builds and judge feel on a release build).
