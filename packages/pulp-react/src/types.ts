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

export interface FlexProps {
    direction?: FlexDirection;
    gap?: number;
    rowGap?: number;
    columnGap?: number;
    padding?: number;
    /// pulp #1434 (cross-surface mega-batch) — per-edge padding accepts
    /// either a number (px) or a percent string ('5%' → percent of parent
    /// main-axis size). Yoga padding does NOT support 'auto'.
    paddingTop?: number | string;
    paddingRight?: number | string;
    paddingBottom?: number | string;
    paddingLeft?: number | string;
    /// pulp #1434 batch 4 — `paddingHorizontal` fans out to `paddingLeft` +
    /// `paddingRight`.
    /// pulp #1434 cross-surface mega-batch — accepts number (px) or
    /// percent string ('5%'). Yoga padding has no 'auto' API.
    paddingHorizontal?: number | string;
    /// pulp #1434 batch 4 — `paddingVertical` fans out to `paddingTop` +
    /// `paddingBottom`.
    paddingVertical?: number | string;
    margin?: number;
    /// pulp #1434 (cross-surface mega-batch) — per-edge margin accepts a
    /// number (px), percent string ('5%' → percent of parent main-axis
    /// size), or the keyword 'auto' (Yoga YGNodeStyleSetMarginAuto —
    /// used for centering with `marginLeft: 'auto'` + `marginRight: 'auto'`).
    marginTop?: number | string;
    marginRight?: number | string;
    marginBottom?: number | string;
    marginLeft?: number | string;
    /// pulp #1434 batch 4 — React Native shorthand alias. `marginHorizontal`
    /// fans out to `marginLeft` + `marginRight` in the prop-applier; same
    /// value applied to both edges. Useful for porting RN code as-is.
    /// pulp #1434 cross-surface mega-batch — accepts number (px),
    /// percent string ('5%'), or 'auto' (Yoga centering).
    marginHorizontal?: number | string;
    /// pulp #1434 batch 4 — `marginVertical` fans out to `marginTop` +
    /// `marginBottom`.
    marginVertical?: number | string;
    flexGrow?: number;
    flexShrink?: number;
    /// pulp #1434 (rn batch C) — accepts number (px), percentage string
    /// (`'50%'`), or the keyword `'auto'`. The keyword maps to Yoga's
    /// `YGNodeStyleSetFlexBasisAuto`; percent maps to
    /// `YGNodeStyleSetFlexBasisPercent`.
    flexBasis?: number | string;
    flexWrap?: boolean;
    order?: number;
    /// pulp #1434 (rn batch C) — number (px) or percent string
    /// (`'100%'`). Yoga's percent API is dispatched on
    /// `FlexStyle::dim_*.unit` in `yoga_layout.cpp`.
    width?: number | string;
    height?: number | string;
    minWidth?: number | string;
    minHeight?: number | string;
    maxWidth?: number | string;
    maxHeight?: number | string;
    alignItems?: FlexAlign;
    alignSelf?: FlexAlignSelf;
    justifyContent?: FlexJustify;
    /// pulp #1434 — width/height ratio for the cross axis. RN-compatible.
    /// When set on a View with `width: 100, aspectRatio: 1.5`, the layout
    /// produces `height = 100 / 1.5 ≈ 66.67`. When `height` is set instead,
    /// the width is derived. Pass `0` or omit to clear.
    aspectRatio?: number;
}

// ── Visual style ────────────────────────────────────────────────────
export interface StyleProps {
    background?: string;            // hex; maps to setBackground
    backgroundGradient?: string;    // CSS-string; maps to setBackgroundGradient
    border?: { color: string; width?: number; radius?: number };  // setBorder
    borderTop?: { color: string; width: number };                  // setBorderSide
    borderRight?: { color: string; width: number };
    borderBottom?: { color: string; width: number };
    borderLeft?: { color: string; width: number };
    // pulp #1027 — per-attribute (RN-style flat) border props. These map to
    // the per-attribute bridge setters that mutate one slot in isolation,
    // unlike `border:` which sets all three at once.
    borderColor?: string;
    borderWidth?: number;
    borderRadius?: number;
    borderTopColor?: string;
    borderRightColor?: string;
    borderBottomColor?: string;
    borderLeftColor?: string;
    borderTopWidth?: number;
    borderRightWidth?: number;
    borderBottomWidth?: number;
    borderLeftWidth?: number;
    borderTopLeftRadius?: number;
    borderTopRightRadius?: number;
    borderBottomLeftRadius?: number;
    borderBottomRightRadius?: number;
    opacity?: number;
    visible?: boolean;
    /// CSS / RN `display` keyword (pulp #1434 Triage #12). `'none'`
    /// hides the View (sets visible=false). `'flex'` is pulp's
    /// implicit default and is accepted as a no-op confirmation. Other
    /// keywords (`'block'` / `'inline-block'` / `'inline-flex'` /
    /// `'grid'`) flow through the CSS shim only — for RN-flavored JSX
    /// consumers, just `'flex'` / `'none'` are the meaningful values.
    display?: 'flex' | 'none' | string;
    /// CSS / RN `box-shadow` (pulp #1434 Triage #15). Accepts:
    /// - Object form (RN-style): `{ offsetX, offsetY, blur?, spread?, color, inset? }`.
    /// - String form (CSS-spec single shadow): `'2px 4px 8px rgba(0,0,0,0.3)'`
    ///   with optional `inset` keyword. Multi-shadow comma-separated
    ///   lists are deferred — single-shadow path lands first.
    /// `'none'` / `null` / `undefined` clears the slot.
    boxShadow?: BoxShadow | string | null;
    /// RN-style transform array (pulp #1434 Triage #9). An array of
    /// single-property objects — Figma / v0.dev / Claude Design exports
    /// emit this constantly. Wired ops:
    ///   • `translateX`, `translateY` — number, px
    ///   • `rotate`, `rotateZ` — `'45deg'` / `'1rad'` / numeric (deg)
    ///   • `scale` — uniform scalar
    ///   • `scaleX`, `scaleY` — last-write-wins (bridge has uniform
    ///     setScale only; independent axes deferred)
    /// Deferred (silently no-op):
    ///   • `skewX`, `skewY` — bridge fn unregistered; follow-up
    ///   • `rotateX`, `rotateY`, `perspective`, `matrix` — 2D View
    ///     model has no 3D / matrix surface
    /// CSS-string form (`'translateX(10px) rotate(45deg)'`) is deferred
    /// — pass the array shape instead.
    transform?: TransformOp[];
}

/// One entry in a `transform` array. RN spec: a single-property object
/// per entry (you don't combine ops in one entry). pulp #1434 Triage #9.
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

/// Object form of `boxShadow` for RN-flavored consumers (mirrors the
/// `border` prop shape). pulp #1434 Triage #15.
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
    /// pulp #1148 — opt this view in as the active click-eligible overlay.
    /// When `true`, the prop-applier calls `claimOverlay(id)` on mount and
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
    /// CSS / RN `text-align`. `'auto'` and `'justify'` added in
    /// pulp #1434. `'auto'` is writing-direction-relative (LTR-only
    /// today). `'justify'` reaches canvas `TextAlign::justify`;
    /// SkParagraph kJustify wiring is a follow-up — backends
    /// approximate as left until then.
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

export interface ListBoxProps extends BaseProps {
    options?: string[];
    selected?: number;
    onChange?: (index: number) => void;
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

/// Inline SVG path widget (pulp #994 / #991). Renders an `<svg><path/></svg>`
/// from a path-data string + paint attributes. Use this for icon glyphs
/// inside React-rendered UIs that previously sent raw `<svg>` markup
/// (which the bridge has no DOM equivalent for).
export interface SvgPathProps extends BaseProps {
    /// SVG path-data string (the `d=` attribute on `<path>`).
    /// Supports M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, A/a, Z/z.
    d?: string;
    /// Two-number viewbox (width, height). The path is parsed in the
    /// viewbox's coordinate space and scaled into the widget's bounds
    /// at paint time. Defaults to (0, 0) — i.e. the bridge will not
    /// scale and the path's native units determine size.
    viewBox?: [number, number];
    /// Fill color as hex (`#rrggbb` / `#rrggbbaa`) or `"none"`. Defaults
    /// to the SvgPathWidget's internal default (transparent + no stroke
    /// = invisible). Pass `"none"` to clear an inherited fill.
    fill?: string;
    /// Stroke color as hex or `"none"`.
    stroke?: string;
    /// Stroke width in widget-local units. Defaults to 1.
    strokeWidth?: number;
}

/// Inline SVG `<rect>` widget (pulp #1416). Renders a rectangle with
/// the configured x/y/width/height + fill/stroke. Pairs with SvgLine
/// for chart-style and band-shape thumbnail UIs (Spectr [G] preset
/// manager).
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

/// Inline SVG `<line>` widget (pulp #1416). Renders a 1-D line from
/// (x1, y1) to (x2, y2) with the configured stroke + width. SVG
/// `<line>` has no fill; for API consistency with `<rect>` and
/// `<path>` the bridge exposes `fill` but it's a no-op for lines.
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
    ListBox: ListBoxProps;
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
}

// ── Container ──────────────────────────────────────────────────────
// The root that React renders into. Pulp's bridge has an implicit root
// (usually the outermost View on screen); we model it as a container
// with a fixed sentinel parentId of "" (empty string), which is the
// convention the bridge uses for "no parent".
export interface PulpContainer {
    rootId: string;
    /// Auto-incrementing counter for generating widget IDs. Per-container
    /// so multiple roots in the same engine don't collide.
    nextId: number;
}
