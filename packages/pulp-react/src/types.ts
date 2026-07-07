// Public TypeScript types for @pulp/react.
//
// These types describe the prop shapes consumers see when authoring JSX
// against the @pulp/react intrinsics. They mirror what setFlex /
// setBackground / setBorder / etc. accept on the bridge — one prop per
// setter, named so it reads as JSX-natural without hiding the bridge
// name when you need to dig deeper.

import type { ReactNode, Key } from 'react';

// ── Flex / layout ───────────────────────────────────────────────────
// One prop per setFlex(id, key, value) key. Pulp's setFlex covers the
// full Yoga API; we mirror the same names with camelCase TS conventions.
export type FlexDirection = 'row' | 'col';
export type FlexAlign = 'start' | 'center' | 'end' | 'stretch';
export type FlexAlignSelf = 'start' | 'center' | 'end' | 'stretch' | 'auto';
export type FlexJustify = 'start' | 'center' | 'end' | 'space-between' | 'space-around' | 'space-evenly';
/// Controls multi-line flex cross-axis distribution. Yoga supports this
/// natively. Accepts bare and prefixed CSS / RN spellings
/// (`flex-start` / `flex-end`) plus the three space-* distributions
/// that only make sense on align-content, not align-items / align-self.
export type FlexAlignContent =
    | 'start' | 'flex-start'
    | 'center'
    | 'end' | 'flex-end'
    | 'stretch'
    | 'space-between' | 'space-around' | 'space-evenly';

export interface FlexProps {
    direction?: FlexDirection;
    gap?: number;
    rowGap?: number;
    columnGap?: number;
    /// `padding` shorthand accepts either a number (px) or a CSS-spec
    /// string with 1-4 space-separated tokens (`'5%'`,
    /// `'10px 20px'`, `'10 20 30 40'`). String values fan out to the
    /// per-edge bridge keys; numeric values flow through the bridge
    /// `padding` shorthand key as a single call.
    padding?: number | string;
    /// Per-edge padding accepts either a number (px) or a percent string
    /// (`'5%'` resolves against parent main-axis size). Yoga padding does
    /// NOT support 'auto'.
    paddingTop?: number | string;
    paddingRight?: number | string;
    paddingBottom?: number | string;
    paddingLeft?: number | string;
    /// `paddingHorizontal` fans out to `paddingLeft` + `paddingRight`.
    /// Accepts number (px) or percent string (`'5%'`). Yoga padding has
    /// no 'auto' API.
    paddingHorizontal?: number | string;
    /// `paddingVertical` fans out to `paddingTop` + `paddingBottom`.
    paddingVertical?: number | string;
    /// `margin` shorthand accepts either a number (px) or a CSS-spec
    /// string. String values fan out to the per-edge bridge
    /// keys (which support `'5%'` percent and the `'auto'` keyword
    /// for centering via Yoga's YGNodeStyleSetMarginAuto). 1-4 tokens
    /// follow the standard CSS expansion rules.
    margin?: number | string;
    /// Per-edge margin accepts a number (px), percent string (`'5%'`
    /// resolves against parent main-axis size), or the keyword 'auto'
    /// for Yoga centering with `marginLeft: 'auto'` +
    /// `marginRight: 'auto'`.
    marginTop?: number | string;
    marginRight?: number | string;
    marginBottom?: number | string;
    marginLeft?: number | string;
    /// React Native shorthand alias. `marginHorizontal` fans out to
    /// `marginLeft` + `marginRight` in the prop-applier, with the same
    /// value applied to both edges. Accepts number (px), percent string
    /// (`'5%'`), or 'auto' for Yoga centering.
    marginHorizontal?: number | string;
    /// `marginVertical` fans out to `marginTop` + `marginBottom`.
    marginVertical?: number | string;
    /// CSS-spec-equivalent flow props. LTR-only fast path:
    /// Start → Left, End → Right. RTL mapping is deferred to a direction
    /// system. Values are number (px), percent string, or `'auto'` on
    /// margin (matches the per-edge surface).
    marginStart?: number | string;
    marginEnd?: number | string;
    paddingStart?: number | string;
    paddingEnd?: number | string;
    borderStartWidth?: number;
    borderEndWidth?: number;
    /// CSS positional logical aliases. LTR: start → left, end → right.
    start?: number | string;
    end?: number | string;
    /// CSS `inset` shorthand: `'10px'`, `'10px 20px'`, etc. Fans out
    /// to top/right/bottom/left via the same expansion rules as
    /// `margin` / `padding`.
    inset?: number | string;
    /// CSS `inset-block` → top + bottom.
    insetBlock?: number | string;
    /// CSS `inset-inline` → left + right (LTR).
    insetInline?: number | string;
    /// RN-style `flex: <number>` shorthand. Positive `n` expands to
    /// `{flexGrow: n, flexShrink: 1, flexBasis: 0}`; `0`
    /// to `(0, 0, 'auto')`; negative to `(0, 1, 'auto')`. Matches RN
    /// semantics, which is what JSX `<View flex={1} />` consumers
    /// expect.
    flex?: number;
    flexGrow?: number;
    flexShrink?: number;
    /// Accepts number (px), percentage string (`'50%'`), or the keyword
    /// `'auto'`. The keyword maps to Yoga's flex-basis auto path; percent
    /// maps to Yoga's percent flex-basis path.
    flexBasis?: number | string;
    /// Accepts boolean (legacy true/false) or the CSS keyword string.
    /// `"wrap-reverse"` routes through Yoga's wrap-reverse path.
    flexWrap?: boolean | 'wrap' | 'nowrap' | 'no-wrap' | 'wrap-reverse';
    order?: number;
    /// Number (px) or percent string (`'100%'`). Percent values use
    /// Yoga's percent dimension path.
    width?: number | string;
    height?: number | string;
    minWidth?: number | string;
    minHeight?: number | string;
    maxWidth?: number | string;
    maxHeight?: number | string;
    alignItems?: FlexAlign;
    alignSelf?: FlexAlignSelf;
    /// Multi-line flex cross-axis distribution. Only meaningful on a flex
    /// container with `flexWrap: true`; on a single-line container it has
    /// no visible effect.
    alignContent?: FlexAlignContent;
    justifyContent?: FlexJustify;
    /// Width/height ratio for the cross axis. RN-compatible.
    /// When set on a View with `width: 100, aspectRatio: 1.5`, the layout
    /// produces `height = 100 / 1.5 ≈ 66.67`. When `height` is set instead,
    /// the width is derived. Pass `0` or omit to clear.
    aspectRatio?: number;
}

// ── Visual style ────────────────────────────────────────────────────
export interface StyleProps {
    background?: string;            // hex; maps to setBackground
    backgroundGradient?: string;    // CSS-string; maps to setBackgroundGradient
    /// CSS background sub-properties. Stored on the View for
    /// round-tripping; paint impact is partial today.
    backgroundAttachment?: 'scroll' | 'fixed' | 'local';
    backgroundClip?: 'border-box' | 'padding-box' | 'content-box' | 'text';
    backgroundOrigin?: 'border-box' | 'padding-box' | 'content-box';
    border?: { color: string; width?: number; radius?: number };  // setBorder
    borderTop?: { color: string; width: number };                  // setBorderSide
    borderRight?: { color: string; width: number };
    borderBottom?: { color: string; width: number };
    borderLeft?: { color: string; width: number };
    // Per-attribute (RN-style flat) border props. These map to bridge
    // setters that mutate one slot in isolation, unlike `border:` which
    // sets all three at once.
    borderColor?: string;
    borderWidth?: number;
    /// `borderRadius` accepts a uniform number (px) or the RN Fabric
    /// elliptical `{ x, y }` form. The paint side currently honors a
    /// single uniform radius per corner, so the elliptical input is
    /// degraded to the average of x and y; true elliptical corner
    /// rendering is deferred.
    borderRadius?: number | { x: number; y: number };
    /// CSS / RN border-style keyword. `'dashed'` / `'dotted'` render as
    /// dashed strokes; other named styles (`'double'`, `'groove'`,
    /// `'ridge'`, `'inset'`, `'outset'`) currently degrade to solid
    /// (paint-side gap).
    /// `'none'` / `'hidden'` skip the stroke entirely.
    borderStyle?: 'solid' | 'dashed' | 'dotted' | 'double' | 'groove'
                | 'ridge' | 'inset' | 'outset' | 'none' | 'hidden';
    /// CSS list-style cluster. Pulp doesn't model HTML `<li>` / `<ul>` /
    /// `<ol>` semantics, so these props round-trip the value to View slots
    /// without painting a marker glyph today. Marker glyph rendering is
    /// deferred. `listStyle` is the shorthand
    /// (`'<type> <position> <image>'` in any order).
    listStyle?: string;
    listStyleType?:
        | 'none' | 'disc' | 'circle' | 'square' | 'decimal'
        // CSS Counter Styles Level 3 keywords. Storage-only round-trip
        // today; paint-side glyph rendering is deferred.
        | 'decimal-leading-zero'
        | 'lower-roman' | 'upper-roman'
        | 'lower-alpha' | 'upper-alpha'
        | 'lower-latin' | 'upper-latin'
        | 'lower-greek' | 'armenian' | 'georgian';
    listStyleImage?: string;
    listStylePosition?: 'inside' | 'outside';
    borderTopColor?: string;
    borderRightColor?: string;
    borderBottomColor?: string;
    borderLeftColor?: string;
    borderTopWidth?: number;
    borderRightWidth?: number;
    borderBottomWidth?: number;
    borderLeftWidth?: number;
    /// Per-corner radii also accept the RN Fabric elliptical `{ x, y }`
    /// form, degraded to averaged uniform radius; see `borderRadius`.
    borderTopLeftRadius?: number | { x: number; y: number };
    borderTopRightRadius?: number | { x: number; y: number };
    borderBottomLeftRadius?: number | { x: number; y: number };
    borderBottomRightRadius?: number | { x: number; y: number };
    /// CSS / RN outline cluster. Outline is paint-only: it draws OUTSIDE
    /// the border-box and does NOT take up Yoga layout space. Style
    /// keywords mirror borderStyle. dashed/dotted render as dashed
    /// strokes; double/groove/ridge/inset/outset currently degrade to
    /// solid. none/hidden / zero-width skip the stroke.
    outlineColor?: string;
    outlineOffset?: number;
    outlineStyle?: 'solid' | 'dashed' | 'dotted' | 'double' | 'groove'
                | 'ridge' | 'inset' | 'outset' | 'none' | 'hidden';
    outlineWidth?: number;
    opacity?: number;
    visible?: boolean;
    /// Forwards the keyword straight through to the matching bridge setter.
    backfaceVisibility?: 'hidden' | 'visible';
    /// CSS `clip-path`. Only the `path("...")` form is honored at paint
    /// time today; URL refs (`url(#clip-id)`) and named shape forms
    /// (`circle()`, `inset()`, `polygon()`, `ellipse()`) are deferred and
    /// forwarded as an empty slot. `none` / empty clears.
    clipPath?: string;
    cursor?: string;
    filter?: string;
    /// CSS `mask` shorthand. Storage-only today; @pulp/react forwards the
    /// shorthand verbatim via `setMask`. Shader compositing is deferred.
    mask?: string;
    /// CSS `mask-image`. Storage-only today; shader compositing is
    /// deferred.
    maskImage?: string;
    /// RN `mixBlendMode` (New Architecture only). Forwards the W3C
    /// blend-mode keyword set to the bridge. `'normal'` and unknown
    /// keywords are paint-time no-ops.
    mixBlendMode?:
        | 'normal' | 'multiply' | 'screen' | 'overlay'
        | 'darken' | 'lighten' | 'color-dodge' | 'color-burn'
        | 'hard-light' | 'soft-light' | 'difference' | 'exclusion'
        | 'hue' | 'saturation' | 'color' | 'luminosity';
    pointerEvents?: 'auto' | 'none' | 'box-only' | 'box-none';
    textTransform?: 'none' | 'uppercase' | 'lowercase' | 'capitalize';
    /// CSS `line-clamp` / `-webkit-line-clamp`. Maximum number of
    /// visible text lines on a multi-line Label; setting >0
    /// implicitly enables wrap on the bridge side. `0` disables clamp
    /// (CSS spec uses `none`; `0` is the numeric equivalent here).
    /// Both keys funnel through the same `setLineClamp` bridge fn.
    lineClamp?: number;
    webkitLineClamp?: number;
    /// CSS `background-repeat` keyword. Storage-only at the View level
    /// today — paint-time honoring requires
    /// `background-image: url(...)` / repeating-gradient backgrounds
    /// which haven't landed yet. Accepts the standard CSS keyword set.
    backgroundRepeat?: 'repeat' | 'repeat-x' | 'repeat-y' | 'no-repeat'
                     | 'space' | 'round';
    /// CSS transform-origin: `'NN% NN%'`, `'NNpx NNpx'`, `'center'`,
    /// or two-keyword combos (`'left top'`). Bridge expects fractional
    /// 0..1 coordinates; the prop-applier parses the string before
    /// dispatching.
    transformOrigin?: string;
    userSelect?: 'none' | 'text' | 'all';
    /// RN-style writing direction. Yoga propagates direction through
    /// layout (RTL flips flexDirection 'row' visually), and text shaping
    /// uses the same value. The CSS spec name `direction` already routes
    /// through FlexProps in this codebase (`FlexDirection` shorthand), so
    /// the JSX surface uses RN's `writingDirection`; the
    /// `style.direction = 'rtl'` path goes through the el.style adapter
    /// and reaches the same bridge function.
    writingDirection?: 'ltr' | 'rtl' | 'auto' | 'inherit';
    /// CSS transitions + animations. `transition` accepts the full CSS
    /// shorthand string; longhand fields apply uniformly across the
    /// parsed list.
    transition?: string;
    transitionProperty?: string;
    transitionDuration?: number | string;
    transitionDelay?: number | string;
    transitionTimingFunction?:
        | 'linear' | 'ease' | 'ease-in' | 'ease-out' | 'ease-in-out'
        | string; // also: 'cubic-bezier(...)', 'steps(N, end)'
    animationName?: string;
    animationDuration?: number | string;
    /// CSS box-sizing. Web designs almost universally reset to
    /// `border-box` via `* { box-sizing: border-box }`; Yoga honors the
    /// spec at layout time.
    boxSizing?: 'content-box' | 'border-box';
    /// CSS Grid surface. Grid props live on StyleProps so JSX can express
    /// `display: grid` layouts directly. The bridge handles
    /// template-track parsing, named-area parsing, and the grid-area
    /// shorthand.
    gridTemplateColumns?: string;
    gridTemplateRows?: string;
    gridTemplateAreas?: string;
    gridAutoColumns?: string;
    gridAutoRows?: string;
    gridAutoFlow?: 'row' | 'column' | 'row dense' | 'column dense' | 'dense';
    gridArea?: string;
    gridColumn?: string;
    gridRow?: string;
    gridColumnStart?: number;
    gridColumnEnd?: number;
    gridRowStart?: number;
    gridRowEnd?: number;
    gridGap?: number;
    gridColumnGap?: number;
    gridRowGap?: number;
    /// CSS / RN `display` keyword. `'none'` hides the View. `'flex'`
    /// ensures the View is visible and, when no flexDirection / direction
    /// / flexFlow is set, applies CSS's default flex-direction: row
    /// because Pulp's Yoga default is column. Other keywords (`'block'` /
    /// `'inline-block'` / `'inline-flex'` / `'grid'`) flow through the CSS
    /// shim only; for RN-flavored JSX consumers, just `'flex'` / `'none'`
    /// are the meaningful values.
    display?: 'flex' | 'none' | string;
    /// CSS / RN `box-shadow`. Accepts:
    /// - Object form (RN-style): `{ offsetX, offsetY, blur?, spread?, color, inset? }`.
    /// - String form (CSS-spec shadow list): `'2px 4px 8px rgba(0,0,0,0.3)'`
    ///   with optional `inset` keyword. Comma-separated multi-shadow
    ///   lists dispatch one bridge call per parsed shadow.
    /// `'none'` / `null` / `undefined` clears the slot.
    boxShadow?: BoxShadow | string | null;
    /// RN-style transform array. An array of single-property objects is
    /// common in design-tool exports. Wired ops:
    ///   • `translateX`, `translateY` — number, px
    ///   • `rotate`, `rotateZ` — `'45deg'` / `'1rad'` / numeric (deg)
    ///   • `scale` — uniform scalar
    ///   • `scaleX`, `scaleY` — last-write-wins because the bridge has
    ///     uniform setScale only; independent axes are deferred
    ///   • `skewX`, `skewY` — accumulated into one setSkew call
    /// Deferred (silently no-op):
    ///   • `rotateX`, `rotateY`, `perspective`, `matrix` — 2D View
    ///     model has no 3D / matrix surface
    /// CSS-string form (`'translateX(10px) rotate(45deg)'`) is deferred
    /// — pass the array shape instead.
    transform?: TransformOp[];
}

/// One entry in a `transform` array. RN spec: a single-property object
/// per entry; do not combine ops in one entry.
export type TransformOp =
    | { translateX: number }
    | { translateY: number }
    | { rotate: string | number }
    | { rotateZ: string | number }
    | { scale: number }
    | { scaleX: number }
    | { scaleY: number }
    | { skewX: string | number }
    | { skewY: string | number }
    | { rotateX: string | number }
    | { rotateY: string | number }
    | { perspective: number }
    | { matrix: ReadonlyArray<number> };

/// Object form of `boxShadow` for RN-flavored consumers. Mirrors the
/// `border` prop shape.
export interface BoxShadow {
    offsetX: number;
    offsetY: number;
    /// Defaults to 4 if omitted (matches the bridge's setBoxShadow default).
    blur?: number;
    /// Defaults to 0.
    spread?: number;
    /// Hex / rgb / rgba / named CSS color string. Resolved via parseCSSColor.
    color: string;
    /// `true` renders the shadow inside the box (CSS `inset` keyword).
    inset?: boolean;
}

// ── Common base props ──────────────────────────────────────────────
export interface BaseProps extends FlexProps, StyleProps {
    /// Optional explicit ID. If omitted, an auto-incrementing ID is
    /// generated. Useful for testing + debugging the bridge log.
    id?: string;
    /// React key for keyed reorder.
    key?: Key;
    /// Children — text or other intrinsics.
    children?: ReactNode;
    /// Opt this view in as the active click-eligible overlay. When `true`,
    /// the prop-applier calls `claimOverlay(id)` on mount and
    /// `releaseOverlay(id)` on unmount. The platform window host routes
    /// any click that lands inside the view's window-rect to the overlay
    /// subtree first, so absolutely-positioned popovers built from
    /// `<View overlay style={{position: 'absolute', ...}}>` get clicks
    /// instead of whatever sibling/ancestor view sits behind the popover.
    overlay?: boolean;
}

// ── Container intrinsics ────────────────────────────────────────────
export type ViewProps = BaseProps;
export type RowProps = BaseProps;
export type ColProps = BaseProps;
export type PanelProps = BaseProps;
export type ScrollViewProps = BaseProps;
export type ModalProps = BaseProps & { open?: boolean };

// ── Text intrinsics ────────────────────────────────────────────────
// Label/Button accept string children that lower to setText, NOT a
// nested text-node. This matches Pulp's createLabel(id, text, parent).
export interface LabelProps extends BaseProps {
    text?: string;
    textColor?: string;
    /// CSS / RN `text-align`. `'auto'` is writing-direction-relative
    /// (LTR-only today). `'justify'` reaches canvas `TextAlign::justify`;
    /// full paragraph justification is deferred, so backends approximate
    /// as left until then.
    textAlign?: 'left' | 'center' | 'right' | 'auto' | 'justify';
}

export interface ButtonProps extends LabelProps {
    onClick?: () => void;
    disabled?: boolean;
}

export interface TextEditorProps extends BaseProps {
    text?: string;
    placeholder?: string;
    multiLine?: boolean;
    onChange?: (value: string) => void;
}

// ── Audio widgets ──────────────────────────────────────────────────
export interface KnobProps extends BaseProps {
    value?: number;     // 0..1
    onChange?: (value: number) => void;
}

export interface FaderProps extends BaseProps {
    orientation?: 'vertical' | 'horizontal';
    value?: number;
    onChange?: (value: number) => void;
}

export interface SpectrumProps extends BaseProps {
    /// FFT magnitudes, normalized 0..1. Maps to setSpectrumData.
    data?: number[] | Float32Array;
}

export interface WaveformProps extends BaseProps {
    /// Sample buffer. Maps to setWaveformData.
    data?: number[] | Float32Array;
}

export interface MeterProps extends BaseProps {
    orientation?: 'vertical' | 'horizontal';
    level?: number;     // 0..1
}

export interface ProgressProps extends BaseProps {
    value?: number;     // 0..1
}

export interface XYPadProps extends BaseProps {
    x?: number;         // 0..1
    y?: number;         // 0..1
    onChange?: (x: number, y: number) => void;
}

export interface CheckboxProps extends BaseProps {
    checked?: boolean;
    onChange?: (checked: boolean) => void;
}

export interface ToggleProps extends CheckboxProps {}

export interface ComboProps extends BaseProps {
    options?: string[];
    selected?: number;
    onChange?: (index: number) => void;
}

// ── Ink & Signal design-system widgets ──────────────────────────────

/// Compact status pill (counts, format/sample-rate chips). Text comes from
/// the `text` prop or string children; `tone` selects the semantic colour.
export interface BadgeProps extends BaseProps {
    text?: string;
    tone?: 'neutral' | 'info' | 'success' | 'warning' | 'danger';
}

/// `[−] value [+]` numeric stepper. `value` maps to setValue; the widget
/// owns its min/max/step (set via setMin/setMax/setStep).
export interface StepperProps extends BaseProps {
    value?: number;
    onChange?: (value: number) => void;
}

/// Bipolar 1-D pan control. `value` is −1 (hard left) .. +1 (hard right).
export interface PanProps extends BaseProps {
    value?: number;
    onChange?: (value: number) => void;
}

export interface ListBoxProps extends BaseProps {
    options?: string[];
    selected?: number;
    onChange?: (index: number) => void;
}

export interface VirtualListBindEvent {
    rowId: string;
    index: number;
}

export interface VirtualListProps extends BaseProps {
    rowCount: number;
    rowHeight: number;
    overscan?: number;
    selectionMode?: 'none' | 'single' | 'multi';
    selected?: number;
    scrollToRow?: number;
    renderRow: (index: number) => ReactNode;
    onBindRow?: (event: unknown) => void;
    onChange?: (selection: number[]) => void;
    onActivate?: (index: number) => void;
}

export interface CanvasProps extends BaseProps {
    /// Drawing happens via the canvas* bridge ops in render callbacks.
    /// Wire your draw function via a ref, not via children.
}

export interface ImageProps extends BaseProps {
    src?: string;       // pulp asset key
}

export interface IconProps extends BaseProps {
    name?: string;
}

/// Inline SVG path widget. Renders an `<svg><path/></svg>` from a
/// path-data string + paint attributes. Use this for icon glyphs inside
/// React-rendered UIs that previously sent raw `<svg>` markup, which the
/// bridge has no DOM equivalent for.
export interface SvgPathProps extends BaseProps {
    /// SVG path-data string (the `d=` attribute on `<path>`).
    /// Supports M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, A/a, Z/z.
    d?: string;
    /// Two-number viewbox (width, height). The path is parsed in the
    /// viewbox's coordinate space and scaled into the widget's bounds
    /// at paint time. Defaults to (0, 0) — i.e. the bridge will not
    /// scale and the path's native units determine size.
    ///
    /// Also accepts the SVG-spec string form `'min-x min-y w h'` (or
    /// `'w h'`). Common with Lucide /
    /// Heroicons / Figma SVG exports. The bridge consumes width +
    /// height only today (the SvgPathWidget doesn't yet honor the
    /// min-x / min-y origin offset — paint-side gap), so the trailing
    /// two tokens become the (w, h) tuple. Tokens may be space- or
    /// comma-separated.
    viewBox?: [number, number] | string;
    /// Fill color as hex (`#rrggbb` / `#rrggbbaa`) or `"none"`. Default
    /// is opaque black (matches the SVG `<path>` default of
    /// `fill="black"` — the SvgPathWidget fills with nonzero black when
    /// no `fill` is set). Pass `"none"` to render the path unfilled
    /// (stroke-only, or invisible if no stroke is set either).
    fill?: string;
    /// Fill winding rule, mirroring SVG's `fill-rule`. Defaults to
    /// `"nonzero"` (SVG / Canvas2D default). Use `"evenodd"` for
    /// compound annular paths — e.g. a stroked ellipse that a framework
    /// has lowered to a two-subpath `M…Z M…Z` fill (JUCE's
    /// `SVGGraphicsContext` does this for `Graphics::drawEllipse`); only
    /// even-odd winding renders the ring's hole, where nonzero paints a
    /// solid disc.
    fillRule?: 'nonzero' | 'evenodd';
    /// Gradient fill as a CSS `linear-gradient(...)` string — e.g.
    /// `"linear-gradient(to bottom, #ff0000, #0000ff)"`. When set
    /// (non-empty), it overrides the solid `fill` color at paint time;
    /// the SvgPathWidget parses the string and fills via
    /// `Canvas::set_fill_gradient_linear`. Unparseable input silently
    /// falls back to the solid `fill`. Radial / conic gradients and the
    /// idiomatic `<SvgLinearGradient>` + `fill="url(#id)"` subtree form
    /// are deferred. This exposes the existing bridge fill-gradient slot
    /// as a typed `@pulp/react` prop.
    fillGradient?: string;
    /// Stroke color as hex or `"none"`.
    stroke?: string;
    /// Stroke width in widget-local units. Defaults to 1.
    strokeWidth?: number;
}

/// Inline SVG `<rect>` widget. Renders a rectangle with the configured
/// x/y/width/height + fill/stroke. Pairs with SvgLine for chart-style
/// and band-shape thumbnail UIs.
export interface SvgRectProps extends BaseProps {
    /// Rect origin x in widget-local units. Defaults to 0.
    x?: number;
    /// Rect origin y in widget-local units. Defaults to 0.
    y?: number;
    /// Rect width in widget-local units. Defaults to 0 (invisible).
    width?: number;
    /// Rect height in widget-local units. Defaults to 0 (invisible).
    height?: number;
    /// Fill color as hex (`#rrggbb` / `#rrggbbaa`) or `"none"`. Default
    /// is opaque black (matches SVG `<rect>` default of `fill="black"`).
    fill?: string;
    /// Stroke color as hex or `"none"`. Default: no stroke.
    stroke?: string;
    /// Stroke width in widget-local units. Default 1.
    strokeWidth?: number;
}

/// Inline SVG `<line>` widget. Renders a 1-D line from (x1, y1) to
/// (x2, y2) with the configured stroke + width. SVG `<line>` has no
/// fill; for API consistency with `<rect>` and `<path>` the bridge
/// exposes `fill` but it's a no-op for lines.
export interface SvgLineProps extends BaseProps {
    /// Start endpoint x in widget-local units. Defaults to 0.
    x1?: number;
    /// Start endpoint y in widget-local units. Defaults to 0.
    y1?: number;
    /// End endpoint x in widget-local units. Defaults to 0.
    x2?: number;
    /// End endpoint y in widget-local units. Defaults to 0.
    y2?: number;
    /// Stroke color as hex or `"none"`. Default: opaque black.
    stroke?: string;
    /// Stroke width in widget-local units. Default 1.
    strokeWidth?: number;
}

// ── Element-name → intrinsic-props map ──────────────────────────────
// The host config consults this implicitly; tests assert names against it.
export interface IntrinsicElementMap {
    View: ViewProps;
    Row: RowProps;
    Col: ColProps;
    Panel: PanelProps;
    ScrollView: ScrollViewProps;
    NativeView: ViewProps;
    Modal: ModalProps;
    Label: LabelProps;
    Button: ButtonProps;
    TextEditor: TextEditorProps;
    Knob: KnobProps;
    Fader: FaderProps;
    Spectrum: SpectrumProps;
    Waveform: WaveformProps;
    Meter: MeterProps;
    Progress: ProgressProps;
    XYPad: XYPadProps;
    Checkbox: CheckboxProps;
    Toggle: ToggleProps;
    Combo: ComboProps;
    Badge: BadgeProps;
    Stepper: StepperProps;
    Pan: PanProps;
    ListBox: ListBoxProps;
    VirtualList: VirtualListProps;
    Canvas: CanvasProps;
    Image: ImageProps;
    Icon: IconProps;
    SvgPath: SvgPathProps;
    SvgRect: SvgRectProps;
    SvgLine: SvgLineProps;
}

export type IntrinsicElementName = keyof IntrinsicElementMap;

// ── Internal: instance descriptor ───────────────────────────────────
// What the host config holds per widget. Not exported.
export interface PulpInstance {
    id: string;
    type: IntrinsicElementName;
    parentId?: string;
    /// Last-applied props. Used for diff in commitUpdate.
    props: Record<string, unknown>;
    /// Children IDs in order — for insertBefore reorder.
    childIds: string[];
    /// True once the bridge createX call has been emitted for this widget.
    /// React calls appendInitialChild bottom-up (leaves before parents),
    /// so we cannot eagerly create on the bridge — the parent doesn't
    /// exist yet, and Pulp's resolve_parent silently falls back to root.
    /// Track on-bridge state and queue child attaches until the parent
    /// reaches the bridge via a root-level appendChildToContainer.
    onBridge: boolean;
    /// Children whose attach was deferred until this descriptor lands on
    /// the bridge. Drained by attach() once onBridge flips true.
    /// Each entry is the child descriptor and the index it should land at.
    pendingChildren: Array<{ child: PulpInstance; index: number }>;
    /// DOM-shim element returned from `getPublicInstance`. Imported
    /// bundles call DOM-style methods
    /// (`getContext('2d')`, `getBoundingClientRect()`, `style.X=`,
    /// `addEventListener`) on `ref.current`, which is what
    /// `getPublicInstance` returns. We instantiate the existing
    /// web-compat `Element` class bound to this instance's native id,
    /// so DOM-shim methods forward through the existing bridge wiring
    /// rather than failing with "not a function" → infinite re-render.
    /// `null` when the Element shim isn't available (e.g. pure-JS
    /// unit tests with no bridge engine).
    _dom?: unknown;
}

// ── Container ──────────────────────────────────────────────────────
// The root that React renders into. Pulp's bridge has an implicit root
// (usually the outermost View on screen); we model it as a container
// with a fixed sentinel parentId of "" (empty string), which is the
// convention the bridge uses for "no parent".
export interface PulpContainer {
    rootId: string;
    idPrefix?: string;
    /// Auto-incrementing counter for generating widget IDs. Per-container
    /// so multiple roots in the same engine don't collide.
    nextId: number;
}
