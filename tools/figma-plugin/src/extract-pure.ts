// Pure, host-neutral helpers shared by the UI plugin and headless extractor.
//
// Every function here operates on types only (SceneNode shape, RGBA,
// Paint, GradientPaint, FrameNode axis enums) — no `await
// figma.X.Async()`, no `getBytesAsync`, no `exportAsync`. Anything
// pulling bytes through the Figma Plugin API stays in `extract.ts`.
//
// The extractor has two real consumers — the UI plugin (`code.ts`) and the
// headless bundle (`headless.ts`) — plus a structural neighbor, the Python REST
// port at `tools/import-design/figma_rest_export.py`, which mirrors these
// helpers field-for-field. Co-locating the pure logic makes drift between the
// language ports visible in one file and reduces the surface area future
// provider abstractions (`NodeProvider`, `AssetProvider`, etc.) have to thread
// through.

import type { ExtractedFigmaNode, ExtractedLayout } from "./extract-model";
import type { AudioWidgetKind } from "./extract-model";
import type { FontFamilyAsset } from "./extract";

// ──────────────────────────────────────────────────────────────────────────
// Color helpers — convert Figma Paint / RGBA shapes into CSS strings.

export function paintToColor(p: SolidPaint): string {
  const c = p.color;
  const a = p.opacity !== undefined ? p.opacity : 1;
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  if (a >= 1) return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  return `rgba(${r}, ${g}, ${b}, ${a.toFixed(3)})`;
}

export function rgbaToCss(c: RGBA): string {
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  if (c.a === undefined || c.a >= 1) {
    return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  }
  return `rgba(${r}, ${g}, ${b}, ${c.a.toFixed(3)})`;
}

export function hex2(n: number): string {
  return n.toString(16).padStart(2, "0");
}

// A gradient paint's own `opacity` (0..1) scales every stop's alpha — it is
// paint-level, distinct from the layer opacity, and dropping it painted a 24%
// white sheen as a hard white ramp. Same fold the solid path (paintToColor)
// and the REST/.fig lanes apply.
function stopWithPaintOpacity(c: RGBA, paintOpacity: number): RGBA {
  if (paintOpacity >= 1) return c;
  return { ...c, a: (c.a === undefined ? 1 : c.a) * paintOpacity };
}

export function gradientToCss(p: GradientPaint): string {
  if (!p.gradientStops || p.gradientStops.length === 0) return "linear-gradient(transparent, transparent)";
  const op = p.opacity !== undefined ? p.opacity : 1;
  // Pulp's setBackgroundGradient bridge takes color stop positions implicitly by
  // index, and its parseColor doesn't strip trailing `Npc%` from a token.
  // Emit colors only (no inline percentages).
  const stops = p.gradientStops.map((s) => rgbaToCss(stopWithPaintOpacity(s.color, op))).join(", ");
  return `linear-gradient(to bottom, ${stops})`;
}

export function gradientFallbackFlat(p: GradientPaint): string {
  const first = p.gradientStops?.[0]?.color;
  if (!first) return "transparent";
  return rgbaToCss(stopWithPaintOpacity(first, p.opacity !== undefined ? p.opacity : 1));
}

// ──────────────────────────────────────────────────────────────────────────
// Ordered paint-stack lowering (audit item 7).
//
// Figma renders `fills` bottom→top (index 0 at the BOTTOM). The IR's
// background model has exactly three slots — one solid color, one gradient,
// one image — painted in that order (color, then gradient over it, then the
// image on top), so a stack is representable exactly when it reads
// [solid…, gradient?, image?] bottom→top. This helper consumes that prefix
// and reports everything else as a structured diagnostic instead of the old
// behavior (first visible paint wins, the rest vanish silently).

export interface LoweredPaintDiagnostic {
  severity: "info" | "warning";
  code: string;
  kind: "unsupported_property" | "capture_partial";
  message: string;
}

export interface LoweredPaintStack {
  /// Leading (bottom) run of SOLID paints, composited source-over in stack
  /// order with each paint's own opacity folded in — exact under NORMAL blend.
  backgroundColor?: string;
  /// First gradient above the solid run. Linear lowers to CSS; radial /
  /// angular / diamond keep the existing flatten-with-diagnostic behavior
  /// (handled by the caller, which owns the CSS conversion policy).
  gradientPaint?: GradientPaint;
  /// First IMAGE paint above the gradient slot.
  imagePaint?: ImagePaint;
  diagnostics: LoweredPaintDiagnostic[];
}

/// Composite a run of SOLID paints the way Figma paints them: array order,
/// index 0 at the bottom, each source-over the result so far. A paint's own
/// `opacity` is its alpha (Plugin API solid colors are RGB). Mirrors
/// compositeSolids in tools/import-design/fig/scene.mjs.
export function compositeSolidPaints(solids: readonly SolidPaint[]): string {
  if (solids.length === 1) return paintToColor(solids[0]);
  let r = 0, g = 0, b = 0, a = 0; // accumulated, non-premultiplied
  for (const p of solids) {
    const sa = p.opacity !== undefined ? p.opacity : 1;
    if (sa <= 0) continue;
    const na = sa + a * (1 - sa);
    if (na <= 0) continue;
    r = (p.color.r * sa + r * a * (1 - sa)) / na;
    g = (p.color.g * sa + g * a * (1 - sa)) / na;
    b = (p.color.b * sa + b * a * (1 - sa)) / na;
    a = na;
  }
  return rgbaToCss({ r, g, b, a });
}

const GRADIENT_TYPES = new Set([
  "GRADIENT_LINEAR", "GRADIENT_RADIAL", "GRADIENT_ANGULAR", "GRADIENT_DIAMOND",
]);

export function lowerFillPaints(fills: readonly Paint[]): LoweredPaintStack {
  const out: LoweredPaintStack = { diagnostics: [] };
  const visible = fills.filter((p) => p.visible !== false);
  if (visible.length === 0) return out;

  // Newer paint families (VIDEO, PATTERN, …) have no color to lower — dispatch
  // them out explicitly so the loss is stated, and keep lowering whatever
  // supported paints remain (better than letting an un-renderable top paint
  // shadow a perfectly good solid below it).
  const supported: Paint[] = [];
  const unsupportedTypes: string[] = [];
  for (const p of visible) {
    if (p.type === "SOLID" || p.type === "IMAGE" || GRADIENT_TYPES.has(p.type)) supported.push(p);
    else unsupportedTypes.push(p.type);
  }
  if (unsupportedTypes.length > 0) {
    out.diagnostics.push({
      severity: "warning",
      code: "unsupported-paint-type",
      kind: "unsupported_property",
      message: `Unsupported paint type(s) ${unsupportedTypes.join(", ")} dropped; ` +
        `only solid / gradient / image fills are lowered.`,
    });
  }

  // Paint-level blend modes have no slot in the one-background model; the
  // paint still lowers, composited NORMAL, and the difference is stated.
  const blendModes = [...new Set(
    visible
      .map((p) => p.blendMode)
      .filter((m): m is BlendMode => m !== undefined && m !== "NORMAL"),
  )];
  if (blendModes.length > 0) {
    out.diagnostics.push({
      severity: "warning",
      code: "paint-blend-unsupported",
      kind: "unsupported_property",
      message: `Paint blend mode(s) ${blendModes.join(", ")} composited as NORMAL.`,
    });
  }

  // A fully opaque solid hides everything below it, so trimming the stack to
  // start at the LAST opaque solid is exact — no diagnostic owed for the
  // hidden paints. Without this, [gradient, opaque solid] would lower the
  // gradient and flatten the solid that actually covers it.
  for (let k = supported.length - 1; k > 0; k--) {
    const p = supported[k];
    if (p.type === "SOLID" && (p.opacity === undefined || p.opacity >= 1)) {
      supported.splice(0, k);
      break;
    }
  }

  // Slot scan, bottom→top: [solid… , gradient?, image?].
  let i = 0;
  const solids: SolidPaint[] = [];
  while (i < supported.length && supported[i].type === "SOLID") {
    solids.push(supported[i] as SolidPaint);
    i++;
  }
  if (solids.length > 0) out.backgroundColor = compositeSolidPaints(solids);
  if (i < supported.length && GRADIENT_TYPES.has(supported[i].type)) {
    out.gradientPaint = supported[i] as GradientPaint;
    i++;
  }
  if (i < supported.length && supported[i].type === "IMAGE") {
    out.imagePaint = supported[i] as ImagePaint;
    i++;
  }
  if (i < supported.length) {
    const extras = supported.slice(i).map((p) => p.type);
    out.diagnostics.push({
      severity: "warning",
      code: "multi-paint-flattened",
      kind: "capture_partial",
      message: `${extras.length} of ${supported.length} visible fill(s) ` +
        `(${extras.join(", ")}) exceed the color/gradient/image background slots ` +
        `and are flattened out; the stack renders from the lower paints only.`,
    });
  }
  return out;
}

/// Figma IMAGE-fill scale mode → CSS object-fit for image-shaped nodes.
/// FILL crops to cover the box (CSS cover); FIT letterboxes (CSS contain);
/// CROP shows a transform-defined window — cover is the closest
/// aspect-preserving approximation; TILE repeats at a scale factor, which the
/// image painter cannot do, so the stretch default stands. The second tuple
/// member says whether the mapping is exact (no diagnostic needed).
export function scaleModeToObjectFit(
  scaleMode: string | undefined,
): { fit?: string; exact: boolean } {
  switch (scaleMode) {
    case undefined:
    case "FILL": return { fit: "cover", exact: true };
    case "FIT":  return { fit: "contain", exact: true };
    case "CROP": return { fit: "cover", exact: false };
    case "TILE": return { exact: false };
    default:     return { exact: false };
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Type and layout mapping — Plugin API enums → envelope strings.
//
// Dispatch is EXHAUSTIVE against the pinned `@figma/plugin-typings` SceneNode
// union (version pinned in package.json). The old shape was a switch with
// `default: return "frame"`, which made every unsupported node — FigJam
// stickies, Slides rows, SLICE export regions — look like a successfully
// imported empty frame. Silent success is the failure mode this replaces:
// every family now either dispatches to a real envelope type, is skipped
// with a diagnostic, or falls back to `frame` WITH a diagnostic saying so.

/// A diagnostic the dispatch decision wants raised. `code`/`kind` follow the
/// ExtractedDiagnostic vocabulary (extract-model.ts); the walker owns the
/// path, so only the message rides here.
export interface NodeDispatchDiagnostic {
  severity: "info" | "warning";
  code: string;
  kind: "unsupported_node" | "capture_partial";
  message: string;
}

/// The dispatch decision for one Figma node type. `skip` means the node must
/// not reach the envelope at all (its diagnostic is mandatory — a skip
/// without a diagnostic would be the silent drop this contract forbids).
export type NodeDispatch =
  | { action: "emit"; type: string; diagnostic?: NodeDispatchDiagnostic }
  | { action: "skip"; diagnostic: NodeDispatchDiagnostic };

// FigJam/editor collaborative families plus Slides families. Out of scope for
// an audio-plugin UI importer: none of these carries design content a plugin
// editor should render, and emitting them as empty generic frames made a
// dropped node look imported. `TABLE_CELL` is the REST spelling (the Python
// port shares this list shape); the plugin API only surfaces TABLE.
const SKIPPED_NODE_TYPES = new Set([
  "STICKY",
  "CONNECTOR",
  "SHAPE_WITH_TEXT",
  "CODE_BLOCK",
  "STAMP",
  "WIDGET",
  "EMBED",
  "LINK_UNFURL",
  "MEDIA",
  "HIGHLIGHT",
  "WASHI_TAPE",
  "TABLE",
  "TABLE_CELL",
  "SLIDE",
  "SLIDE_ROW",
  "SLIDE_GRID",
  "INTERACTIVE_SLIDE_ELEMENT",
]);

/// Exhaustive node-type dispatch. `name` only feeds diagnostic messages.
/// `REGULAR_POLYGON` is accepted alongside `POLYGON` so the Python REST port
/// (which sees the REST spelling) can mirror this table field-for-field.
export function dispatchNodeType(figmaType: string, name: string): NodeDispatch {
  switch (figmaType) {
    case "FRAME":
    case "GROUP":
    case "SECTION":
    // TRANSFORM_GROUP is a legitimate container (a group with a shared
    // transform) — it renders fine as a frame; the point of the explicit case
    // is that it no longer reaches the unknown-type fallback.
    case "TRANSFORM_GROUP":
      return { action: "emit", type: "frame" };
    case "COMPONENT":
    case "COMPONENT_SET":
    case "INSTANCE":
      return { action: "emit", type: "frame" }; // recognized instances are promoted to widget kinds later
    case "TEXT":
      return { action: "emit", type: "text" };
    // TEXT_PATH carries real `characters`; dropping it would lose copy. The
    // on-path layout has no envelope representation, so the glyphs land as a
    // normal straight-baseline text run — content preserved, layout diagnosed.
    case "TEXT_PATH":
      return {
        action: "emit",
        type: "text",
        diagnostic: {
          severity: "warning",
          code: "text-path-flattened",
          kind: "capture_partial",
          message: `TEXT_PATH "${name}": text-on-path layout flattened to a straight text run; characters preserved.`,
        },
      };
    // An ELLIPSE is a circle, not a box. "Has a fill means renderable" holds for
    // a RECTANGLE (a frame paints its own background box) and is FALSE for a
    // circle: codegen has no painter for one, so a filled ellipse typed `frame`
    // paints a SQUARE. The IR already has `ellipse` (is_synthesizable_primitive)
    // and synthesize_node gives it a real path — this extractor just never said
    // what the node was. Mirrors the .fig lane's envelopeType (fig/scene.mjs)
    // and the REST lane's dispatch_node_type (figma_rest_export.py).
    //
    // STAR / POLYGON need no equivalent case: extract.ts's isVectorLike captures
    // them as PNG assets (type `image`) before their frame typing can matter.
    // They reach this mapping only when that export fails, which diagnoses itself.
    case "ELLIPSE":
      return { action: "emit", type: "ellipse" };
    case "RECTANGLE":
    case "POLYGON":
    case "REGULAR_POLYGON":
    case "STAR":
    case "LINE":
      return { action: "emit", type: "frame" };
    case "VECTOR":
    case "BOOLEAN_OPERATION":
      return { action: "emit", type: "vector" };
    // A SLICE is an export region — it paints NOTHING in Figma, so emitting it
    // as a frame invented a box the design never had. Skipping is the correct
    // rendering; the diagnostic keeps the removal visible.
    case "SLICE":
      return {
        action: "skip",
        diagnostic: {
          severity: "warning",
          code: "slice-skipped",
          kind: "unsupported_node",
          message: `SLICE "${name}" skipped: an export region paints nothing and emits no node.`,
        },
      };
    // A SLOT is a component-system placeholder. A bare import has no slot
    // content to substitute, so it dispatches as an (empty) frame and says so.
    case "SLOT":
      return {
        action: "emit",
        type: "frame",
        diagnostic: {
          severity: "warning",
          code: "slot-placeholder",
          kind: "unsupported_node",
          message: `SLOT "${name}": component slot placeholder imported as an empty frame; slot content is not resolved.`,
        },
      };
    default:
      if (SKIPPED_NODE_TYPES.has(figmaType)) {
        return {
          action: "skip",
          diagnostic: {
            severity: "warning",
            code: "unsupported-node",
            kind: "unsupported_node",
            message: `${figmaType} "${name}" skipped: editor-specific node family outside the audio-plugin UI import scope.`,
          },
        };
      }
      // A type this table has never heard of (a Figma family newer than the
      // pinned typings). Fall back to `frame` so the import never crashes,
      // but say so — the fallback is honest, not silent.
      return {
        action: "emit",
        type: "frame",
        diagnostic: {
          severity: "warning",
          code: "unknown-node-type",
          kind: "unsupported_node",
          message: `Unknown Figma node type ${figmaType} ("${name}") imported as a generic frame; update the dispatch table for the current plugin typings.`,
        },
      };
  }
}

/// Envelope type for a node dispatch already known to emit. Kept for callers
/// (and tests) that only need the type string; anything that can encounter a
/// skippable or diagnosable node must go through dispatchNodeType instead.
export function mapNodeType(n: SceneNode): string {
  const d = dispatchNodeType(n.type, n.name ?? "");
  // A skipped family has no envelope type; `frame` here is unreachable from
  // extract.ts (its walker skips before asking) and exists only to keep this
  // convenience accessor total.
  return d.action === "emit" ? d.type : "frame";
}

export function mapPrimaryAxisAlign(v: FrameNode["primaryAxisAlignItems"]): ExtractedLayout["justify"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "SPACE_BETWEEN": return "space_between";
    default: return "flex_start";
  }
}

export function mapCounterAxisAlign(v: FrameNode["counterAxisAlignItems"]): ExtractedLayout["align"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "BASELINE": return "flex_start"; // Pulp Yoga doesn't model baseline; closest fallback
    default: return "stretch";
  }
}

export function mapAxisSize(v: FrameNode["layoutSizingHorizontal"]): ExtractedLayout["width_mode"] {
  switch (v) {
    case "HUG": return "hug";
    case "FILL": return "fill";
    case "FIXED":
    default: return "fixed";
  }
}

/// The parent's layout mode when (and only when) it lays its children out:
/// "HORIZONTAL" / "VERTICAL" (flex) or "GRID" (cell placement). null for
/// plain frames, groups, and non-container parents, whose children are
/// positioned absolutely in the parent's coordinate space.
export function parentLayoutMode(parent: SceneNode | null): "HORIZONTAL" | "VERTICAL" | "GRID" | null {
  if (!parent) return null;
  if (parent.type !== "FRAME" && parent.type !== "COMPONENT" && parent.type !== "INSTANCE" && parent.type !== "COMPONENT_SET") {
    return null;
  }
  const mode = (parent as FrameNode).layoutMode;
  return mode === "HORIZONTAL" || mode === "VERTICAL" || mode === "GRID" ? mode : null;
}

/// Auto-layout extraction — the container's own layout AND the properties
/// this node carries as a CHILD of an auto-layout parent (layoutGrow /
/// layoutAlign / grid cell placement), which is why the parent is a
/// parameter: those fields are meaningless — and Figma leaves them stale —
/// outside an auto-layout parent, so they must be gated on parent context,
/// not just present on the node.
export function extractLayout(n: SceneNode, parent: SceneNode | null): ExtractedLayout {
  const l: ExtractedLayout = {};
  const isContainer =
    n.type === "FRAME" || n.type === "COMPONENT" || n.type === "INSTANCE" || n.type === "COMPONENT_SET";
  if (isContainer) {
    const f = n as FrameNode;
    if (f.layoutMode === "HORIZONTAL" || f.layoutMode === "VERTICAL") {
      l.display = "flex";
      l.direction = f.layoutMode === "HORIZONTAL" ? "row" : "column";
      l.gap = f.itemSpacing ?? 0;
      l.padding = {
        top: f.paddingTop ?? 0,
        right: f.paddingRight ?? 0,
        bottom: f.paddingBottom ?? 0,
        left: f.paddingLeft ?? 0,
      };
      l.justify = mapPrimaryAxisAlign(f.primaryAxisAlignItems);
      l.align = mapCounterAxisAlign(f.counterAxisAlignItems);
      l.wrap = f.layoutWrap === "WRAP";
      if (l.wrap) {
        // counterAxisSpacing is the gap BETWEEN wrapped tracks — cross-axis,
        // so a row's tracks stack vertically (rowGap) and a column's
        // horizontally (columnGap). AUTO align-content is the default
        // packing; only SPACE_BETWEEN changes distribution.
        if (typeof f.counterAxisSpacing === "number") {
          if (f.layoutMode === "HORIZONTAL") l.rowGap = f.counterAxisSpacing;
          else l.columnGap = f.counterAxisSpacing;
        }
        if (f.counterAxisAlignContent === "SPACE_BETWEEN") l.alignContent = "space-between";
      }
      l.width_mode = mapAxisSize(f.layoutSizingHorizontal);
      l.height_mode = mapAxisSize(f.layoutSizingVertical);
    } else if (f.layoutMode === "GRID") {
      // Figma GRID auto-layout → the IR's CSS-grid contract. The Plugin API
      // exposes counts + gaps (uniform tracks), so the template is
      // repeat(N, 1fr); per-child cells arrive as 0-based anchors below.
      l.display = "grid";
      if (typeof f.gridColumnCount === "number" && f.gridColumnCount > 0) {
        l.gridTemplateColumns = `repeat(${f.gridColumnCount}, 1fr)`;
      }
      if (typeof f.gridRowCount === "number" && f.gridRowCount > 0) {
        l.gridTemplateRows = `repeat(${f.gridRowCount}, 1fr)`;
      }
      if (typeof f.gridRowGap === "number") l.rowGap = f.gridRowGap;
      if (typeof f.gridColumnGap === "number") l.columnGap = f.gridColumnGap;
    } else if (f.layoutMode === "NONE" || f.layoutMode === undefined) {
      // Children positioned absolutely — emit no display/direction
      l.width_mode = "fixed";
      l.height_mode = "fixed";
    }
  }

  // Child-side properties — only for a FLOWING child of an auto-layout
  // parent. layoutPositioning ABSOLUTE opts the child out of the stack; it
  // takes the absolute-position + constraints path in walk() instead, and
  // grow/align here would fight that placement.
  const pMode = parentLayoutMode(parent);
  const flowing =
    pMode !== null &&
    !("layoutPositioning" in n && (n as SceneNode & { layoutPositioning?: string }).layoutPositioning === "ABSOLUTE");
  if (flowing && (pMode === "HORIZONTAL" || pMode === "VERTICAL")) {
    const child = n as SceneNode & { layoutGrow?: number; layoutAlign?: string };
    if (typeof child.layoutGrow === "number" && child.layoutGrow > 0) l.flexGrow = child.layoutGrow;
    // INHERIT is the default (follow the parent's counterAxisAlignItems) —
    // exactly what omitting align-self does, so it emits nothing.
    switch (child.layoutAlign) {
      case "STRETCH": l.alignSelf = "stretch"; break;
      case "MIN": l.alignSelf = "flex-start"; break;
      case "MAX": l.alignSelf = "flex-end"; break;
      case "CENTER": l.alignSelf = "center"; break;
      default: break;
    }
  }
  if (flowing && pMode === "GRID") {
    // 0-based anchors + spans → CSS 1-based grid lines.
    const g = n as SceneNode & {
      gridColumnAnchorIndex?: number;
      gridRowAnchorIndex?: number;
      gridColumnSpan?: number;
      gridRowSpan?: number;
    };
    if (typeof g.gridColumnAnchorIndex === "number" && g.gridColumnAnchorIndex >= 0) {
      const span = g.gridColumnSpan ?? 1;
      l.gridColumn = span > 1 ? `${g.gridColumnAnchorIndex + 1} / span ${span}` : `${g.gridColumnAnchorIndex + 1}`;
    }
    if (typeof g.gridRowAnchorIndex === "number" && g.gridRowAnchorIndex >= 0) {
      const span = g.gridRowSpan ?? 1;
      l.gridRow = span > 1 ? `${g.gridRowAnchorIndex + 1} / span ${span}` : `${g.gridRowAnchorIndex + 1}`;
    }
  }

  // targetAspectRatio only constrains an axis the layout can flex — a fully
  // fixed node already carries Figma's solved w/h, and the ratio would fight
  // that over rounding. Flexible = grow, stretch, or a non-FIXED sizing mode.
  const withRatio = n as SceneNode & {
    targetAspectRatio?: { x: number; y: number } | null;
    layoutGrow?: number;
    layoutAlign?: string;
    layoutSizingHorizontal?: string;
    layoutSizingVertical?: string;
  };
  const ar = "targetAspectRatio" in n ? withRatio.targetAspectRatio : null;
  if (ar && typeof ar.x === "number" && typeof ar.y === "number" && ar.x > 0 && ar.y > 0) {
    const flexible =
      (typeof withRatio.layoutGrow === "number" && withRatio.layoutGrow > 0) ||
      withRatio.layoutAlign === "STRETCH" ||
      withRatio.layoutSizingHorizontal === "HUG" || withRatio.layoutSizingHorizontal === "FILL" ||
      withRatio.layoutSizingVertical === "HUG" || withRatio.layoutSizingVertical === "FILL";
    if (flexible) l.aspectRatio = ar.x / ar.y;
  }
  return l;
}

// Resize constraints, passed through in the Plugin API's own spelling
// (MIN/MAX/CENTER/STRETCH/SCALE) — the C++ importer normalizes tokens, so a
// mapping here would just be a second dialect to keep in sync. Not every
// SceneNode type carries constraints (e.g. groups, slices), hence the
// property-presence guard. Returns undefined rather than an empty object so
// the serializer's truthy-passthrough never emits `constraints: {}`.
export function extractConstraints(
  node: SceneNode,
): { horizontal?: string; vertical?: string } | undefined {
  if (!("constraints" in node)) return undefined;
  const c = (node as SceneNode & { constraints?: Constraints }).constraints;
  if (!c) return undefined;
  const out: { horizontal?: string; vertical?: string } = {};
  if (typeof c.horizontal === "string") out.horizontal = c.horizontal;
  if (typeof c.vertical === "string") out.vertical = c.vertical;
  return out.horizontal || out.vertical ? out : undefined;
}

// Variable bindings (`node.boundVariables`) resolved to canonical token names.
// The Plugin API shape is `{ [property]: VariableAlias | VariableAlias[] |
// { [key]: VariableAlias } }` where a VariableAlias is `{ type:
// "VARIABLE_ALIAS", id }`: array-valued properties (fills, strokes, effects)
// carry one alias per entry, and a few properties (componentProperties,
// textRangeFills) nest a keyed map of aliases. Output is the flat
// `{ property: tokenName }` map the envelope's `figma.bound_variables` field
// carries: a single alias keeps the bare property key, an array binds index 0
// to the bare key and later entries to "<property>.<i>", and a nested map
// binds "<property>.<key>" — flat string→string so the C++ consumer can
// preserve each binding as one namespaced attribute.
//
// An id the token pass didn't capture (remote-library or deleted variable) is
// NOT emitted — a dangling reference that resolves to nothing downstream is
// worse than an honest skip — and is returned in `unresolved` for the caller
// to diagnose.
export interface BoundVariableBindings {
  bindings: Record<string, string>;
  unresolved: string[];
}

export function extractBoundVariableBindings(
  bound: unknown,
  idToName: Record<string, string>,
): BoundVariableBindings | undefined {
  if (!bound || typeof bound !== "object") return undefined;
  const out: BoundVariableBindings = { bindings: {}, unresolved: [] };

  const aliasId = (v: unknown): string | undefined => {
    if (v && typeof v === "object" && (v as VariableAlias).type === "VARIABLE_ALIAS" &&
        typeof (v as VariableAlias).id === "string") {
      return (v as VariableAlias).id;
    }
    return undefined;
  };
  const bind = (key: string, id: string) => {
    const name = idToName[id];
    if (name) out.bindings[key] = name;
    else out.unresolved.push(id);
  };

  for (const [property, value] of Object.entries(bound as Record<string, unknown>)) {
    if (value === null || value === undefined) continue;
    const single = aliasId(value);
    if (single) {
      bind(property, single);
      continue;
    }
    if (Array.isArray(value)) {
      for (let i = 0; i < value.length; i++) {
        const id = aliasId(value[i]);
        if (id) bind(i === 0 ? property : `${property}.${i}`, id);
      }
      continue;
    }
    if (typeof value === "object") {
      for (const [sub, entry] of Object.entries(value as Record<string, unknown>)) {
        const id = aliasId(entry);
        if (id) bind(`${property}.${sub}`, id);
      }
    }
  }

  return Object.keys(out.bindings).length > 0 || out.unresolved.length > 0 ? out : undefined;
}

// ──────────────────────────────────────────────────────────────────────────
// Library recognition fallback — name-based heuristic when the
// component_set_key match doesn't hit (e.g. unpublished local
// components, or designs that use the audio widget visual without
// installing the published library).

// Whole-word tokenizer mirroring the C++ tokenize_name (design_import.cpp): split
// on non-alphanumerics AND camelCase / acronym / digit boundaries, lowercased.
// "VUMeter" -> [vu, meter]; "Knob_1" -> [knob, 1]; "Dialog" -> [dialog].
export function tokenizeName(name: string): string[] {
  const tokens: string[] = [];
  let cur = "";
  const flush = () => { if (cur) { tokens.push(cur); cur = ""; } };
  for (let i = 0; i < name.length; i++) {
    const c = name.charAt(i);
    if (!/[a-z0-9]/i.test(c)) { flush(); continue; }
    if (cur) {
      const p = name.charAt(i - 1);
      const next = i + 1 < name.length ? name.charAt(i + 1) : "";
      let boundary = false;
      if (/[a-z]/.test(p) && /[A-Z]/.test(c)) boundary = true;                            // aB -> a|B
      else if (/[A-Z]/.test(p) && /[A-Z]/.test(c) && /[a-z]/.test(next)) boundary = true; // ABc -> A|Bc
      else if (/[0-9]/.test(p) !== /[0-9]/.test(c)) boundary = true;                      // a1 / 1a
      if (boundary) flush();
    }
    cur += c.toLowerCase();
  }
  flush();
  return tokens;
}

// Recognize an audio widget by WHOLE-WORD name tokens (not substrings), mirroring
// the C++ detect_audio_widget. The old substring match promoted any name
// *containing* "dial"/"meter"/… — so "Dialog"/"Radial" became knobs and
// "Parameter"/"Diameter" became meters. Token matching (tolerant of a simple
// English plural, as the C++ `has` is) fixes those while keeping "xy_pad"/"XYPad"
// and acronym names like "VUMeter".
export function audioWidgetKindFromName(name: string): AudioWidgetKind | undefined {
  const toks: { [t: string]: true } = {};
  const list = tokenizeName(name);
  for (let i = 0; i < list.length; i++) toks[list[i]] = true;
  const has = (w: string) => toks[w] === true || toks[w + "s"] === true;
  if (has("knob") || has("dial")) return "knob";
  if (has("fader") || has("slider")) return "fader";
  if (has("meter") || has("level") || has("vu")) return "meter";
  if (has("xypad") || (has("xy") && has("pad"))) return "xy_pad";
  if (has("waveform") || has("oscilloscope")) return "waveform";
  if (has("spectrum") || has("analyzer") || has("analyser")) return "spectrum";
  return undefined;
}

// ──────────────────────────────────────────────────────────────────────────
// Vector-illustration detection — heuristic, used to flatten an entire
// shape illustration frame to a single SVG export instead of walking
// each primitive. Recursive but bounded by tree depth.

/// Returns true if the node and all its recursive descendants are
/// vector-like primitives (or empty frames wrapping them).
export function isPureVectorIllustration(node: SceneNode): boolean {
  if (!("children" in node)) return false;
  const children = (node as ChildrenMixin).children;
  if (children.length === 0) return false;
  for (const child of children) {
    const t = child.type;
    if (
      t === "VECTOR" ||
      t === "BOOLEAN_OPERATION" ||
      t === "LINE" ||
      t === "STAR" ||
      t === "POLYGON" ||
      t === "ELLIPSE" ||
      t === "RECTANGLE"
    ) {
      continue;
    }
    if (t === "FRAME" || t === "GROUP") {
      if (!isPureVectorIllustration(child)) return false;
      continue;
    }
    // text, instance, image, anything else → not a pure illustration
    return false;
  }
  return true;
}

// ──────────────────────────────────────────────────────────────────────────
// Font catalog — walks the post-extraction IR (not the Figma scene),
// so it's already host-neutral and operates only on ExtractedFigmaNode
// trees. Emitted as the envelope's top-level `font_family_assets`.

export function collectFontFamilyAssets(roots: ExtractedFigmaNode[]): FontFamilyAsset[] {
  const seen = new Map<string, FontFamilyAsset>();
  function visit(n: ExtractedFigmaNode): void {
    const family = n.style.font_family;
    if (family) {
      // Figma's `fontName.style` (already captured by extractTextStyle)
      // lives on `style.font_style` as either "normal" or "italic" today;
      // the verbose style string ("Semi Bold", "Bold Italic") is not yet
      // surfaced. Emit what we have and let the runtime resolve.
      const styleField = (n.style.font_style as string | undefined) ?? "Regular";
      const weight = n.style.font_weight;
      const italic = styleField === "italic" || /italic/i.test(styleField);
      const key = `${family}|${styleField}|${weight ?? ""}|${italic ? "i" : ""}`;
      if (!seen.has(key)) {
        const row: FontFamilyAsset = { family, style: styleField };
        if (typeof weight === "number") row.weight = weight;
        if (italic) row.italic = true;
        seen.set(key, row);
      }
    }
    for (const c of n.children) visit(c);
  }
  for (const r of roots) visit(r);
  return Array.from(seen.values());
}

// ──────────────────────────────────────────────────────────────────────────
// Text-run offset conversion — the Figma Plugin API indexes styled text
// segments in UTF-16 code units (a BMP char is 1 unit, an astral char like an
// emoji is a surrogate pair = 2 units), while the IR text-run contract is
// UTF-8 BYTE offsets into `content` (the native slicer is byte-based; see
// design_ir.hpp::IRTextRun). Mirrors the REST port's u16_to_byte map in
// figma_rest_export.py::extract_text_runs. No TextEncoder — the Figma plugin
// sandbox doesn't provide one, so byte widths come from the code points.

/// Map from UTF-16 code-unit index → UTF-8 byte offset, with a past-the-end
/// sentinel at index s.length (so `map[s.length]` is the total byte count).
export function utf16ToUtf8ByteOffsets(s: string): number[] {
  const map: number[] = [];
  let byte = 0;
  for (const ch of s) {          // iterates by code point
    const cp = ch.codePointAt(0) as number;
    const units = cp > 0xffff ? 2 : 1;
    const bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    for (let k = 0; k < units; k++) map.push(byte);
    byte += bytes;
  }
  map.push(byte);
  return map;
}
