# Style props reference

All bridge functions exposed by `WidgetBridge` that map directly to React
Native / CSS style props. RN-shaped bridge counterparts sit next to the
CSS-shaped primitives so `@pulp/react`'s prop-applier can route JSX style props
onto Pulp Views without an intermediate translation layer.

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
| `flexWrap` | `setFlex(id, "flex_wrap", "nowrap" \| "wrap" \| "wrap-reverse")` | |
| `flexGrow` | `setFlex(id, "flex_grow", n)` | |
| `flexShrink` | `setFlex(id, "flex_shrink", n)` | |
| `flexBasis` | `setFlex(id, "flex_basis", n)` | |
| `aspectRatio` | `setFlex(id, "aspect_ratio", n)` | Positive finite ratio; `0` or non-finite values clear the constraint. |
| `order` | `setFlex(id, "order", n)` | |
| `gap` | `setFlex(id, "gap", n)` | Also `row_gap` / `column_gap`. |
| `padding` | `setFlex(id, "padding", n)` | Also `padding_{top,right,bottom,left}`. |
| `margin` | `setFlex(id, "margin", n)` | Also `margin_{top,right,bottom,left}`. |
| `width` / `minWidth` / `maxWidth` | `setFlex(id, "width" \| "min_width" \| "max_width", n)` | |
| `height` / `minHeight` / `maxHeight` | `setFlex(id, "height" \| "min_height" \| "max_height", n)` | |
| `alignItems` | `setFlex(id, "align_items", "start" \| "center" \| "end" \| "stretch" \| "baseline")` | |
| `alignSelf` | `setFlex(id, "align_self", ...)` | |
| `justifyContent` | `setFlex(id, "justify_content", "start" \| "center" \| "end" \| "space-between" \| "space-around" \| "space-evenly")` | |
| `display: grid` | `createGrid(id, parentId)`, then `setGrid(id, key, value)` | Container; keys include `template_columns`, `template_rows`, `column_gap`, `row_gap`, `gap`, `auto_flow`, and `template_areas`. |
| `gridColumn` / `gridRow` | `setGrid(id, "column_start"/"column_end"/"row_start"/"row_end", n)` | |
| `position` | `setPosition(id, "static" \| "relative" \| "absolute" \| "fixed" \| "sticky")` | |
| `top` / `right` / `bottom` / `left` | `setTop(id, n)` / `setRight(id, n)` / `setBottom(id, n)` / `setLeft(id, n)` | |
| `zIndex` | `setZIndex(id, n)` | |
| `direction` / RN `writingDirection` | `setDirection(id, "ltr" \| "rtl" \| "auto")` | Yoga and text shaping honor the writing direction. |
| `boxSizing` | `setBoxSizing(id, "content-box" \| "border-box" \| "inherit")` | Stored on layout style and routed through Yoga sizing. |
| `overflow` | `setOverflow(id, "visible" \| "hidden" \| "scroll")` | |
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
| `borderColor` | `setBorderColor(id, color)` | Mutates only the color slot and enables border paint. |
| `borderWidth` | `setBorderWidth(id, width)` | Mutates only the width slot and enables border paint. |
| `borderRadius` | `setBorderRadius(id, radius)` | Uniform corner radius; accepts numbers and percent strings. |
| `borderTopLeftRadius` | `setBorderTopLeftRadius(id, n)` or `setCornerRadius(id, "TopLeft", n)` | Mutates one corner radius; accepts numbers and percent strings. |
| `borderTopRightRadius` | `setBorderTopRightRadius(id, n)` or `setCornerRadius(id, "TopRight", n)` | Mutates one corner radius; accepts numbers and percent strings. |
| `borderBottomLeftRadius` | `setBorderBottomLeftRadius(id, n)` or `setCornerRadius(id, "BottomLeft", n)` | Mutates one corner radius; accepts numbers and percent strings. |
| `borderBottomRightRadius` | `setBorderBottomRightRadius(id, n)` or `setCornerRadius(id, "BottomRight", n)` | Mutates one corner radius; accepts numbers and percent strings. |
| `borderTopColor` | `setBorderTopColor(id, color)` | Preserves that edge's width. |
| `borderRightColor` | `setBorderRightColor(id, color)` | Preserves that edge's width. |
| `borderBottomColor` | `setBorderBottomColor(id, color)` | Preserves that edge's width. |
| `borderLeftColor` | `setBorderLeftColor(id, color)` | Preserves that edge's width. |
| `borderTopWidth` | `setBorderTopWidth(id, n)` | Preserves that edge's color. |
| `borderRightWidth` | `setBorderRightWidth(id, n)` | Preserves that edge's color. |
| `borderBottomWidth` | `setBorderBottomWidth(id, n)` | Preserves that edge's color. |
| `borderLeftWidth` | `setBorderLeftWidth(id, n)` | Preserves that edge's color. |
| `border-{top,right,bottom,left}` (CSS shorthand) | `setBorderSide(id, "top" \| "right" \| "bottom" \| "left", width, color)` | Both at once. |
| `borderStyle` | `setBorderStyle(id, style)` | `dashed` and `dotted` render with dash effects; other named styles currently degrade to solid. |
| `outline` | `setOutlineColor(id, color)`, `setOutlineWidth(id, n)`, `setOutlineStyle(id, style)` | Paints outside the border box and does not affect Yoga layout. |
| `outline-offset` | `setOutlineOffset(id, n)` | Gap between the border edge and outline stroke. |

## Shadow

| Prop | Bridge function | Notes |
|---|---|---|
| `box-shadow` (CSS) | `setBoxShadow(id, offsetX, offsetY, blur, spread, color, inset?)` | CSS-shaped primitive; carries spread + inset. |
| RN `shadowColor` / `shadowOffset` / `shadowOpacity` / `shadowRadius` | `setShadowColor(id, color)`, `setShadowOffset(id, x, y)`, `setShadowOpacity(id, opacity)`, `setShadowRadius(id, radius)` | RN iOS-legacy longhands. Each setter mutates one `BoxShadow` field without clobbering the others; modern RN-style `boxShadow` routes through `setBoxShadow`. |
| RN consolidated shadow bridge | `setShadow(id, color, offsetX, offsetY, opacity, radius)` | Legacy aggregate helper; lowers onto `BoxShadow` with spread `0` and `inset=false`. |
| RN `elevation` | `setElevation(id, n)` | Material-style single-shadow approximation; `0` clears the shadow. |
| (clear shadow) | `clearBoxShadow(id)` | |

## Transforms

| Prop | Bridge function | Notes |
|---|---|---|
| `transform: translate(x, y)` | `setTranslate(id, x, y)` | |
| `transform: scale(s)` | `setScale(id, s)` | |
| `transform: rotate(deg)` | `setRotation(id, deg)` | |
| `transform: skewX(a)` / `skewY(b)` | `setSkew(id, xDeg, yDeg)` | Both axes are accumulated into one bridge call. |
| `transform: matrix(a, b, c, d, e, f)` | `setTransform(id, a, b, c, d, e, f)` | Full 2D affine. Composes onto parent transform at paint time; layout and hit-testing keep the untransformed bounds. |
| (clear matrix transform) | `clearTransform(id)` | Reverts to scalar transforms. |
| `transformOrigin` | `setTransformOrigin(id, x, y)` | Normalized 0..1; `0.5,0.5` = center. Scalar transforms use the current origin, defaulting to center; the matrix path uses the origin only after callers set it explicitly. |

## Filters

| Prop | Bridge function | Notes |
|---|---|---|
| `filter` | `setFilter(id, cssFilterString)` | Parses filter chains including blur, brightness, contrast, grayscale, invert, opacity, saturate, sepia, hue-rotate, and drop-shadow; CoreGraphics falls back to blur-only. |
| `backdrop-filter: blur(Npx)` | `setBackdropFilter(id, radius)` | Frosted-glass blur of content behind the view. |
| `clip-path: path("...")` | `setClipPath(id, svgPathD)` | Path form clips via SVG path data. Shape functions and URL refs are accepted but currently clear the clip. |
| `mask-image` / `mask` | `setMaskImage(id, value)`, `setMask(id, value)` | Partial; Skia composites masks, while CoreGraphics and recording backends fall back to a plain layer. |
| `mix-blend-mode` / RN `mixBlendMode` | `setMixBlendMode(id, keyword)` | W3C blend-mode keywords map to the View compositing layer; unknown keywords fall back to normal. |

## Pointer / interaction

| Prop | Bridge function | Notes |
|---|---|---|
| RN `pointerEvents` | `setPointerEvents(id, "auto" \| "none" \| "box-only" \| "box-none")` | Four-valued enum matching RN's hit-test contract. |
| `cursor` | `setCursor(id, "default" \| "pointer" \| "crosshair" \| "text" \| "grab")` | |
| RN `backfaceVisibility` | `setBackfaceVisibility(id, "visible" \| "hidden")` | Stored for bridge parity; paint-time no-op while Pulp's transform model is 2D-affine. |
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
| `text-align` | `setTextAlign(id, "auto" \| "left" \| "center" \| "right" \| "justify")` | |
| `text-transform` | `setTextTransform(id, "uppercase" \| "lowercase" \| "capitalize" \| "none")` | |
| `text-decoration` | `setTextDecoration(id, "none" \| "underline" \| "line-through")` | |
| `text-overflow` | `setTextOverflow(id, "ellipsis" \| "clip")` | |
| `white-space` | `setWhiteSpace(id, "normal" \| "nowrap" \| "pre" \| "pre-wrap" \| "pre-line" \| "break-spaces")` | |
| `user-select` | `setUserSelect(id, "auto" \| "none" \| "text" \| "all" \| "contain")` | Stored on the View for selection-aware widgets; unsupported keywords fall back to `auto`. |
| `placeholder` (text input) | `setPlaceholder(id, text)` | |

## Animation / transition

| Prop | Bridge function | Notes |
|---|---|---|
| `transition-duration` | `setTransitionDuration(id, seconds)` | |
| `@keyframes` | `defineKeyframes(name, stopsJson)` | JSON stops use `{ offset, properties: { ... } }`; stores entries in the application-wide keyframe registry. |
| `animation` | `setAnimation(id, animationName, duration, iterations, direction)` plus longhand control tokens | Seeds active animations from registered keyframes; longhands use `setAnimation(id, "duration" \| "delay" \| "easing" \| ..., value)`. |

## Theming hooks

| Prop | Bridge function | Notes |
|---|---|---|
| Theme tokens (color) | `setColorToken(name, color)` | Sets a theme color token; resolved via `View::resolve_color`. |
| Theme tokens (dimension) | `setDimensionToken(name, n)` | |
| Apply panel theme | `setPanelStyle(id, bgToken, borderToken, radius, width)` | |
| Theme preset / overrides | `setTheme(name)`, `getThemeJson()`, `applyTokenDiff(jsonString)` | `setTheme` accepts built-in theme names such as `dark`, `light`, and `pro_audio`; `applyTokenDiff` applies JSON overrides to the current theme. |
| Design tokens import / export | `importDesignTokens(json)` / `exportDesignTokens()` | |
| Per-state style | `setStateStyle(id, state, prop, value)` | `:hover`, `:active`, `:focus`. |
| Per-widget style | `setWidgetStyle(id, mode)` | Switches render style for supported widgets; modes include `standard`, `minimal`, and `silver`. |

## Deferred / Partial

Style props that exist in the React Native or CSS surface area but either do not
yet have a bridge primitive in Pulp or have only storage/subset support.

| Prop | Notes |
|---|---|
| `tintColor` (RN Image) | Image tinting. |
| `resizeMode` (RN Image) | Image fit mode. |
| `transform: perspective(N)` and `rotateX/Y` | 3D transforms; `setBackfaceVisibility` is plumbed but inert until this lands. |
| shape-function `clip-path` and URL clips | `circle()`, `ellipse()`, `inset()`, `polygon()`, `rect()`, `xywh()`, and `url()` are deferred; `path("...")` is supported. |
| full cross-backend mask compositing | `mask` and `mask-image` composite on Skia; CoreGraphics and recording backends fall back to a plain layer. |
| `caret-color` | Text-input caret tint. |

## See also

- `@pulp/react` — JSX style-prop applier translates style props into the
  bridge calls in this document.
- `@pulp/css-adapt` — translates browser-CSS into the same bridge calls.
- React Native's [View Style Props](https://reactnative.dev/docs/view-style-props)
  and [Text Style Props](https://reactnative.dev/docs/text-style-props) for
  the consumer-facing surface.
- [JS Bridge API Reference](js-bridge.md) for the author-facing bridge overview,
  including non-style-prop primitives (createKnob, registerClick, etc.). Use
  `core/view/src/widget_bridge_api_manifest.tsv` for the exhaustive native
  registration list.
