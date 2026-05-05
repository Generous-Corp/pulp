# Canvas2D oracle

The canvas2d oracle is a static reference table of HTML5 Canvas2D methods,
attributes, and Pulp-specific extensions, sourced from:

1. The HTML Living Standard `2dcontext` section
   (<https://html.spec.whatwg.org/multipage/canvas.html#2dcontext>).
2. The actual bridge surface in `core/view/src/widget_bridge.cpp` —
   `register_function("canvasX", ...)` is the truth-of-record for what
   reaches the C++ side.
3. The JS shim at `core/view/js/web-compat-canvas.js` — the layer that
   translates `ctx.fillRect()` etc. into `canvasX(id, ...)` bridge calls.
4. The hard-won gotchas catalogued in `.agents/skills/import-design/SKILL.md`
   under "Canvas2D Bridge Gotchas" (#1–#8) — production-debugged deviations
   from spec.

## Why static, not Chromium-headless?

Per the week-1 plan in `planning/pulp-agent-prompt-harness-week1.md`, the
"oracle" for canvas2d is a static reference table, NOT a Chromium-headless
pixel-diff harness. The latter is a week-3+ upgrade once the static
classifier is in place. The benefits of static for week 1:

* Portable — no headless-Chromium runtime dependency at verifier time.
* Stable — Canvas2D's API surface is well-bounded (~36 methods + ~20
  attributes + a few Pulp-specific extensions).
* Capturable in one JSON file with bridge-mapping intent encoded inline.

## File: `canvas2d-supported.json`

Schema:

```jsonc
{
  "version": "html5-canvas-living-standard",
  "source": "...",
  "entries": {
    "<entryName>": {
      "kind": "method | attribute | context | extension",
      "bridge": ["canvasX", "canvasY"],   // bridge functions the entry should route through
      "values": ["..."],                   // optional — enum value list for attributes like lineCap
      "expectedStatus": "partial | missing", // optional — set when the gotchas catalog
                                             //   says full PASS isn't possible today
      "gotcha": "<gotcha-key>",            // optional — references gotchas table below
      "notes": "..."
    }
  },
  "gotchas": {
    "<gotcha-key>": "human description with SKILL gotcha #N reference"
  }
}
```

Entry names match the catalog short-name (the part after `canvas2d/`).
Examples: `fillRect`, `strokeStyle`, `createRadialGradient`,
`_native_canvasFillCircle`, `getContext_2d`.

## Three classification layers (mirrors the yoga adapter)

The adapter (`tools/harness/adapters/canvas2d.py`) classifies each entry by
combining:

1. **Oracle entry** — does the entry exist + what bridge functions does spec
   route through + what's the spec-allowed value set + are there documented
   gotchas?
2. **Bridge presence** — grep `widget_bridge.cpp` for the named
   `register_function("canvasX", ...)` call. Missing-on-bridge ⇒ `NOT-IMPL`
   for any entry whose oracle says it should route there.
3. **Shim presence** — grep `web-compat-canvas.js` for the prototype
   method or attribute. Missing-in-shim ⇒ `NOT-IMPL`.
4. **Catalog `mapsTo`** — heuristic markers like "Not implemented", "shim
   returns null", "no implementation" still classify as `NOT-IMPL` /
   `NO-OP` even if the bridge is present.

## Regeneration

The oracle is hand-maintained for now. To refresh:

1. Walk new entries in `compat.json` under `canvas2d/`.
2. Cross-reference to bridge: `grep 'register_function("canvas' core/view/src/widget_bridge.cpp`.
3. Cross-reference to shim: `grep 'CanvasRenderingContext2D.prototype' core/view/js/web-compat-canvas.js`.
4. Verify gotchas list is still in sync with `.agents/skills/import-design/SKILL.md` § "Canvas2D Bridge Gotchas".
5. Run `python3 tools/harness/verifier.py --surface=canvas2d` and review the drift list.

## Future: Chromium-headless oracle (week 3+)

When per-op pixel verification matters, swap this static oracle for a
headless-Chromium harness that:

1. Renders each catalog op into both a real `<canvas>` (oracle) and Pulp's
   bridge surface.
2. Captures pixels (or an op-trace) from each.
3. Diffs at the call-recording level (counts of bridge calls + arg shapes)
   AND the pixel level (per-region tolerance).

The static table will continue to drive the per-entry expectedStatus +
gotcha taxonomy; the headless oracle just adds a stricter pixel-bench
PASS gate.
