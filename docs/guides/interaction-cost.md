# Keeping a drag interaction cheap

A pointer drag samples at display rate. Anything a Pulp app does *per sample*
runs 60–120 times a second, so a cost that is invisible in a click is fatal in
a drag. This guide states the invariants that keep a drag cheap, the two places
they are enforced, and how to measure a violation rather than guess at one.

It is written from a real regression: band drawing in a design-imported app
went from smooth to visibly sluggish, and the 95th-percentile pointer dispatch
measured **326 ms** against **1.6 ms** once the invariant below was restored.

## The invariant

> A pointer sample must not write UI framework state, and must not change the
> measured geometry of any widget.

"Pointer sample" means every `pointermove`, not only a drag: a plain hover over
a dense target (64 spectrum bands, a keyboard, a grid) crosses a target on
nearly every move. The same rule covers any per-frame path — an animation tick,
an LFO, a meter — because it runs at the same rate.

Both halves matter, and they fail for different reasons.

### A pointer sample must not write framework state

In a design-imported app the captured document is the layout authority. When a
React commit reaches the host config, the runtime re-applies the captured
import metadata — that is what keeps native layout agreed with the captured
geometry. It is correct, and its cost follows the size of the document, not the
size of the change. Current runtimes scope the re-apply's bridge calls to the
subtree a commit mutated, but still resolve every binding per commit; an app
that vendors an older `runtime.js` pays the full-document pass. Either way, a
commit per pointer sample is the wrong shape.

So one `setState` per pointer sample turns a drag into one full-document
re-apply per sample. In the measured regression that was ~43,000
`setFontFamily` calls and ~1,550 layout passes over a single drag, and 94% of
the worst frame was JS self-time inside the re-apply walk — not native layout,
not painting.

Live readouts are the usual offender, because a readout genuinely does change
every sample. Drive them from a ref and paint them directly:

```js
const hoverRef = useRef(null);
const readoutRef = useRef(null);        // a small text node, e.g. a tooltip
const onPointerMove = (e) => {          // registered ONCE, not also as ...Capture
  const next = hitTest(e);
  if (sameHover(hoverRef.current, next)) return;  // unchanged: touch nothing
  hoverRef.current = next;              // the canvas draw path reads this
  requestRepaint();                     // the canvas repaints from refs
  const el = readoutRef.current;        // readout text + position, imperatively
  if (el) { el.textContent = next ? next.label : ''; el.style.left = `${e.x}px`; }
};
```

The canvas already re-paints per sample; reading the ref there costs nothing.
Publishing the same value into framework state buys no pixels and pays for a
whole document. "Only commit when the hovered target changes" is not enough on
a dense target: with 64 bands under a moving mouse the target changes on nearly
every move, so that guard still commits per move.

In one measured case, an LFO animating 64 bands ran at a steady 60 fps with the
mouse still and stalled for 100 ms to 2.5 s per frame while the mouse moved. A
timing wrapper around `__dispatch__` put each `pointermove` at ~42 ms — against
~0.2 ms for `mousemove` on the same element — and each slow event re-applied the
document (~150 `getLayoutBoxMetrics`, ~160 `setFlex`, ~195 `setFontFamily`, ~12
layout passes). Drawing was cheap throughout (~2.5 ms paint, ~1–4 ms
`gpu_acquire`). Moving the pointer path to refs, imperative DOM and a repaint
took `pointermove` to ~0.6 ms and removed every >100 ms stall.

#### Where the per-move commits come from

Each of these commits on a pointer or frame path. Audit all of them — fixing one
leaves the stall in place while any other remains:

1. **Hover or pointer state in React state.** `setHover(next)` per move,
   including the "only when it changed" form on dense targets. Keep it in a ref.
2. **Status or readout published through a high component's state.** An effect
   that pushes a hover/status string into the root (or any ancestor) re-renders
   and commits from there. Write the text node directly.
3. **Setters called with an unchanged value.** Do not rely on React's
   same-value bail-out — in the `@pulp/react` runtime a same-value
   `setCursor(...)` still committed. Compare first; never call a setter with
   the value it already holds.
4. **One handler registered twice.** The same function as both
   `onPointerMoveCapture` and `onPointerMove` runs twice per move. Handle each
   event once.
5. **Transient overlays that hide and re-show through state.** A banner whose
   hide timer calls `setVisible(false)` and whose next update re-shows it via
   state commits twice per cycle. Keep it mounted and restart its timer
   imperatively.

### A pointer sample must not change measured geometry

A widget whose measured size can change dirties layout, and a dirty tree costs
a Yoga pass over every node — not just the one that changed.

`Label::set_text` is where this bites, because a live readout is a label whose
text changes every sample. It skips `invalidate_layout()` when the new copy
provably cannot move the box:

- the horizontal axis must be pinned by an explicit width, and
- the label must be single-line, horizontal, non-attributed, and not a
  captured-wrap fallback, and
- the vertical axis must be pinned — either by an explicit height, or by
  measuring the line box before and after the write and finding it unchanged, and
- the label must not participate in baseline alignment.

The measured form matters. Requiring a declared height on both axes sounds
safe, but design-import emits a declared width and an implicit line-box height
— authors write an explicit height only when centering forces them to. So the
*common* case missed the fast path, and a routine CSS cleanup that dropped a
`height: 100%` silently reintroduced a full-tree relayout per sample.

**The probe is not a cost — it is a saving.** Measured over a 60-sample
synthetic drag on a width-only label, counting
`canvas::text_shaper_prepare_call_count()` and `View::layout_pass_count()`:

| | shaper `prepare()` calls | layout passes |
|---|---|---|
| probe on (fast path taken) | 60 — one per write | **0** |
| probe off (invalidate per write) | 120 — two per write | **60** |

`Label::intrinsic_height()` is memoized on a fully-resolved `MeasureBasis`
that includes the text, so the pre-write probe is a memo hit in steady state
and the post-write probe is the miss. That single shaping is then cached for
the paint and measure calls that follow. Skipping the probe does not avoid
that work — it defers it into a Yoga pass that asks for the text *twice*.

**Baseline alignment is the one case an unchanged height does not cover.**
Under `align-items: baseline` Yoga places the row from each item's baseline,
and `yoga_baseline()` asks `Label::baseline_y()`, which shapes the text and
returns `PreparedText::ascent()`. `TextShaper::prepare` maxes ascent, descent
and leading *independently* against the shaped box, so copy can hold the line
height constant while moving the ascent. `baseline_y()` also ignores the box
height entirely, so an explicit height pins the box without pinning the
baseline. A baseline participant therefore reflows on every text write.

No text pair on this platform's font stack actually exercises that move — an
exhaustive scan (889 single-codepoint samples across 23 Unicode blocks, 15
distinct ascent/descent/leading triples, all 79 combinations reachable by
mixing them) produced 79 distinct line heights and zero equal-height /
differing-ascent pairs. The scan's detector was positive-controlled against a
synthetic face offset by +1 ascent / −1 descent, which it did report. The guard
is kept because the fast path ships to every Pulp app, on font stacks that scan
never saw.

Intrinsic-width, multiline, vertical, attributed, and captured-wrap labels keep
the conservative path: their text really can move their siblings.

## Measuring it

Do not infer this from reading code. Two instruments answer it directly.

**Layout passes** — `View::layout_pass_count()` is a process-wide counter. Take
a delta around the interaction:

```cpp
const auto before = View::layout_pass_count();
/* drive N pointer samples */
CHECK(View::layout_pass_count() - before == 0);
```

A zero here is only meaningful with a negative control in the same test, on the
same tree, that must read non-zero — an intrinsic-width label, say. Without one,
a broken instrument and a clean build are indistinguishable, and the broken
instrument is the one that looks like good news.

**Commits per event** — wrap the global `__dispatch__(id, type, payload)` with
a timer and bucket by event type. A `pointermove` that costs orders of
magnitude more than a `mousemove` on the same element is the React handler, not
event delivery. Counting bridge calls (`getLayoutBoxMetrics`, `setFlex`,
`setFontFamily`) inside one event tells you whether it committed.

**A Perfetto trace** — build a traced SDK (`PULP_TRACING=ON`, kept under
`~/.pulp/sdk-trace/`, never shipped) and drive the interaction. The fingerprint
of a re-apply storm is a `dom_event_evaluate` slice that dominates its own
frame with *self* time while the bridge calls under it are individually
trivial, plus bridge-call counts in the tens of thousands over one drag.

Report **medians and p95, never means** — the defect is a periodic hitch, and a
mean spreads one 326 ms stall across every sample until it looks like noise.
Frame median barely moved across this entire regression; p95 moved 200×.

Capture it so the numbers are about the app and not the capture:

- **Drive a real mouse.** Post `CGEventCreateMouseEvent(kCGEventMouseMoved)` at
  60 Hz across the target, and run a deterministic animation source (an LFO)
  at the same time. Compare a window *during* the sweep against a window of the
  *same animation with the mouse still*, in one trace.
- **Do not measure while a scripted-scenario harness is stepping.** Its
  per-step snapshots add 100–800 ms stalls that look exactly like app jank.
- **Size the ring for a script-heavy capture.** Set `PULP_TRACE_RING_KB` large
  (`524288`); an overrun drops the main thread's whole sequence
  (`traced_buf_incremental_sequences_dropped` in `stats`).
- **Let the flush happen.** Wait for `PULP_TRACE_SECONDS` plus flush time
  before ending the process; a kill before that writes no file.
- **Name the cost by slice.** `dom_event_dispatch`, `dom_event_evaluate` and
  `__flushTimers__` carry the JS-side cost, `gpu_acquire` the GPU wait, and
  `paint` the drawing.

The `trace-analysis` skill has the full capture and truncation checks.

Note that a userspace-only trace carries no `thread_state`, `sched`, or
per-slice `thread_dur` rows, so you cannot split wall from CPU time by joining
them. Attribute the frame by descendant self-time instead, and say that is what
you did.

## Why this keeps regressing

The invariants are cheap to satisfy and invisible to every other kind of test.
A violation renders identically, passes a screenshot diff, passes a pixel
comparison against the design source, and passes any browser fixture — the
browser has no captured-geometry re-apply and no Yoga tree. It shows up only as
a feel, which is why it reaches a user before it reaches a gate.

So state each half as an executable check rather than a convention:

- geometry — a ctest driving a synthetic drag and asserting a zero
  `layout_pass_count()` delta, with the mandatory non-zero control
  (`test/test_widget_bridge.cpp`, `[layout][perf][text]`);
- framework state — a static detector over the shipped artifact asserting the
  pointer path reaches no state setter unguarded.

The second one exists because the regression was **artifact drift**. The hover
path was correct in both generator sources the whole time; only the checked-in
artifact disagreed, and nothing compared them. A rule that lives in a generator
protects nothing if a build can ship an artifact that ignores it.
