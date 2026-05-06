// pulp #1486 — Pulp design-import IR (Phase 1 spike).
//
// IRNode is the typed intermediate representation that survives source
// re-import. Adapters lower source formats (Figma / Mitosis / Claude
// Design HTML / etc.) into IRNode trees keyed by `stable_anchor_id`;
// the tweaks layer (`pulp-tweaks.json`) is keyed by the same anchors,
// so dev overrides survive when the designer regenerates the source.
//
// Spec: /tmp/pulp-ir-spec-spike.md (sub-agent #25 draft).
// Phase 1: types + Claude Design HTML adapter (content-hash anchors)
// + tweaks read/write + 5-scenario harness.

// ── Value types (§1.1, §2.1) ──────────────────────────────────────────

export type TokenRef = `{${string}}`;

export type Length =
    | number
    | `${number}%`
    | `${number}vw`
    | `${number}vh`
    | `${number}vmin`
    | `${number}vmax`
    | 'auto'
    | TokenRef;

export type Color = string | TokenRef;
export type Pixels = number | TokenRef;
export type NumberRef = number | TokenRef;

// ── TypedLayout (§1.2) ────────────────────────────────────────────────

export interface TypedLayout {
    display?: 'flex' | 'none' | 'contents' | 'block' | 'inline-flex' | string;

    flexDirection?: 'row' | 'row-reverse' | 'column' | 'column-reverse';
    flexWrap?: 'nowrap' | 'wrap' | 'wrap-reverse';
    flexGrow?: number;
    flexShrink?: number;
    flexBasis?: Length;
    order?: number;

    alignItems?: 'flex-start' | 'center' | 'flex-end' | 'stretch' | 'baseline';
    alignSelf?: 'auto' | 'flex-start' | 'center' | 'flex-end' | 'stretch' | 'baseline';
    alignContent?: 'flex-start' | 'center' | 'flex-end' | 'stretch'
        | 'space-between' | 'space-around' | 'space-evenly';
    justifyContent?: 'flex-start' | 'center' | 'flex-end'
        | 'space-between' | 'space-around' | 'space-evenly';

    margin?: Length;
    marginTop?: Length;
    marginRight?: Length;
    marginBottom?: Length;
    marginLeft?: Length;
    marginHorizontal?: Length;
    marginVertical?: Length;

    padding?: Length;
    paddingTop?: Length;
    paddingRight?: Length;
    paddingBottom?: Length;
    paddingLeft?: Length;
    paddingHorizontal?: Length;
    paddingVertical?: Length;

    top?: Length;
    right?: Length;
    bottom?: Length;
    left?: Length;
    inset?: Length;

    width?: Length;
    height?: Length;
    minWidth?: Length;
    maxWidth?: Length;
    minHeight?: Length;
    maxHeight?: Length;

    position?: 'static' | 'relative' | 'absolute' | 'fixed' | 'sticky';
    zIndex?: number;

    gap?: Length;
    rowGap?: Length;
    columnGap?: Length;

    aspectRatio?: number | `${number}/${number}`;

    overflow?: 'visible' | 'hidden' | 'scroll' | 'auto';
    overflowX?: 'visible' | 'hidden' | 'scroll' | 'auto';
    overflowY?: 'visible' | 'hidden' | 'scroll' | 'auto';
}

// ── TypedPaint (§2.2) ─────────────────────────────────────────────────

export interface Gradient {
    type: 'linear' | 'radial' | 'conic';
    angle?: number;
    stops: { offset: number; color: Color }[];
}

export interface BoxShadowOp {
    offsetX: Pixels;
    offsetY: Pixels;
    blur: Pixels;
    spread?: Pixels;
    color: Color;
    inset?: boolean;
}

export type FilterFn =
    | { fn: 'blur'; px: Pixels }
    | { fn: 'brightness'; amount: NumberRef }
    | { fn: 'contrast'; amount: NumberRef }
    | { fn: 'grayscale'; amount: NumberRef }
    | { fn: 'sepia'; amount: NumberRef }
    | { fn: 'hue-rotate'; deg: NumberRef }
    | { fn: 'invert'; amount: NumberRef }
    | { fn: 'saturate'; amount: NumberRef }
    | { fn: 'opacity'; amount: NumberRef }
    | { fn: 'drop-shadow'; offsetX: Pixels; offsetY: Pixels; blur: Pixels; color: Color };

export type TransformOp =
    | { op: 'scale'; x: NumberRef; y?: NumberRef }
    | { op: 'scaleX'; x: NumberRef }
    | { op: 'scaleY'; y: NumberRef }
    | { op: 'translate'; x: Pixels; y: Pixels }
    | { op: 'translateX'; x: Pixels }
    | { op: 'translateY'; y: Pixels }
    | { op: 'rotate'; deg: NumberRef }
    | { op: 'rotateX'; deg: NumberRef }
    | { op: 'rotateY'; deg: NumberRef }
    | { op: 'rotateZ'; deg: NumberRef }
    | { op: 'skewX'; deg: NumberRef }
    | { op: 'skewY'; deg: NumberRef }
    | { op: 'matrix'; values: [number, number, number, number, number, number] }
    | { op: 'perspective'; distance: Pixels };

export interface TypedPaint {
    backgroundColor?: Color;
    backgroundGradient?: Gradient;

    color?: Color;

    borderColor?: Color;
    borderTopColor?: Color;
    borderRightColor?: Color;
    borderBottomColor?: Color;
    borderLeftColor?: Color;
    borderWidth?: Pixels;
    borderTopWidth?: Pixels;
    borderRightWidth?: Pixels;
    borderBottomWidth?: Pixels;
    borderLeftWidth?: Pixels;
    borderStyle?: 'solid' | 'dashed' | 'dotted' | 'double' | 'groove'
        | 'ridge' | 'inset' | 'outset' | 'none' | 'hidden';

    borderRadius?: Pixels;
    borderTopLeftRadius?: Pixels;
    borderTopRightRadius?: Pixels;
    borderBottomLeftRadius?: Pixels;
    borderBottomRightRadius?: Pixels;

    boxShadow?: BoxShadowOp[];

    opacity?: NumberRef;
    filter?: FilterFn[];
    backdropFilter?: FilterFn[];

    transform?: TransformOp[];

    cursor?: 'default' | 'pointer' | 'text' | 'crosshair' | 'grab' | 'grabbing' | 'not-allowed';
}

// ── TypedText (§3) ────────────────────────────────────────────────────

export interface TypedText {
    fontFamily?: string | TokenRef;
    fontSize?: Pixels;
    fontWeight?: number | 'normal' | 'bold' | 'lighter' | 'bolder';
    fontStyle?: 'normal' | 'italic' | 'oblique';

    lineHeight?: Pixels | number;
    letterSpacing?: Pixels;
    wordSpacing?: Pixels;

    textAlign?: 'left' | 'center' | 'right' | 'justify' | 'auto';
    textDecorationLine?: 'none' | 'underline' | 'line-through' | 'underline line-through';
    textDecorationColor?: Color;
    textDecorationStyle?: 'solid' | 'double' | 'dotted' | 'dashed' | 'wavy';
    textTransform?: 'none' | 'uppercase' | 'lowercase' | 'capitalize';

    whiteSpace?: 'normal' | 'nowrap' | 'pre' | 'pre-wrap' | 'pre-line';
    wordWrap?: 'normal' | 'break-word';
    overflowWrap?: 'normal' | 'break-word' | 'anywhere';
    textOverflow?: 'clip' | 'ellipsis';
    numberOfLines?: number;

    text?: string;
}

// ── raw_source (§5) ──────────────────────────────────────────────────

export type SourceFormat =
    | { kind: 'figma-reconstruction'; spec: unknown }
    | { kind: 'figma-mcp-design-context'; payload: unknown }
    | { kind: 'html'; outerHtml: string; computedStyle?: Record<string, string> }
    | { kind: 'mitosis'; node: unknown }
    | { kind: 'pencil-mcp'; node: unknown }
    | { kind: 'stitch-html'; outerHtml: string }
    | { kind: 'v0-tsx'; jsxText: string }
    | { kind: 'claude-design-html'; outerHtml: string; computedStyle?: Record<string, string> }
    | { kind: 'rn-file'; jsxText: string }
    | { kind: 'unknown'; payload: unknown };

// ── provenance (§6) ──────────────────────────────────────────────────

export interface IRProvenance {
    adapter: string;
    version: string;
    ts: string;

    source_uri?: string;
    imported_at?: string;
    last_seen_at?: string;
    regen_count?: number;

    source_url?: string;
    source_commit?: string;
}

// ── confidence (§7.2) ────────────────────────────────────────────────

export type Confidence = 'PASS' | 'DIVERGE' | 'NOT_IMPL';

// ── IRNode (the unifying tree shape) ─────────────────────────────────

export type IRTag =
    | 'View'
    | 'Label'
    | 'Button'
    | 'Image'
    | 'Icon'
    | 'TextEditor'
    | 'ScrollView'
    | 'Modal'
    | string;

export interface IRMeta {
    role?: string;
    anchor_id_override?: string;
    /** Set by applyTweaks when a tweak's anchor isn't found in the tree. */
    orphaned_tweaks?: Record<string, TweakValue>;
    /** Allow adapter-specific extras without widening the surface area. */
    [key: string]: unknown;
}

export interface IRNode {
    tag: IRTag;
    stable_anchor_id: string;
    /** Optional adapter-side identifier — Figma node IDs, Pencil node IDs, etc. */
    source_node_id?: string;

    layout?: TypedLayout;
    paint?: TypedPaint;
    text?: TypedText;

    children: IRNode[];

    meta?: IRMeta;
    provenance: IRProvenance;
    raw_source: SourceFormat;
    confidence: Confidence;
}

// ── Adapter contract (§7) ────────────────────────────────────────────

export type AnchorStrategy = 'adapter' | 'content-hash' | 'path';

export interface LowerOptions {
    anchorStrategy?: AnchorStrategy;
    preserveRawSource?: boolean;
    resolveTokens?: boolean;
    tokens?: DTCGTokenTree;
}

export type Drift =
    | { kind: 'added'; anchor: string; node: IRNode }
    | { kind: 'removed'; anchor: string; node: IRNode }
    | { kind: 'changed'; anchor: string; field: string; oldValue: unknown; newValue: unknown }
    | { kind: 'reordered'; anchor: string; oldIndex: number; newIndex: number; parent: string }
    | {
          kind: 'orphaned-tweak';
          anchor: string;
          tweak: TweakValue;
          reason: 'node-deleted' | 'field-no-longer-supported' | 'anchor-strategy-changed';
      };

export interface SourceAdapter<R = unknown> {
    readonly name: string;
    readonly version: string;
    lower(raw: R, opts?: LowerOptions): Promise<IRNode>;
    diff(prev: IRNode, next: IRNode): Drift[];
    applyTweaks(node: IRNode, tweaks: TweaksFile): IRNode;
}

// ── Tweaks layer (§8) ────────────────────────────────────────────────

export interface TweaksFile {
    $schema?: string;
    meta: {
        pulpVersion: string;
        importSession: string;
        anchorStrategy?: AnchorStrategy;
    };
    tweaks: Record<string, TweakValue>;
}

export type TweakValue = {
    [path: string]: unknown;
};

// ── DTCG token tree (§10 Q3 — post-lowering resolution) ──────────────

export interface DTCGTokenTree {
    /** Recursive token map. Leaves carry `$value` per DTCG spec. */
    [key: string]: { $value?: unknown; $type?: string; [k: string]: unknown } | DTCGTokenTree;
}

export class AdapterCapabilityError extends Error {
    constructor(message: string) {
        super(message);
        this.name = 'AdapterCapabilityError';
    }
}
