# Frame-pipeline hints — `render` / `layout` / `canvas` / `text` / `gpu` spans

Domain knowledge for investigating dropped frames and UI jank. This is
Perfetto's home turf and the **safe side** — the UI thread carries no
real-time-audio hazard, so these answers hold regardless of the DSP story.

## The pipeline, stage by stage

A frame walks: **Yoga layout** (`layout`) → **paint / dirty-walk** (`canvas`) →
**text shaping** (`text`) → **Skia/Graphite record** + **Dawn submit** (`gpu`) →
**present** (`render`). The per-frame wrapper span is `frame` in category
`render` (see `core/view/include/pulp/view/frame_clock.hpp`). Each stage is a
span-worthy boundary; `pulp_layout_vs_paint` gives the one-row-per-stage totals
so you can see where the budget went before drilling in.

## Dropped frames vs the vsync budget

A dropped frame is a `frame` span longer than the refresh interval: 16.667 ms
at 60 Hz, 8.333 ms at 120 Hz. Use `pulp_frames_over_budget_ms(<budget>)` with
the surface's real refresh rate (the default view assumes 60 Hz). Rank by
`over_ms` — the worst overruns first — then open the fattest frame and drill
into its stages. `pulp_slowest_frames` is the unbudgeted "worst frames" list
when you don't yet know the refresh rate.

## The `TextShaper::prepare` re-run trap (the classic)

Pulp's text layout is PreText-style: **`prepare()` is expensive** (shapes text
via SkFont/HarfBuzz, caches segment widths) and **`layout()` is cheap** (pure
arithmetic over the cached widths). See
`core/canvas/include/pulp/canvas/text_shaper.hpp`. The architecture exists
precisely so you shape once and reflow forever.

The trap: a value label (a knob readout, a meter number) that calls
`prepare()` **every frame** instead of preparing once and calling `layout()`.
On the timeline this is a fat `text` span recurring on every frame during an
interaction — e.g. a knob sweep re-shaping its label each update.

Tell it apart from legitimate first-shape cost:

```sql
-- Is prepare firing every frame, or once?
SELECT name, COUNT(*) AS n, SUM(dur)/1e6 AS total_ms
FROM slice WHERE category='text' AND dur>=0
GROUP BY name ORDER BY n DESC;
```

A `prepare`-shaped span with a count near the frame count is the bug; a count
of 1 (or once-per-label at startup) is expected. There is a runtime counter —
`text_shaper_prepare_call_count()` — whose growth during a steady interaction
corroborates the trace. The fix is to cache the `PreparedText` and only call
`layout()` per frame.

## Layout vs paint split

`pulp_layout_vs_paint` attributes the frame budget across `layout` / `text` /
`canvas` / `render` / `gpu` in one query. Use it to decide *which* stage to
open before spending queries on the wrong one:

- Fat `layout` → a JS callback or state change is invalidating layout nodes
  (cross to `hints_js.md`).
- Fat `text` → the `prepare` re-run trap above.
- Fat `canvas` → dirty-rect churn (below).
- Fat `gpu` → submit/present stall (cross to `hints_gpu.md`).

## Dirty-rect churn

Paint cost tracks the dirty area. A frame that repaints far more than the pixels
that actually changed is dirty-rect churn — often a too-coarse invalidation
(invalidating a whole panel when one label changed). Correlate the `canvas`
span cost with the dirty area; if the motion subsystem is active, the
`CostAttributor` carries `dirty_rect_area_px` / `dirty_rect_count` for the same
frame (see the `motion` skill), which corroborates the trace.

## GPU-submit stalls

A `frame` span whose time is dominated by a `gpu`/present child that was
**waiting** (not recording) is a submit/present stall, not CPU cost. Apply the
wall-time-vs-CPU-time rule from the harness — a blocked present is a GPU
pacing / back-pressure problem. Details in `hints_gpu.md`.

## The motion join (what changed × where time went)

When a motion trace is active during capture, `frame` spans carry the motion
`trace_id` as a span arg. `pulp_motion_join` lists the frames a gesture/animation
is on the hook for — so a motion `CostSample` ("frame 412 = 9 ms,
trace_id=knob-sweep") leads straight to that frame's flamegraph. This is the
unique Pulp correlation: motion says *what moved*, tracing says *where the ms
went*. Capture both (`pulp trace start ...` + `pulp motion record ...`) when the
complaint is "it hitches when I do X".

## Startup is a frame-pipeline story too

"Why is my plugin slow to open?" (the flagship) uses these same
`layout`/`paint`/`canvas`/`text`/`gpu`/`js` spans, captured from process/editor
start — bounded, main-thread, deterministic. First-open cost is one-time atlas
build + device init + script eval + first layout; the question is which
dominates and whether it is re-paid on every open. See the harness worked
example and docs/guides/tracing.md use case 1.
