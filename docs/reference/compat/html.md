# HTML / DOM-lite compat

Status of the DOM-lite consumer surface in `core/view/js/web-compat-element.js`
and `core/view/js/web-compat-document.js`. This is the runtime that
implements `document.createElement(...)`, `el.setAttribute(...)`,
`el.appendChild(...)`, `el.addEventListener(...)`, `el.classList`,
`el.dataset`, etc., for plugins that author UI as HTML+CSS.

The authoritative inventory is `compat.json` (`html/*` prefix).

## Generation

The current table describes the checked-in `compat.json` inventory for the HTML
surface.

Spec walk:
[MDN Element / HTMLElement](https://developer.mozilla.org/en-US/docs/Web/API/Element)
+ subclass attribute coverage, ARIA attrs, dataset, classList, the
DocumentFragment shim, Event constructor surface.

The table covers the tag-to-widget mapping, the Element surface (~40 properties
/ methods), the document surface, and the StyleSheet / inline `<style>` parser.

## Counts

Harness verdict (per `tools/harness/verifier --all` after evidence validation):

| Verdict | Count |
|---------|------:|
| PASS    | 2 |
| SUPPORTED-NO-EVIDENCE | 57 |
| DIVERGE | 0 |
| NO-OP   | 0 |
| NOT-IMPL | 0 |
| OOS     | 0 |

Catalog status counts (informational):

| Status | Count |
|--------|------:|
| supported | 59 |
| partial | 0 |
| missing | 0 |

`SUPPORTED-NO-EVIDENCE` means the static implementation checks pass, but the
catalog's `evidence.tests` references are missing, stale, or point at tags that
no longer exist. See `docs/reports/harness-coverage.md` for the full drift
list.

## Current support notes

The catalog classifies behavior by current runtime contract, not by
implementation history. Entries marked `supported` may still document explicit
architectural non-goals when the supported surface is narrower than the full
browser API.

- **`html/ARIA`** — `aria-label` / `role` route through `setAccessibilityLabel` /
  `setAccessibilityRole` to `View::set_access_label_` /
  `_role_`. State routing (aria-pressed/checked/disabled/hidden) is
  partial-deferred-access-state; not part of the supported claim.
- **`html/document_querySelector`** — selector engine covers tag / `.class` /
  `#id` / `[attr]` /
  `[attr=v]` (incl. `^=`/`$=`/`*=`/`|=`/`~=`) / descendant
  (`a b`) / child (`a > b`) / compound forms. Pseudo-classes,
  pseudo-elements, sibling combinators, selector lists, and the
  case-insensitive flag are arch-no-cascade-engine.
- **`html/StyleSheet_inline`** + **`html/style`** —
  arch-no-cascade-engine. The single-pass selector engine (extended
  to `[attr]` / descendant / child selectors) is the supported
  surface. `@media` / `@keyframes` / `@import` / `@font-face` /
  `@supports` are explicit non-goals; consumers reach for `@pulp/react`
  state-dependent components for media-query / keyframe needs.
- **`html/DocumentFragment`** — arch-text-editor-owns-selection. The
  React reconciler hot path (createDocumentFragment + batched
  appendChild) is the supported surface; the W3C Range / Selection
  API is not modeled by design.
- **`html/svg`** — arch-explicit-non-goal. The `<svg>` layout-leaf
  shim reserves flex space; rendered SVG paths route
  through the `@pulp/react` `SvgPath` intrinsic (which is the
  canonical rendered-path API).

## Architectural reclassification

- **`html/dialog`** — `Element.show()` / `showModal()` / `close()` are
  stored-only on the panel; modal dialog rendering is not in the
  paint pipeline today.
- **`html/input`** — `<input>` is
  fully wired through `_ensureNative` -> bridge `createTextEditor` /
  `createFader` / `createCheckbox`.
- **Architectural reclassification (4 entries, status retained):**
  - `html/StyleSheet_inline` and `html/style` — @media / @keyframes /
    @import / @font-face / @supports / complex selectors are
    `arch-no-cascade-engine` (Pulp doesn't model the CSS cascade by
    design; selector evaluation is single-pass tag/.class/#id).
  - `html/DocumentFragment` — full Range / Selection API is
    `arch-text-editor-owns-selection` (Pulp's text editor has its
    own selection model).
  - `html/svg` — full SVG rasterization of children is
    `arch-explicit-non-goal` (createCol shim reserves layout space;
    use @pulp/react SvgPath intrinsic for rendered paths).

## Compatibility fixes

These entries are implemented by the current DOM-lite runtime:

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
- **`html/img`** — `<img>` now creates an `ImageView`; `src`
  routes through `setImageSource` to `ImageView::set_image_path`, and
  Skia-backed canvases decode file paths through `draw_image_from_file`.
  Empty or undecodable sources still render the built-in `IMG`
  placeholder.

Current divergent entries: none. Remaining non-PASS rows in the harness
report are `SUPPORTED-NO-EVIDENCE` evidence-anchor refresh work.

## Tag → widget mapping

| HTML tag | Widget | Notes |
|---|---|---|
| `div`, `section`, `article`, `aside`, `header`, `footer`, `nav`, `main` | `createCol` | Generic flow containers (column flex). |
| `span`, `p`, `label` | `createLabel` | Text-content tags map to Label widget, not View. |
| `h1`–`h6` | `createLabel` + `setFontSize` + `setFontWeight` | Heading levels carry default size/weight per level. |
| `button` | `createToggleButton` | Inherits ToggleButton — toggleable by default. |
| `input` | `createTextEditor` (default), `createFader` (type=range), `createCheckbox` (type=checkbox) | |
| `textarea` | `createTextEditor` + `setMultiLine(1)` | |
| `select` | `createCombo` | |
| `canvas` | `createCanvas` | Use `getContext('2d')` or `getContext('webgpu')`. |
| `progress` | `createProgress` | |
| `hr` | `createCol` + 1px height + grey background | Visual divider. |
| `img` | `createImage` / `ImageView` | `src` routes to `setImageSource`; HTML `width` / `height` attrs reserve flex space. |
| `virtual-list`, `virtuallist` | `createVirtualList` | Recycling fixed-height rich-row list. Use `@pulp/react`'s `<VirtualList>` intrinsic for typed row props. |
| `svg` | `createCol` | Layout-leaf placeholder; HTML width/height reserve space; child shapes do NOT render. For rendered SVG paths, use `<SvgPath>` from `@pulp/react`. |
| `details` | `createCol` | Toggle / summary semantics not modeled. |
| `dialog` | `createPanel` + hidden | `showModal()` / `close()` not wired. |
| `style` | `createCol` + hidden + textContent → CSS parser | `:hover` selector support is implemented. |

## Current runtime behavior

- `html/ARIA`: the storage half (round-trip through `setAttribute` /
  `getAttribute`) works today; platform accessibility-state routing is
  the remaining gap.
- `html/svg`: inline `<svg>` is now a layout
  placeholder that honors the HTML `width` / `height` attributes.
- `html/span`, `html/p`, `html/label`: confirmed mapping to **Label**
  widget (not View).
- `html/style`: inline `<style>` text
  is parsed and applied; `:hover` rules are stashed and toggled on
  mouseenter/mouseleave.
- `html/Element_addEventListener` for hover events:
  `registerHover` is now called when a hover
  listener is registered, so the C++ side actually fires.
- `html/Document_addEventListener`:
  `document.addEventListener` / `removeEventListener` /
  `dispatchEvent` are now real on the document object, where they
  were previously installed as no-ops. The platform Esc handler also fires a synthetic
  `document.dispatchEvent({type:'mousedown'})` so React click-outside
  popovers (`document.addEventListener('mousedown', onDoc)`) close
  without per-app wiring. `removeEventListener` still silently
  ignores unknown handlers — the Three.js cleanup contract is
  preserved.

## Element surface — supported

`setAttribute`, `getAttribute`, `removeAttribute`, `classList` (full),
`dataset` (camelCase data-*), `textContent`, `value`, `hidden`,
`disabled` (re-applies stylesheets, see partial note below),
`appendChild` / `removeChild` / `insertBefore` / `replaceChild`,
`cloneNode`, `addEventListener` (click, mouseenter/leave,
pointerenter/leave, pointerdown/move/up/cancel, gesture*, input,
change, focus, blur, wheel, drop), `dispatchEvent`, `setPointerCapture` /
`releasePointerCapture`, `getBoundingClientRect`, `offsetWidth` /
`offsetHeight` / `clientWidth` / `clientHeight`, `nodeType` /
`nodeName` (for React's reconciler hot path).

## Notable gaps

1. **`html/ARIA`** state routing — `aria-label` and `role` route
   through to `View::set_access_label_` / `set_access_role_`; the
   `aria-pressed` / `aria-checked` / `aria-disabled` / `aria-hidden`
   set is partial-deferred-access-state (the View doesn't expose a
   state slot today). Linux AT-SPI / Windows UIA platform routing is
   tracked under #217.
2. **`html/document_querySelector`** — full CSS Selectors Level 4 is
   arch-no-cascade-engine. Pseudo-classes, pseudo-elements, sibling
   combinators (`+` / `~`), selector lists (`a, b`), and the case-
   insensitive flag are explicit non-goals; consumers reach for
   `@pulp/react` state-dependent components instead.
3. **`html/StyleSheet_inline`** / **`html/style`** —
   arch-no-cascade-engine. `@media`, `@keyframes`, `@import`,
   `@font-face`, `@supports` are explicit non-goals.
4. **`html/DocumentFragment`** — arch-text-editor-owns-selection.
   Range / Selection API is not modeled.
5. **`html/svg`** — arch-explicit-non-goal. Layout-leaf shim only;
   use `<SvgPath>` from `@pulp/react` for rendered paths.
6. **Source-side drag lifecycle** — drop-target registration is wired
   through `addEventListener('drop', ...)` / `registerDrop`, but
   JS-initiated drags, native drag-image rendering, and the full
   `dragstart` / `drag` / `dragend` source lifecycle are not modeled.
   Drop data is surfaced on the synthesized event as `_dropData`, not as
   a full `dataTransfer` object.
8. **Keyboard events** — `keydown` / `keyup` / `keypress` are forwarded
   globally via `__dispatch__` rather than bound per-element.
