# Web Standards Coverage

Pulp's web-compat layer was audited against **26 web specifications** — 16 W3C CSS/DOM specs plus 10 Web API specs that frontend developers rely on. This document tracks what's implemented, what's not, and how to run validation tests.

## How to Run Tests

```bash
# All tests (1247 total, ~31s)
ctest --test-dir build --output-on-failure --exclude-regex AudioWorkgroup

# Web-compat tests only (parser, layout, events, selectors, screenshots)
./build/test/web-compat/pulp-test-web-compat-parser
./build/test/web-compat/pulp-test-web-compat-events
./build/test/web-compat/pulp-test-web-compat-layout
./build/test/web-compat/pulp-test-web-compat-reftest
./build/test/web-compat/pulp-test-web-compat-fixture

# Platform maturity tests (cursor, focus, IME, accessibility)
./build/test/pulp-test-platform-maturity

# Widget bridge tests
./build/test/pulp-test-widget-bridge
```

---

## Spec-by-Spec Coverage

### 1. CSS Flexible Box Layout Level 1
**W3C:** https://www.w3.org/TR/css-flexbox-1/
**Status:** ✅ Complete

| Property | Status | Notes |
|----------|--------|-------|
| `flex-direction` | ✅ | row, column |
| `flex-wrap` | ✅ | wrap, nowrap |
| `flex-flow` | ✅ | Shorthand for direction + wrap |
| `justify-content` | ✅ | start, center, end, space-between, space-around, space-evenly |
| `align-items` | ✅ | start, center, end, stretch |
| `align-self` | ✅ | start, center, end, stretch, auto |
| `align-content` | ✅ | Multi-line flex cross-axis |
| `order` | ✅ | Integer ordering |
| `flex-grow` | ✅ | |
| `flex-shrink` | ✅ | |
| `flex-basis` | ✅ | |
| `flex` shorthand | ✅ | `flex: grow [shrink] [basis]` |
| `gap` / `row-gap` / `column-gap` | ✅ | |
| `place-items` / `place-content` | ✅ | Shorthands |

**Tests:** test_layout_flex_row.cpp, test_layout_flex_column.cpp, test_layout_flex_wrap.cpp (~135 tests)

### 2. CSS Grid Layout Level 1
**W3C:** https://www.w3.org/TR/css-grid-1/
**Status:** ⚠️ Partial

| Property | Status | Notes |
|----------|--------|-------|
| `grid-template-columns` / `grid-template-rows` | ✅ | fr, px, auto units |
| `grid-column` / `grid-row` | ✅ | start/end spans |
| `column-gap` / `row-gap` | ✅ | |
| `grid-auto-flow` | ❌ | Auto-placement direction |
| `grid-auto-rows` / `grid-auto-columns` | ❌ | Implicit track sizing |
| `grid-template-areas` | ❌ | Named areas |
| `grid-area` shorthand | ❌ | |

**Tests:** test_layout_grid.cpp (~20 tests)

### 3. CSS Box Model Level 3
**W3C:** https://www.w3.org/TR/css-box-3/
**Status:** ✅ Complete

| Property | Status | Notes |
|----------|--------|-------|
| `margin` (shorthand + individual) | ✅ | 1-4 value shorthand |
| `padding` (shorthand + individual) | ✅ | 1-4 value shorthand |
| `width` / `height` | ✅ | |
| `min-width` / `max-width` | ✅ | |
| `min-height` / `max-height` | ✅ | |
| `margin: auto` centering | ❌ | Not yet implemented |
| `box-sizing` | ⚠️ | Parsed, not enforced in layout |

### 4. CSS Backgrounds & Borders Level 3
**W3C:** https://www.w3.org/TR/css-backgrounds-3/
**Status:** ⚠️ Mostly Complete

| Property | Status | Notes |
|----------|--------|-------|
| `background-color` | ✅ | Full CSS Color L4 |
| `background-image` (gradient) | ✅ | linear-gradient with multi-stop |
| `background-image` (URL) | ❌ | Only gradients, not images |
| `background-size` | ⚠️ | CSS parsed, bridge stub |
| `background-position` | ⚠️ | CSS parsed, bridge stub |
| `background-repeat` | ⚠️ | CSS parsed, bridge stub |
| `border` (shorthand) | ✅ | width + style + color |
| `border-top` / `right` / `bottom` / `left` | ✅ | Per-side borders |
| `border-width` / `color` per-side | ✅ | |
| `border-radius` | ✅ | Single value |
| `border-top-left-radius` etc. | ✅ | Per-corner radii |
| `box-shadow` | ✅ | offset, blur, spread, color |

**Tests:** test_css_color_parser.cpp, test_css_shorthand.cpp (~80 tests)

### 5. CSS Color Level 4
**W3C:** https://www.w3.org/TR/css-color-4/
**Status:** ✅ Complete

| Format | Status |
|--------|--------|
| `#RGB` / `#RRGGBB` / `#RRGGBBAA` | ✅ |
| `rgb()` / `rgba()` | ✅ |
| `hsl()` / `hsla()` | ✅ |
| 148 named colors | ✅ |
| `transparent` | ✅ |
| `currentColor` | ⚠️ Resolves via theme |

**Tests:** test_css_color_parser.cpp (~40 tests)

### 6. CSS Values & Units Level 4
**W3C:** https://www.w3.org/TR/css-values-4/
**Status:** ✅ Mostly Complete

| Feature | Status | Notes |
|---------|--------|-------|
| `px` | ✅ | Default unit |
| `em` | ✅ | Relative to parent font-size |
| `rem` | ✅ | Relative to root font-size |
| `%` | ✅ | Relative to parent dimension |
| `vw` / `vh` / `vmin` / `vmax` | ✅ | Via getRootSize() |
| `ch` | ✅ | Approximate (0.5 × font-size) |
| `calc()` | ✅ | Full arithmetic + nested functions |
| `min()` / `max()` / `clamp()` | ✅ | With mixed units |
| `auto` | ✅ | |
| `min-content` / `max-content` / `fit-content` | ❌ | Size keywords not supported |

**Tests:** test_css_value_parser.cpp, test_css_calc.cpp (~80 tests)

### 7. CSS Transforms Level 1
**W3C:** https://www.w3.org/TR/css-transforms-1/
**Status:** ✅ Mostly Complete

| Function | Status |
|----------|--------|
| `translate(x, y)` / `translateX` / `translateY` | ✅ |
| `scale(s)` | ✅ |
| `rotate(deg)` | ✅ |
| `skew(x, y)` | ✅ |
| `transform-origin` | ✅ |
| `matrix()` | ❌ |
| 3D transforms | ❌ |

### 8. CSS Transitions Level 1
**W3C:** https://www.w3.org/TR/css-transitions-1/
**Status:** ⚠️ Partial

| Property | Status | Notes |
|----------|--------|-------|
| `transition` shorthand | ✅ | Parsed |
| `transition-duration` | ✅ | Applied |
| `transition-property` | ⚠️ | Parsed, not per-property |
| `transition-timing-function` | ⚠️ | Parsed, stored |
| `transition-delay` | ⚠️ | Parsed, stored |
| `transitionend` event | ⚠️ | Infrastructure exists |

### 9. CSS Animations Level 1
**W3C:** https://www.w3.org/TR/css-animations-1/
**Status:** ⚠️ Partial

| Property | Status | Notes |
|----------|--------|-------|
| `animation` shorthand | ✅ | Parsed |
| `animation-name` | ✅ | Parsed |
| `animation-duration` | ✅ | Parsed |
| `animation-timing-function` | ✅ | Parsed |
| `animation-delay` | ✅ | Parsed |
| `animation-iteration-count` | ✅ | Parsed (including `infinite`) |
| `animation-direction` | ✅ | Parsed |
| `animation-fill-mode` | ✅ | Parsed |
| `@keyframes` | ⚠️ | defineKeyframes infrastructure |
| `animationend` event | ⚠️ | Infrastructure exists |

**Note:** Pulp uses FrameClock-driven `animate()` bridge for actual animation execution.

### 10. CSS Text Level 3
**W3C:** https://www.w3.org/TR/css-text-3/
**Status:** ✅ Complete

| Property | Status |
|----------|--------|
| `text-align` | ✅ |
| `text-transform` | ✅ |
| `text-decoration` | ✅ |
| `text-overflow` | ✅ |
| `text-shadow` | ✅ (parsed) |
| `white-space` | ✅ |
| `word-break` / `overflow-wrap` | ✅ |
| `letter-spacing` | ✅ |
| `line-height` | ✅ |
| `font-size` / `font-weight` / `font-style` | ✅ |
| `font-family` | ✅ (parsed) |
| `-webkit-line-clamp` | ✅ (parsed) |

### 11. CSS Positioned Layout Level 3
**W3C:** https://www.w3.org/TR/css-position-3/
**Status:** ✅ Complete

| Property | Status |
|----------|--------|
| `position` (static/relative/absolute/fixed/sticky) | ✅ |
| `top` / `right` / `bottom` / `left` | ✅ |
| `z-index` | ✅ |
| `inset` shorthand | ✅ |

### 12. CSS Overflow Level 3
**W3C:** https://www.w3.org/TR/css-overflow-3/
**Status:** ✅ Complete

| Property | Status |
|----------|--------|
| `overflow` (visible/hidden/scroll/auto) | ✅ |

### 13. CSS Sizing Level 3
**W3C:** https://www.w3.org/TR/css-sizing-3/
**Status:** ⚠️ Mostly Complete

| Property | Status |
|----------|--------|
| `width` / `height` / `min-*` / `max-*` | ✅ |
| `aspect-ratio` | ✅ |
| `box-sizing` | ⚠️ Parsed |
| `min-content` / `max-content` / `fit-content` | ❌ |

### 14. CSS Filter Effects Level 1
**W3C:** https://www.w3.org/TR/filter-effects-1/
**Status:** ⚠️ Partial

| Function | Status |
|----------|--------|
| `blur(px)` | ✅ |
| `brightness()` / `contrast()` / `grayscale()` | ❌ |
| `hue-rotate()` / `invert()` / `saturate()` / `sepia()` | ❌ |
| `drop-shadow()` | ❌ |

### 15. Selectors Level 4
**W3C:** https://www.w3.org/TR/selectors-4/
**Status:** ✅ Mostly Complete

| Selector | Status |
|----------|--------|
| Type, class, ID | ✅ |
| Descendant, child (>) | ✅ |
| `:first-child` / `:last-child` | ✅ |
| `:nth-child(An+B)` / `:nth-last-child` | ✅ |
| `:only-child` | ✅ |
| `:empty` | ✅ |
| `:checked` / `:disabled` | ✅ |
| `:hover` / `:focus` / `:active` | ✅ |
| `:not(selector)` | ✅ |
| `:first-of-type` / `:last-of-type` / `:nth-of-type` | ✅ |
| Attribute selectors (`[attr]`, `[attr="val"]`, `[attr^=]`, etc.) | ✅ |
| `:valid` / `:invalid` | ❌ |
| `::before` / `::after` | ❌ |

**Tests:** test_selector_matching.cpp (~20 tests)

### 16. UI Events / DOM Living Standard
**W3C:** https://www.w3.org/TR/uievents/ + https://dom.spec.whatwg.org/
**Status:** ✅ Mostly Complete

| Feature | Status |
|---------|--------|
| `addEventListener` / `removeEventListener` | ✅ |
| Capture + bubble event flow | ✅ |
| `stopPropagation()` | ✅ |
| `stopImmediatePropagation()` | ✅ |
| `preventDefault()` | ✅ |
| `CustomEvent` constructor | ✅ |
| `click` / `mousedown` / `mouseup` | ✅ |
| `mouseenter` / `mouseleave` | ✅ |
| `dblclick` | ✅ |
| `wheel` | ✅ |
| `scroll` | ✅ |
| `contextmenu` | ✅ |
| `keydown` / `keyup` | ✅ |
| `input` / `change` | ✅ |
| `focus` / `blur` | ✅ |
| `resize` (window) | ⚠️ Via window.addEventListener |
| Pointer Events Level 2 | ✅ |
| Gesture events | ✅ |
| `createElement` / `appendChild` / `remove` | ✅ |
| `innerHTML` / `outerHTML` | ✅ |
| `closest()` / `matches()` / `contains()` | ✅ |
| `querySelector` / `querySelectorAll` | ✅ |
| `classList` (add/remove/toggle/contains/replace) | ✅ |
| `getAttribute` / `setAttribute` / `dataset` | ✅ |
| `getBoundingClientRect()` | ✅ |
| `getComputedStyle()` | ⚠️ Inline style + layout dims |
| `createDocumentFragment()` | ✅ |
| `append` / `prepend` / `before` / `after` / `replaceWith` | ✅ |
| `focus()` / `blur()` | ✅ |
| `window.matchMedia()` | ✅ |
| `window.addEventListener` | ✅ |
| `window.setTimeout` / `setInterval` | ✅ |

**Tests:** test_events_click.cpp, test_events_hover.cpp, test_events_keyboard.cpp, test_events_focus.cpp, test_events_bubbling.cpp (~81 tests)

---

## Web API Specs (Beyond CSS/DOM)

These are the 10 additional Web API specifications that frontend developers use but aren't W3C CSS/DOM specs.

### 17. HTML Canvas 2D Context
**Spec:** https://html.spec.whatwg.org/multipage/canvas.html
**Status:** ✅ Mostly Complete

| Feature | Status | Notes |
|---------|--------|-------|
| fillRect / strokeRect / clearRect | ✅ | Including rounded variants |
| fillText / set font / text align / text baseline | ✅ | |
| beginPath / moveTo / lineTo / quadTo / cubicTo / closePath | ✅ | |
| fill / stroke (current path) | ✅ | |
| arc / stroke_arc | ✅ | For pie charts, circular progress |
| save / restore | ✅ | |
| translate / scale / rotate | ✅ | |
| clipRect | ✅ | |
| setFillColor / setStrokeColor / setLineWidth | ✅ | |
| lineCap / lineJoin | ✅ | butt/round/square, miter/round/bevel |
| globalAlpha | ✅ | Per-canvas opacity |
| globalCompositeOperation | ✅ | source-over, multiply, screen, overlay |
| createLinearGradient / createRadialGradient | ✅ | With color stops |
| measureText | ⚠️ | Approximate (no Skia font shaping) |
| drawImage | ❌ | Cannot render images on canvas |
| getImageData / putImageData | ❌ | Structurally hard with command list |
| createPattern | ❌ | Needs Skia shader pattern |

### 18. Clipboard API
**Spec:** https://www.w3.org/TR/clipboard-apis/
**Status:** ✅ Complete

| Feature | Status |
|---------|--------|
| `navigator.clipboard.readText()` | ✅ |
| `navigator.clipboard.writeText()` | ✅ |

### 19. Drag and Drop API
**Spec:** https://html.spec.whatwg.org/multipage/dnd.html
**Status:** ⚠️ Partial (C++ exists, JS bridge pending)

| Feature | Status | Notes |
|---------|--------|-------|
| C++ DropTarget interface | ✅ | Platform-level drag target |
| JS dragstart/drag/drop events | ❌ | Not yet bridged to JS |
| dataTransfer object | ❌ | |

### 20. Web Storage API
**Spec:** https://html.spec.whatwg.org/multipage/webstorage.html
**Status:** ✅ Complete

| Feature | Status | Notes |
|---------|--------|-------|
| `localStorage.getItem/setItem/removeItem` | ✅ | File-backed in temp dir |
| `sessionStorage` | ✅ | Alias to localStorage |

### 21. High Resolution Time
**Spec:** https://www.w3.org/TR/hr-time-3/
**Status:** ✅ Complete

| Feature | Status | Notes |
|---------|--------|-------|
| `performance.now()` | ✅ | std::chrono::steady_clock, sub-ms precision |

### 22. Console Standard
**Spec:** https://console.spec.whatwg.org/
**Status:** ✅ Mostly Complete

| Feature | Status |
|---------|--------|
| `console.log/info/warn/error/debug` | ✅ |
| `console.time/timeEnd` | ✅ |
| `console.table` | ❌ |

### 23. Fetch Standard
**Spec:** https://fetch.spec.whatwg.org/
**Status:** ✅ Basic (via curl)

| Feature | Status | Notes |
|---------|--------|-------|
| `fetch(url, opts)` | ✅ | Minimal: GET/POST via curl, returns {text(), json()} |
| Request/Response objects | ❌ | Simplified interface |
| AbortController | ❌ | |

### 24. Encoding Standard
**Spec:** https://encoding.spec.whatwg.org/
**Status:** ✅ Complete

| Feature | Status |
|---------|--------|
| `TextEncoder` (encode to UTF-8) | ✅ |
| `TextDecoder` (decode from UTF-8) | ✅ |
| `atob` / `btoa` (base64) | ✅ |

### 25. Web Cryptography API
**Spec:** https://www.w3.org/TR/WebCryptoAPI/
**Status:** ⚠️ Minimal

| Feature | Status | Notes |
|---------|--------|-------|
| `crypto.getRandomValues()` | ✅ | Math.random() based (not cryptographic) |
| SubtleCrypto | ❌ | Not needed for plugin UIs |

### 26. Structured Clone / Web IDL
**Spec:** https://html.spec.whatwg.org/multipage/structured-data.html
**Status:** ✅ Complete

| Feature | Status | Notes |
|---------|--------|-------|
| `structuredClone()` | ✅ | Via JSON round-trip |

---

## Deliberately Out of Scope

These W3C features are intentionally not implemented because they don't apply to plugin UIs:

- **CSS Cascade** — Rules apply in source order, no specificity scoring
- **Shadow DOM / Web Components** — Not needed for plugin UIs
- **CSS @import / @supports / @layer** — Single-file JS scripts
- **CSS Houdini** — Paint worklets, layout API
- **SVG elements** — Canvas 2D API covers drawing needs
- **Full HTML parsing** — innerHTML handles common nested patterns
- **Form submission** — No network requests from plugin UIs
- **Service Workers / IndexedDB** — No offline storage
- **3D Transforms** — 2D transforms cover plugin UI needs
- **CSS Container Queries** — matchMedia covers responsive needs

---

## Test Summary

| Test Suite | Tests | What It Covers |
|-----------|-------|----------------|
| web-compat-parser | 157 | CSS values, colors, shorthand, calc |
| web-compat-layout | 135 | Flex row/col/wrap, grid, position, nested |
| web-compat-events | 81 | Click, hover, keyboard, focus, bubbling, selectors |
| web-compat-reftest | 23 | Visual screenshot regression |
| web-compat-fixture | 23 | Screenshot + integration fixtures |
| platform-maturity | 17 | Cursor, focus, IME, accessibility |
| widget-bridge | ~50 | Bridge function correctness |
| **Total web-compat** | **~486** | |
| **Full suite** | **1247** | All subsystems |
