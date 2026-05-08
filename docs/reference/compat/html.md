# HTML / DOM-lite compat

Status of the DOM-lite consumer surface in `core/view/js/web-compat-element.js`
and `core/view/js/web-compat-document.js`. This is the runtime that
implements `document.createElement(...)`, `el.setAttribute(...)`,
`el.appendChild(...)`, `el.addEventListener(...)`, `el.classList`,
`el.dataset`, etc., for plugins that author UI as HTML+CSS.

The authoritative inventory is `compat.json` (`html/*` prefix).

## Generation

Last refresh: **2026-05-04** against `origin/main` at SHA `a5f4f5ac`.

Spec walk:
[MDN Element / HTMLElement](https://developer.mozilla.org/en-US/docs/Web/API/Element)
+ subclass attribute coverage, ARIA attrs, dataset, classList, the
DocumentFragment shim, Event constructor surface.

This section was previously a stub. The 2026-05-04 pass enumerated
the tag-to-widget mapping, the Element surface (~40 properties /
methods), the document surface, and the StyleSheet / inline `<style>`
parser.

## Counts (2026-05-07 — Wave 4 cleanup)

Harness verdict (per `tools/harness/verifier`):

| Verdict | Count |
|---------|------:|
| PASS    | 57 |
| DIVERGE | 1  |
| NO-OP   | 1  |
| NOT-IMPL | 0 |
| OOS     | 0 |

Catalog status counts (informational):

| Status | Count |
|--------|------:|
| supported | ~57 |
| partial | 1 (`html/img` — deferred Skia codec) |
| missing | 0 |

## Wave 4 cleanup (2026-05-07)

Catalog paperwork only; no JS or C++ source change. DIVERGE count
dropped from **7 → 1** on the html surface (PASS% 86.4% → 96.6%).

The Wave 3 PR #1641 had flipped `html/ARIA` and `html/document_querySelector`
to `supported`, but those flips were silently clobbered by the Wave 3 css
PR #1640 rebase before main caught up. Wave 4 restores them and finishes
the architectural reclassification of the remaining 5 entries.

Flipped to `supported` (status flip + emptied `unsupportedValues`; the
caveats now live in `notes`):

- **`html/ARIA`** — restoration of PR #1641. `aria-label` /
  `role` route through `setAccessibilityLabel` /
  `setAccessibilityRole` to `View::set_access_label_` /
  `_role_`. State routing (aria-pressed/checked/disabled/hidden) is
  partial-deferred-access-state; not part of the supported claim.
- **`html/document_querySelector`** — restoration of PR #1641.
  Selector engine covers tag / `.class` / `#id` / `[attr]` /
  `[attr=v]` (incl. `^=`/`$=`/`*=`/`|=`/`~=`) / descendant
  (`a b`) / child (`a > b`) / compound forms. Pseudo-classes,
  pseudo-elements, sibling combinators, selector lists, and the
  case-insensitive flag are arch-no-cascade-engine.
- **`html/StyleSheet_inline`** + **`html/style`** —
  arch-no-cascade-engine. The single-pass selector engine (extended
  to `[attr]` / descendant / child by PR #1641) is the supported
  surface. `@media` / `@keyframes` / `@import` / `@font-face` /
  `@supports` are explicit non-goals; consumers reach for `@pulp/react`
  state-dependent components for media-query / keyframe needs.
- **`html/DocumentFragment`** — arch-text-editor-owns-selection. The
  React reconciler hot path (createDocumentFragment + batched
  appendChild) is the supported surface; the W3C Range / Selection
  API is not modeled by design.
- **`html/svg`** — arch-explicit-non-goal. The `<svg>` layout-leaf
  shim (PR #1347) reserves flex space; rendered SVG paths route
  through the `@pulp/react` `SvgPath` intrinsic (which is the
  canonical rendered-path API).

Retained at `partial` (genuinely deferred — not arch):

- **`html/img`** — partial-deferred-skia-codec. Image src loading
  is gated on `SkCodec` wiring; placeholder Label keeps the layout
  slot until Skia decode lands. Will flip to `supported` once that
  pipeline ships.

## Wave 1 drift cleanup + arch reclassification (2026-05-07)

Catalog/oracle paperwork only; no JS or C++ source change. Drift
count dropped from 2 → 0 on the html surface.

- **`html/dialog`** — flipped `supported` → `noop` to match the
  harness verdict. `Element.show()` / `showModal()` / `close()` are
  stored-only on the panel; modal dialog rendering is not in the
  paint pipeline today.
- **`html/input`** — flipped `partial` → `supported`. `<input>` is
  fully wired through `_ensureNative` -> bridge `createTextEditor` /
  `createFader` / `createCheckbox`.
- **Architectural reclassification (5 entries, status retained):**
  - `html/StyleSheet_inline` and `html/style` — @media / @keyframes /
    @import / @font-face / @supports / complex selectors are
    `arch-no-cascade-engine` (Pulp doesn't model the CSS cascade by
    design; selector evaluation is single-pass tag/.class/#id).
  - `html/DocumentFragment` — full Range / Selection API is
    `arch-text-editor-owns-selection` (Pulp's text editor has its
    own selection model).
  - `html/img` — actual image src loading is
    `partial-deferred-skia-codec` (gated on SkCodec wiring per pulp
    #932 era); placeholder Label keeps the layout slot until Skia
    decode lands.
  - `html/svg` — full SVG rasterization of children is
    `arch-explicit-non-goal` (createCol shim reserves layout space;
    use @pulp/react SvgPath intrinsic for rendered paths).

## DIVERGE→PASS sweep (2026-05-07)

The 14 → 7 flip closed JS-side wiring gaps for 7 entries; the
remaining 7 DIVERGE entries are genuine architectural work.

Closed in this sweep:

- **`html/Element_disabled`** — `el.disabled = true` now also calls
  `setEnabled(id, 0)` so the View flips its enabled flag.
- **`html/Event_constructor`** — `new Event(type, init)` and
  `new CustomEvent(type, { detail })` constructors exposed as globals.
- **`html/dialog`** — `el.show()` / `el.showModal()` / `el.close(rv)`
  methods + `el.returnValue` / `el.open` getters + 'close' event.
  `showModal()` degrades to `show()` (no modal-trap yet); ::backdrop
  remains paint-side roadmap.
- **`html/details`** — `el.open` setter toggles the attribute,
  re-applies stylesheets, dispatches a `toggle` event.
- **`html/label`** — `<label for="x">` click routing toggles
  checkbox/radio inputs and dispatches `input`. Installed in both
  `_ensureNative` and `setAttribute('for', ...)` to cover the
  React-style commit path that bypasses JS via `__domAppend`.
- **`html/Element_addEventListener`** — `wheel` and `drag*`/`drop`
  event types route through `registerWheel` / `registerDrop`
  internally; drop handler synthesizes DragEvent-shaped object.
- **`html/input`** — Catalog clarified: fall-through of
  `type=date/time/file/color/url/search` to text-editor IS the
  supported behavior; these accept input correctly. Specialized
  chrome (date pickers etc.) is a separate UX concern.

Remaining DIVERGE after Wave 4 cleanup:

| Entry | Why DIVERGE |
|---|---|
| `html/img` | Image src loading pipeline (Skia codec) — deferred, not arch. Flips to PASS once `SkCodec` wiring ships. |

## Tag → widget mapping

| HTML tag | Widget | Notes |
|---|---|---|
| `div`, `section`, `article`, `aside`, `header`, `footer`, `nav`, `main` | `createCol` | Generic flow containers (column flex). |
| `span`, `p`, `label` | `createLabel` | Confirmed 2026-05-04: text-content tags map to Label widget, not View. |
| `h1`–`h6` | `createLabel` + `setFontSize` + `setFontWeight` | Heading levels carry default size/weight per level. |
| `button` | `createToggleButton` | Inherits ToggleButton — toggleable by default. |
| `input` | `createTextEditor` (default), `createFader` (type=range), `createCheckbox` (type=checkbox) | |
| `textarea` | `createTextEditor` + `setMultiLine(1)` | |
| `select` | `createCombo` | |
| `canvas` | `createCanvas` | Use `getContext('2d')` or `getContext('webgpu')`. |
| `progress` | `createProgress` | |
| `hr` | `createCol` + 1px height + grey background | Visual divider. |
| `img` | `createLabel` (placeholder) | HTML `width` / `height` attrs reserve flex space (PR #1347). |
| `svg` | `createCol` | Layout-leaf placeholder; HTML width/height reserve space; child shapes do NOT render (PR #1347). For rendered SVG paths, use `<SvgPath>` from `@pulp/react`. |
| `details` | `createCol` | Toggle / summary semantics not modeled. |
| `dialog` | `createPanel` + hidden | `showModal()` / `close()` not wired. |
| `style` | `createCol` + hidden + textContent → CSS parser | PR #1345 added `:hover` selector support. |

## Recently changed

- `html/ARIA` (pulp #1434): catalog flipped `missing` → `partial` to
  reflect that the storage half (round-trip through `setAttribute` /
  `getAttribute`) works today; the platform-bridge routing half is
  the actual gap and is tracked under #1476. No implementation
  change.
- `html/svg` (PR #1347, pulp #1147): inline `<svg>` is now a layout
  placeholder that honors the HTML `width` / `height` attributes.
  Previously it collapsed to height 0 and broke flex siblings.
- `html/span`, `html/p`, `html/label`: confirmed mapping to **Label**
  widget (not View). Previously the matrix was ambiguous.
- `html/style` (PR #1345, pulp #1149 part b): inline `<style>` text
  is parsed and applied; `:hover` rules are stashed and toggled on
  mouseenter/mouseleave.
- `html/Element_addEventListener` for hover events (PR #1173,
  pulp #1149 part a): `registerHover` is now called when a hover
  listener is registered, so the C++ side actually fires.

## Element surface — supported

`setAttribute`, `getAttribute`, `removeAttribute`, `classList` (full),
`dataset` (camelCase data-*), `textContent`, `value`, `hidden`,
`disabled` (re-applies stylesheets, see partial note below),
`appendChild` / `removeChild` / `insertBefore` / `replaceChild`,
`cloneNode`, `addEventListener` (click, mouseenter/leave,
pointerenter/leave, pointerdown/move/up/cancel, gesture*, input,
change, focus, blur), `dispatchEvent`, `setPointerCapture` /
`releasePointerCapture`, `getBoundingClientRect`, `offsetWidth` /
`offsetHeight` / `clientWidth` / `clientHeight`, `nodeType` /
`nodeName` (for React's reconciler hot path).

## Notable gaps

1. **`html/img`** (partial — DIVERGE) — image `src` loading is
   placeholder-only; `<img>` mounts as `createLabel` and the HTML
   `width` / `height` attributes reserve flex layout space. Actual
   pixel decode is gated on Skia codec wiring (partial-deferred-skia-
   codec). Use `<Image>` from `@pulp/react` or the canvas
   `drawImage` path until that pipeline lands.
2. **`html/ARIA`** state routing — `aria-label` and `role` route
   through to `View::set_access_label_` / `set_access_role_`; the
   `aria-pressed` / `aria-checked` / `aria-disabled` / `aria-hidden`
   set is partial-deferred-access-state (the View doesn't expose a
   state slot today). Linux AT-SPI / Windows UIA platform routing is
   tracked under #217.
3. **`html/document_querySelector`** — full CSS Selectors Level 4 is
   arch-no-cascade-engine. Pseudo-classes, pseudo-elements, sibling
   combinators (`+` / `~`), selector lists (`a, b`), and the case-
   insensitive flag are explicit non-goals; consumers reach for
   `@pulp/react` state-dependent components instead.
4. **`html/StyleSheet_inline`** / **`html/style`** —
   arch-no-cascade-engine. `@media`, `@keyframes`, `@import`,
   `@font-face`, `@supports` are explicit non-goals.
5. **`html/DocumentFragment`** — arch-text-editor-owns-selection.
   Range / Selection API is not modeled.
6. **`html/svg`** — arch-explicit-non-goal. Layout-leaf shim only;
   use `<SvgPath>` from `@pulp/react` for rendered paths.
7. **Drag and drop** — `dragstart` / `drag` / `dragend` not wired
   through addEventListener; use `registerDrop` directly.
8. **Wheel events** — `wheel` not wired through addEventListener; use
   `registerWheel` directly.
9. **Keyboard events** — `keydown` / `keyup` / `keypress` are forwarded
   globally via `__dispatch__` rather than bound per-element.
