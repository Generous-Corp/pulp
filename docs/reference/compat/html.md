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

## Counts (2026-05-07 — DIVERGE→PASS sweep)

Harness verdict (per `tools/harness/verifier`):

| Verdict | Count |
|---------|------:|
| PASS    | 51 |
| DIVERGE | 7  |
| NO-OP   | 1  |
| NOT-IMPL | 0 |
| OOS     | 0 |

Catalog status counts (informational):

| Status | Count |
|--------|------:|
| supported | ~50 |
| partial | ~7 |
| missing | ~3 |

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

Remaining DIVERGE (deferred — architectural work):

| Entry | Why deferred |
|---|---|
| `html/ARIA` | Needs bridge `setAccessibilityLabel` / `setAccessibilityRole` / `setAccessibilityState` setters. |
| `html/DocumentFragment` | Full Range / Selection API. |
| `html/StyleSheet_inline` | `@media` / `@keyframes` / `@import` / `@font-face` / `@supports` parsers + complex selector engine. |
| `html/document_querySelector` | CSS-spec-conforming selector engine for attribute / pseudo-class / combinator selectors. |
| `html/img` | Image src loading pipeline (Skia codec). |
| `html/style` | Subset of `StyleSheet_inline`. |
| `html/svg` | Full SVG path / shape rasterization. |

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

1. **`html/ARIA`** (partial) — `aria-*` attributes round-trip through
   `setAttribute` / `getAttribute` (storage works), but the html-compat
   layer does NOT yet forward them onto the platform accessibility
   bridge. The bridge itself is in good shape (macOS / iOS / Android
   done, Windows UIA + Linux AT-SPI in flight under #217); the missing
   piece is the html-side translation. Tracked under #1476.
2. **`html/Element_disabled`** (partial) — re-applies stylesheets but
   does NOT call the bridge's `setEnabled`. Native widget disabled-
   state is not toggled.
3. **`html/Event_constructor`** (partial) — `_makeEvent` factory
   exists, but `new Event('foo')` user-side construction surface is
   not wired.
4. **`html/document_querySelector`** (partial) — only `#id`, `.class`,
   and `tagname` selectors. Complex selectors / attribute selectors /
   pseudo-classes / combinators are not supported.
5. **`html/StyleSheet_inline`** — no `@media`, `@keyframes`, `@import`,
   `@font-face`, `@supports`, or complex selectors.
6. **Drag and drop** — `dragstart` / `drag` / `dragend` not wired
   through addEventListener; use `registerDrop` directly.
7. **Wheel events** — `wheel` not wired through addEventListener; use
   `registerWheel` directly.
8. **Keyboard events** — `keydown` / `keyup` / `keypress` are forwarded
   globally via `__dispatch__` rather than bound per-element.
