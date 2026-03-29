# Phase 8 — Remaining Web-Compat Gaps (Definitive)

**Version:** 2026-03-28
**Status:** Proposed — bugs filed
**Depends on:** Phase 7 (complete)
**Goal:** Close every gap a frontend developer would hit bringing a Figma design to Pulp.

---

## Methodology

This list was produced by a systematic property-by-property audit of the 81 CSS properties in our `CSSStyleDeclaration`, all 84 bridge functions, the full selector engine, event system, and DOM API — compared against the W3C specs that matter for UI development:
- CSS Flexbox L1, Grid L1, Box Model L3, Backgrounds & Borders L3
- CSS Sizing L3, Transitions L1, Animations L1, Transforms L1
- CSS Text L3, Color L4, Values & Units L4, Filter Effects L1
- Selectors L4, DOM Living Standard, UI Events, Pointer Events L2

**This is intended to be exhaustive.** If it's not on this list, it's either implemented or deliberately out of scope.

---

## Tier 1: Events (blocks UI state management)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 1 | **transitionend / transitionstart events not fired** | Cannot sequence "fade out → remove" or "slide in → enable interaction" | Bridge: fire when FrameClock animation completes for a transitioning property |
| 2 | **animationend / animationstart events not fired** | Cannot detect keyframe animation completion | Bridge: fire when keyframe animation reaches end |
| 3 | **wheel event missing** | Cannot build custom scroll, zoom wheels, rotary controls | Bridge: wire scrollWheel as 'wheel' with deltaX/deltaY/deltaMode |
| 4 | **scroll event missing** | Cannot implement infinite scroll, sticky headers, scroll-driven effects | Bridge: fire on ScrollView position change |
| 5 | **resize event missing** | Cannot respond to window/container resize | Bridge: fire on root bounds change; consider ResizeObserver |
| 6 | **dblclick event missing** | Cannot implement double-click-to-edit, double-click-to-zoom | Bridge: fire on click_count == 2 |
| 7 | **stopImmediatePropagation() missing** | Breaks event delegation patterns where multiple listeners exist on same element | JS: add _stoppedImmediate flag, check in _fireListeners loop |
| 8 | **CustomEvent constructor missing** | Cannot dispatch custom events between components | JS: CustomEvent(type, {detail}) constructor |

## Tier 2: Borders & Backgrounds (breaks design fidelity)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 9 | **No per-side border** (border-top, border-right, etc.) | Cannot style e.g. bottom border only, tab underlines | View: add border_top/right/bottom/left fields; bridge: setBorderSide |
| 10 | **No per-corner border-radius** (border-top-left-radius, etc.) | Cannot do pill shapes, chat bubbles, asymmetric cards | View: float corner_radii_[4]; bridge: setCornerRadii |
| 11 | **background-size/position not wired** | CSS cases exist in style-decl.js but no native handler | Bridge: add setBackgroundSize/setBackgroundPosition to widget_bridge |
| 12 | **No background-repeat** | Cannot tile background patterns | View: add background_repeat enum |
| 13 | **No background-clip: text** | Cannot do gradient text effects | Needs canvas-level text mask |
| 14 | **No text-shadow rendering** | CSS parsed (style-decl.js:431) but no native paint | Label paint: add shadow draw before text |

## Tier 3: Layout keywords & shorthands

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 15 | **margin: auto centering not implemented** | Most common CSS centering pattern doesn't work | layout_children: detect auto margin, distribute remaining space |
| 16 | **No min-content / max-content / fit-content** | Cannot size to content (common in responsive design) | FlexStyle: add intrinsic size keywords, resolve in layout |
| 17 | **No box-sizing: border-box** | Padding adds to width instead of subtracting from it | FlexStyle: add box_sizing field, adjust layout |
| 18 | **No flex-flow shorthand** | Must set flex-direction + flex-wrap separately | Style decl: parse "row wrap" → set both |
| 19 | **No grid-gap shorthand** | Must set row-gap + column-gap separately | Style decl: parse "10px 20px" → set both |
| 20 | **No place-items / place-content shorthands** | Must set align-* + justify-* separately | Style decl: parse shorthands |

## Tier 4: Transitions & Animations (incomplete infrastructure)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 21 | **Transitions only store duration** — timing function, delay, per-property not executed | CSS transitions are decorative only | Bridge: implement actual interpolation on property change |
| 22 | **@keyframes only stubbed** — defineKeyframes is a no-op | CSS animation property doesn't animate | Bridge: implement keyframe storage + playback in FrameClock |
| 23 | **animation-* properties not in style decl** | Cannot set animation via CSS | Style decl: add animation, animationName, animationDuration, etc. |

## Tier 5: Selectors & DOM (minor gaps)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 24 | **No attribute selectors** ([data-x], [attr="val"]) | Cannot select by data attributes | Selector engine: match against element._attributes |
| 25 | **No :nth-of-type / :first-of-type / :last-of-type** | Cannot select "every other div" (only every other child) | Selector engine: filter by tag before indexing |
| 26 | **No createDocumentFragment()** | Cannot batch-insert without multiple reflows | JS: lightweight container that transfers children |
| 27 | **No <option> DOM support in <select>** | Cannot build dynamic selects by appending option children | JS: observe select children, sync to setItems |
| 28 | **getComputedStyle reads inline style only** | Reports what was set, not what was computed by layout | Bridge: query resolved values from native view |

## Tier 6: Filters (limited to blur)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 29 | **Only filter:blur() implemented** — no brightness, contrast, grayscale, hue-rotate, invert, saturate, sepia, drop-shadow | Limited visual effects | Skia-dependent: requires GPU filter pipeline per-element |

## Tier 7: Nice-to-have (low priority)

| # | Bug | Impact | Scope |
|---|-----|--------|-------|
| 30 | **No CSS logical properties** (margin-inline, padding-block, etc.) | RTL layout support | Style decl: alias logical → physical based on direction |
| 31 | **No ::before / ::after pseudo-elements** | Cannot use CSS-generated decorative content | Would need virtual child views — complex |
| 32 | **No line-clamp / -webkit-line-clamp** | Cannot truncate multi-line text with ellipsis | Label: add max_lines property |
| 33 | **No CSS container queries** | Modern responsive design at container level | matchMedia covers most cases |
| 34 | **No ResizeObserver / IntersectionObserver / MutationObserver** | Advanced DOM observation patterns | JS: implement via layout polling |
| 35 | **No font-family rendering** — CSS parsed but font selection not applied | All text renders in default font | Label/TextEditor: add font family storage + Skia font resolution |
| 36 | **No window.addEventListener for global events** | Cannot listen for global keyboard without focus | Bridge: route all key events through window listeners |

---

## Scope Estimate

| Tier | Items | Estimated Lines | Priority |
|------|-------|----------------|----------|
| 1 — Events | 8 | ~200 JS + ~150 C++ | High |
| 2 — Borders/BG | 6 | ~100 C++ + ~50 JS | High |
| 3 — Layout | 6 | ~200 C++ (layout engine) | Medium |
| 4 — Transitions/Animations | 3 | ~300 C++ (FrameClock) | Medium |
| 5 — Selectors/DOM | 5 | ~150 JS | Medium |
| 6 — Filters | 1 | ~100 C++ (Skia) | Low |
| 7 — Nice-to-have | 7 | ~300 mixed | Low |
| **Total** | **36** | **~1550 lines** | |

---

## What's Deliberately Out of Scope

- Full HTML parser (innerHTML handles common nested patterns, not arbitrary HTML)
- CSS cascade/specificity (source-order rules by design)
- Shadow DOM, Web Components, custom elements
- Service Workers, IndexedDB, fetch(), XMLHttpRequest
- SVG elements (Canvas 2D covers drawing)
- CSS Houdini (paint worklets, layout API)
- Full form semantics (form submission, fieldset/legend, constraint validation)
- 3D transforms (rotateX/Y/Z, perspective)
- CSS matrix() transform
- CSS @supports, @layer, @import
