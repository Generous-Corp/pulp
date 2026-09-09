# Canvas2D Compat

Status of the Canvas2D rendering surface exposed by the web-compat
shim files under `core/view/js/web-compat-canvas*.js` and the native
`canvas*` bridge functions registered through
`core/view/src/widget_bridge_api_manifest.tsv` and
`core/view/src/widget_bridge/canvas2d_api.cpp`.

The authoritative inventory is `compat.json` and the split
`compat/canvas2d.json` catalog (`canvas2d/*` entries). The harness
adapter cross-checks that catalog against the Canvas2D oracle, the
bridge manifest/source, and the JS shim surface.

## Current Counts

Catalog status counts:

| Status | Count |
| --- | ---: |
| supported | 66 |
| partial | 0 |
| missing | 0 |
| noop | 0 |
| wontfix | 0 |

Harness verdict counts after evidence validation:

| Verdict | Count |
| --- | ---: |
| PASS | 18 |
| SUPPORTED-NO-EVIDENCE | 48 |
| DIVERGE | 0 |
| NO-OP | 0 |
| NOT-IMPL | 0 |
| OOS | 0 |

`SUPPORTED-NO-EVIDENCE` means the static implementation checks pass,
but the catalog's `evidence.tests` references are missing, stale, or
point at tags that no longer exist. It is an evidence-refresh problem,
not a Canvas2D bridge/shim support gap.

## Harness Sources

The Canvas2D adapter now scans the monolithic/split C++ source for both
legacy `register_function("canvas...")` calls and split
`register_bridge_function(api, "canvas...")` calls. It uses
`core/view/src/widget_bridge_api_manifest.tsv` only as a last-resort
fallback when source registrations cannot be scanned. This matters
because Canvas2D registrations moved out of the old monolithic
`core/view/src/widget_bridge.cpp` path into the split
`core/view/src/widget_bridge/canvas2d_api.cpp` implementation.

## Evidence Work Remaining

The implementation catalog currently reports every Canvas2D entry as
`supported`, but most rows still need valid `evidence.tests` anchors.
The first cleanup pass should refresh or remove dangling references for
the rows that currently demote to `SUPPORTED-NO-EVIDENCE`:

| Entry group | Current stale reference shape |
| --- | --- |
| Broad bridge backfill rows such as `_native_canvasFillCircle`, `fillRect`, `beginPath`, `getLineDash` | `test/test_widget_bridge.cpp [issue-1711][evidence-backfill]` |
| Shim expansion rows such as `arc`, `bezierCurveTo`, `lineCap`, `lineJoin`, `transform` | `test/test_canvas2d_shim.cpp [issue-1526]` or adjacent stale issue tags |
| Former gotcha rows such as `arcTo`, `filter`, `direction` | catalog entries are supported, but evidence anchors need a current test tag or explicit removal |

Do not reintroduce `partial` or `DIVERGE` just because evidence tags are
stale. Use `SUPPORTED-NO-EVIDENCE` until the evidence references are
reconciled with live tests.

## Backend Notes

Canvas2D is backed by Skia on Linux/Windows and Core Graphics on macOS.
The catalog notes still preserve backend-specific history where it is
useful for future audits, but the current machine-readable status is the
catalog plus harness output above.

## Text Baseline Default

`textBaseline` initializes to `alphabetic`, which is the Canvas2D initial
value: the `y` passed to `fillText` / `strokeText` is the baseline itself,
not the top of the em box. Browser-authored canvas code that never assigns
`ctx.textBaseline` depends on that, so a `top` default silently displaces
every such caption down by one ascent.

All six spec values are honored end to end — `top`, `hanging`, `middle`,
`alphabetic`, `ideographic`, `bottom`. `hanging` and `ideographic` carry no
dedicated font metric in the shaper and are approximated from ascent and
descent, which is how engines behave when the face has no `BASE` table. The
bridge keeps `top`, `middle` and `bottom` on their original wire codes, so a
canvas recorded before the newer values existed replays unchanged.

## Shared WidgetBridge event dispatch

Canvas elements use the same single DOM fan-out as other native widgets. Wheel
input is normalized in `__dispatch__` (including the legacy positional
`deltaX`, `deltaY` form) and dispatched exactly once; the registration callback
only keeps the native channel alive. This changes event delivery, not the
Canvas2D drawing inventory or backend status above.

## Batched path emission

`moveTo` and `lineTo` do not cross the bridge one point at a time. The shim
buffers a contiguous run of path points and ships it as a single
`canvasPathPolyline(canvasId, coords)` call, where `coords` is a flat
`[x0, y0, x1, y1, …]` array; the native side expands it back into the same
`move_to` + `line_to` command sequence the per-point calls produced. `rect`
composes through that same buffer. A malformed run — odd length, a
non-finite coordinate, or more than 65536 values — is rejected whole, so a
bad batch never records a partial path.

This changes the wire, not the Canvas2D drawing inventory or the recorded
command stream: `moveTo`, `lineTo` and `rect` keep their catalog status and
their observable output. The per-point `canvasMoveTo` / `canvasLineTo`
bridge functions remain registered and are still used when
`canvasPathPolyline` is absent, so a host on an older bridge surface renders
identically.

The invariant that makes it safe is a flush: every other shim method that
emits a `canvas*` bridge call must call `this._fp()` first, or its command
would land ahead of the buffered points and silently reorder the path.
`tools/scripts/check_canvas_path_flush.py` (ctest `canvas-path-flush-lint`)
enforces that for every bridge-emitting method in the shim.
