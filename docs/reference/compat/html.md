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

## Counts (2026-05-04)

| Status | Count |
|--------|------:|
| supported | ~45 |
| partial | ~10 |
| missing | ~5 |

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

1. **`html/ARIA`** (missing) — `aria-*` attributes are stored in
   `_attributes` but NOT routed to platform accessibility APIs.
   Major gap; needs dedicated work.
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
