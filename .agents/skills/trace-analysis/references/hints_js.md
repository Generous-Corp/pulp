# JS hints — `js` spans (QuickJS bridge)

Domain knowledge for investigating JavaScript-driven cost in a Pulp UI. Pulp's
scripted GPU UIs run on QuickJS via the view bridge; `js` spans bracket bridge
dispatch, so they are where "the UI is doing invisible work" becomes visible.

## What `js` spans cover

- **Script eval** — one-shot cost of evaluating the bundled UI script at
  startup (`WidgetBridge::load_script`). A single fat `js` span early in a
  startup trace; expected once, a bug if repeated per open.
- **Bridge dispatch** — each call across the C++↔JS boundary: event handlers,
  `requestAnimationFrame` callbacks, property setters the bridge marshals.

## The QuickJS bridge dispatch cost trap

Every crossing of the C++↔JS boundary has marshaling overhead. A UI that
dispatches into JS **per frame** (or worse, several times per frame) pays that
overhead every frame. On the timeline this is a recurring `js` span cadence
locked to the frame rate. The fix is usually to batch bridge calls or move
per-frame state into a form the native side reads without a crossing — not to
optimize the JS itself.

```sql
-- How often is the bridge dispatching, and how much does it cost?
SELECT name, COUNT(*) AS n, SUM(dur)/1e6 AS total_ms, MAX(dur)/1e6 AS max_ms
FROM slice WHERE category='js' AND dur>=0
GROUP BY name ORDER BY total_ms DESC;
```

## The "JS callback invalidated layout" chain (the high-value find)

This is the cross-category story only a unified timeline can tell. A `js` span
(a callback firing) is immediately followed by a fat `layout` span (Yoga
re-running) and then `text`/`canvas`/`gpu` — because the callback mutated a
style/geometry property that dirtied a layout node, forcing a reflow and
re-paint. Motion can say "frame M was expensive"; the trace says "frame M was
expensive **because** a JS callback invalidated a layout node."

To confirm, look for the temporal ordering on the same thread within one frame:
`js` → `layout` → `text`/`canvas`. A `js` span with no following `layout`
inflation is benign; a `js` span that consistently precedes a layout spike is
the culprit. Follow the harness's drill-down: which property did the callback
set, and does it need to on every invocation?

## Traps

- **JS spans run on the UI thread.** They are not a separate "JS thread" — a
  slow callback blocks the frame. Apply the wall-time-vs-CPU-time rule: a `js`
  span that is *blocked* (e.g. awaiting a bridge round-trip) is a different fix
  from one that is *computing*.
- **Eval cost is one-time; dispatch cost is per-frame.** Don't conflate the two.
  A 410 ms script-eval span at open is a caching opportunity (compile once per
  process); a 2 ms dispatch span recurring 60×/s is a batching opportunity.
- **Engine matters.** Pulp can run QuickJS, JavaScriptCore, or V8 (see the
  `engine` skill). Dispatch cost characteristics differ by backend; note which
  backend the capture used before generalizing.
