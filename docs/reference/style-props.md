# Style props reference

All bridge functions exposed by `WidgetBridge` (`core/view/src/widget_bridge.cpp`)
that map directly to React Native / CSS style props. Issue
[#1026](https://github.com/danielraffel/pulp/issues/1026) adds RN-shaped
counterparts to the existing CSS-shaped primitives so `@pulp/react`'s
prop-applier can route JSX style props onto Pulp Views without an
intermediate translation layer.

**Status:** experimental.

The two consumer-facing surfaces are:

- **`@pulp/react`** — the React renderer. Its prop-applier translates JSX
  `style={{ ... }}` props directly into the bridge calls listed below.
- **`@pulp/css-adapt`** — the browser-CSS shim. It translates browser-style
  selectors and longhand CSS into the same bridge calls.

The native-side primitives (`View::set_*` methods) are documented in
`core/view/include/pulp/view/view.hpp`. This page is the contract between
JS callers and the bridge.

## Layout

| Prop | Bridge function | Notes |
|---|---|---|
| `flexDirection` | `setFlex(id, "direction", "row" \| "column" \| ...)` | |
| `flexWrap` | `setFlex(id, "flex_wrap", true \| false)` | |
| `flexGrow` | `setFlex(id, "flex_grow", n)` | |
| `flexShrink` | `setFlex(id, "flex_shrink", n)` | |
| `flexBasis` | `setFlex(id, "flex_basis", n)` | |
| `order` | `setFlex(id, "order", n)` | |
| `gap` | `setFlex(id, "gap", n)` | Also `row_gap` / `column_gap`. |
| `padding` | `setFlex(id, "padding", n)` | Also `padding_{top,right,bottom,left}`. |
| `margin` | `setFlex(id, "margin_{t,r,b,l}", n)` | Per-side only. |
| `width` / `minWidth` / `maxWidth` | `setFlex(id, "width" \| "min_width" \| "max_width", n)` | |
| `height` / `minHeight` / `maxHeight` | `setFlex(id, "height" \| "min_height" \| "max_height", n)` | |
| `alignItems` | `setFlex(id, "align_items", "start" \| "center" \| "end" \| "stretch")` | |
| `alignSelf` | `setFlex(id, "align_self", ...)` | |
| `justifyContent` | `setFlex(id, "justify_content", "start" \| "center" \| "end" \| "space-between" \| "space-around" \| "space-evenly")` | |
| `display: grid` | `setGrid(id, columns, rows, columnGap, rowGap)` | Container; children use `setGrid(id, ...)` for placement. |
| `gridColumn` / `gridRow` | `setGrid(id, "column_start"/"column_end"/"row_start"/"row_end", n)` | |
| `position` | `setPosition(id, "static" \| "relative" \| "absolute" \| "fixed" \| "sticky")` | |
| `top` / `right` / `bottom` / `left` | `setTop(id, n)` / `setRight(id, n)` / `setBottom(id, n)` / `setLeft(id, n)` | |
| `zIndex` | `setZIndex(id, n)` | |
| `overflow` | `setOverflow(id, "visible" \| "hidden")` | |
| `display` | `setVisible(id, true \| false)` and `setVisibility(id, "visible" \| "hidden")` | `setVisible` removes from layout (`display:none`); `setVisibility` only hides. |

## Background

| Prop | Bridge function | Notes |
|---|---|---|
| `backgroundColor` | `setBackground(id, color)` | Accepts hex, `rgb()`, `rgba()`, `hsl()`. |
| `background: linear-gradient(...)` | `setBackgroundGradient(id, "linear-gradient(to right, red, blue)")` | CSS gradient string. |
| `opacity` | `setOpacity(id, 0..1)` | Layer alpha. |

## Border

| Prop | Bridge function | Notes |
|---|---|---|
| `border` (shorthand) | `setBorder(id, color, width, radius)` | All four at once. |
| `borderColor` | `setBorderColor(id, color)` | pulp #1026. Standalone setter; flips `has_border_` on. |
| `borderWidth` | `setBorderWidth(id, width)` | pulp #1026. |
| `borderRadius` | `setBorderRadius(id, radius)` | pulp #1026. Uniform corner radius. |
| `borderTopLeftRadius` | `setBorderTopLeftRadius(id, n)` or `setCornerRadius(id, "TopLeft", n)` | pulp #1026. |
| `borderTopRightRadius` | `setBorderTopRightRadius(id, n)` or `setCornerRadius(id, "TopRight", n)` | pulp #1026. |
| `borderBottomLeftRadius` | `setBorderBottomLeftRadius(id, n)` or `setCornerRadius(id, "BottomLeft", n)` | pulp #1026. |
| `borderBottomRightRadius` | `setBorderBottomRightRadius(id, n)` or `setCornerRadius(id, "BottomRight", n)` | pulp #1026. |
| `borderTopColor` | `setBorderTopColor(id, color)` | pulp #1026. |
| `borderRightColor` | `setBorderRightColor(id, color)` | pulp #1026. |
| `borderBottomColor` | `setBorderBottomColor(id, color)` | pulp #1026. |
| `borderLeftColor` | `setBorderLeftColor(id, color)` | pulp #1026. |
| `borderTopWidth` | `setBorderTopWidth(id, n)` | pulp #1026. |
| `borderRightWidth` | `setBorderRightWidth(id, n)` | pulp #1026. |
| `borderBottomWidth` | `setBorderBottomWidth(id, n)` | pulp #1026. |
| `borderLeftWidth` | `setBorderLeftWidth(id, n)` | pulp #1026. |
| `border-{top,right,bottom,left}` (CSS shorthand) | `setBorderSide(id, "top" \| "right" \| "bottom" \| "left", width, color)` | Both at once. |

## Shadow

| Prop | Bridge function | Notes |
|---|---|---|
| `box-shadow` (CSS) | `setBoxShadow(id, offsetX, offsetY, blur, spread, color, inset?)` | Pulp's CSS-shaped primitive (#925). Carries spread + inset. |
| RN `shadowColor` / `shadowOffset` / `shadowOpacity` / `shadowRadius` | `setShadow(id, color, offsetX, offsetY, opacity, radius)` | pulp #1026. RN-shaped; lowers onto `setBoxShadow` with spread=0, inset=false; opacity multiplies into color alpha. |
| (clear shadow) | `clearBoxShadow(id)` | |

## Transforms

| Prop | Bridge function | Notes |
|---|---|---|
| `transform: translate(x, y)` | `setTranslate(id, x, y)` | |
| `transform: scale(s)` | `setScale(id, s)` | |
| `transform: rotate(deg)` | `setRotation(id, deg)` | |
| `transform: matrix(a, b, c, d, e, f)` | `setTransform(id, a, b, c, d, e, f)` | Full 2D affine (#930). Composes onto parent transform. |
| (clear matrix transform) | `clearTransform(id)` | Reverts to scalar transforms. |
| `transformOrigin` | `setTransformOrigin(id, x, y)` | Normalized 0..1; `0.5,0.5` = center. Applies to BOTH the CSS-transform path AND the matrix path (#1026). |

## Filters

| Prop | Bridge function | Notes |
|---|---|---|
| `filter: blur(Npx)` | `setFilter(id, "blur(Npx)")` | Per-element blur via compositing layer. |
| `backdrop-filter: blur(Npx)` | `setBackdropFilter(id, radius)` | Frosted-glass blur of content behind (#926). |

## Pointer / interaction

| Prop | Bridge function | Notes |
|---|---|---|
| RN `pointerEvents` | `setPointerEvents(id, "auto" \| "none" \| "box-only" \| "box-none")` | pulp #1026. 4-valued enum, matches RN contract. |
| `cursor` | `setCursor(id, "default" \| "pointer" \| "crosshair" \| "text" \| "grab")` | |
| RN `backfaceVisibility` | `setBackfaceVisibility(id, "visible" \| "hidden")` | pulp #1026. Plumbed; no-op for paint until 3D transforms land. |
| (CSS-style hit toggling) | `setEnabled(id, bool)` | `:disabled` equivalent — blocks input, fades opacity. |

## Text

| Prop | Bridge function | Notes |
|---|---|---|
| `color` | `setTextColor(id, color)` | On `Label`: explicit color. On a container: cascade. |
| `font-family` | `setFontFamily(id, name)` | |
| `font-weight` | `setFontWeight(id, n)` | |
| `font-style` | `setFontStyle(id, "normal" \| "italic")` | |
| `font-size` | `setFontSize(id, n)` | |
| `letter-spacing` | `setLetterSpacing(id, n)` | |
| `line-height` | `setLineHeight(id, n)` | |
| `text-align` | `setTextAlign(id, "left" \| "center" \| "right")` | |
| `text-transform` | `setTextTransform(id, "uppercase" \| "lowercase" \| "capitalize" \| "none")` | |
| `text-decoration` | `setTextDecoration(id, "none" \| "underline" \| "line-through")` | |
| `text-overflow` | `setTextOverflow(id, "ellipsis" \| "clip")` | |
| `white-space` | `setWhiteSpace(id, "normal" \| "nowrap" \| "pre" \| "pre-wrap")` | |
| `user-select` | `setUserSelect(id, "none" \| "text" \| "all")` | Currently a no-op stub. |
| `placeholder` (text input) | `setPlaceholder(id, text)` | |

## Animation / transition

| Prop | Bridge function | Notes |
|---|---|---|
| `transition-duration` | `setTransitionDuration(id, seconds)` | |
| `@keyframes` | `defineKeyframes(name, [{offset, value}, ...])` | Stub today. |
| `animation` | `setAnimation(id, prop, duration, iterations, direction, keyframes)` | Stub today. |

## Theming hooks

| Prop | Bridge function | Notes |
|---|---|---|
| Theme tokens (color) | `setColorToken(name, color)` | Sets a theme color token; resolved via `View::resolve_color`. |
| Theme tokens (dimension) | `setDimensionToken(name, n)` | |
| Apply panel theme | `setPanelStyle(id, bgToken, borderToken, radius, width)` | |
| Theme JSON (whole) | `setTheme(id, json)`, `getThemeJson()`, `applyTokenDiff(id, diffJson)` | |
| Design tokens import / export | `importDesignTokens(json)` / `exportDesignTokens()` | |
| Per-state style | `setStateStyle(id, state, prop, value)` | `:hover`, `:active`, `:focus`. |
| Per-widget style | `setWidgetStyle(id, json)` | |

## Not yet implemented

Style props that exist in the React Native or CSS surface area but do
not yet have a bridge primitive in Pulp. These are the to-do list for
future rounds. Cite issue [#1026](https://github.com/danielraffel/pulp/issues/1026)
when filing follow-ups.

| Prop | Notes |
|---|---|
| `aspectRatio` | RN style prop; pulp's `FlexStyle` does not yet model an aspect-ratio constraint. |
| `direction: rtl` | Bidirectional layout. |
| `tintColor` (RN Image) | Image tinting. |
| `resizeMode` (RN Image) | Image fit mode. |
| `transform: perspective(N)` and `rotateX/Y` | 3D transforms; `setBackfaceVisibility` is plumbed but inert until this lands. |
| `elevation` (RN Android) | Material elevation; RN's lowering varies per platform. |
| `boxSizing` | `border-box` vs `content-box`. |
| `outline` / `outline-offset` | Outline ring distinct from `border`. |
| `mask-image` / `clip-path` | Per-pixel masking. |
| `mix-blend-mode` (View-level) | Canvas already supports per-draw blend modes via `canvasSetBlendMode`. |
| `caret-color` | Text-input caret tint. |

## See also

- `@pulp/react` — JSX style-prop applier translates style props into the
  bridge calls in this document.
- `@pulp/css-adapt` — translates browser-CSS into the same bridge calls.
- React Native's [View Style Props](https://reactnative.dev/docs/view-style-props)
  and [Text Style Props](https://reactnative.dev/docs/text-style-props) for
  the consumer-facing surface.
- [JS Bridge API Reference](js-bridge.md) for the full bridge function
  catalog, including non-style-prop primitives (createKnob, registerClick, etc.).
