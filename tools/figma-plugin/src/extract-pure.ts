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

import type { ExtractedFigmaNode, ExtractedLayout, ExtractedStyle } from "./extract-model";
import type { AudioWidgetKind, ExtractedDiagnostic } from "./extract-model";
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
// Layer blend mode — the shared supported-blend table.
//
// Mirrors fig/scene.mjs::FIGMA_BLEND_CSS and figma_rest_export.py::
// _FIGMA_BLEND_CSS: every listed Figma mode is a real CSS mix-blend-mode
// value, lowered by spelling transform (UPPER_SNAKE → lowercase-hyphen). The
// consumer side of the same table is design_ir_json.cpp::
// is_supported_blend_keyword. LINEAR_BURN and LINEAR_DODGE are absent on
// purpose (see the .fig lane's table comment for the full reasoning):
// LINEAR_DODGE's natural spelling has no verified wiring, and LINEAR_BURN's
// (`plus-darker`) maps to the ADDITIVE kPlus in Skia/Chromium, which would
// LIGHTEN a layer the designer asked to darken. Unmappable modes lower to
// nothing WITH a `blend-unsupported` diagnostic — a blend that is silently
// ignored still paints, ~25/255 too bright in the file that motivated this.

/// Blend modes that mean "just composite it" — never emitted, never diagnosed.
export const BLEND_IS_DEFAULT = new Set<string>(["NORMAL", "PASS_THROUGH"]);

/// Figma blend mode → CSS mix-blend-mode, for the modes CSS has.
export const FIGMA_BLEND_CSS = new Set<string>([
  "DARKEN", "MULTIPLY", "COLOR_BURN", "LIGHTEN", "SCREEN", "COLOR_DODGE",
  "OVERLAY", "SOFT_LIGHT", "HARD_LIGHT", "DIFFERENCE", "EXCLUSION",
  "HUE", "SATURATION", "COLOR", "LUMINOSITY",
]);

export interface LoweredLayerBlend {
  /// CSS mix-blend-mode keyword, present only for supported non-default modes.
  css?: string;
  /// Present when the mode has no CSS equivalent — the caller pushes it.
  diagnostic?: Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message">;
}

/// Lower a node-level blend mode: supported modes become the CSS keyword,
/// defaults lower to nothing, and everything else raises `blend-unsupported`.
export function lowerLayerBlendMode(mode: string | undefined): LoweredLayerBlend {
  if (mode === undefined || BLEND_IS_DEFAULT.has(mode)) return {};
  if (FIGMA_BLEND_CSS.has(mode)) {
    return { css: mode.toLowerCase().replace(/_/g, "-") };
  }
  return {
    diagnostic: {
      severity: "warning",
      code: "blend-unsupported",
      kind: "unsupported_property",
      message: `${mode} is not lowered; composited normally.`,
    },
  };
}

/// True when any node in the subtree rooted at `n` (inclusive) carries a
/// CSS-lowerable blend mode — the condition under which a missing isolation
/// layer changes pixels.
function subtreeHasLoweredBlend(n: ExtractedFigmaNode): boolean {
  if (n.style.mix_blend_mode !== undefined) return true;
  return n.children.some(subtreeHasLoweredBlend);
}

/// Figma GROUP/FRAME nodes default to PASS_THROUGH — children composite
/// against the backdrop, which is exactly the default web/native behavior, so
/// dropping it is correct and silent. An EXPLICIT NORMAL on a container is
/// different: it is Figma's "isolate" (children blend within the group, the
/// group composites normally — CSS `isolation: isolate`), and the flat lowering
/// has no isolation layer. That only changes pixels when something in the
/// subtree actually blends, so the diagnostic is gated on that. (A container
/// with a non-default blend mode needs no diagnostic: CSS mix-blend-mode
/// itself forms an isolated group, matching Figma.) Post-order pass over the
/// extracted tree; call on each root after extraction.
export function collectGroupIsolationDiagnostics(
  root: ExtractedFigmaNode,
): Array<Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message"> & { path: string }> {
  const out: Array<Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message"> & { path: string }> = [];
  const visit = (n: ExtractedFigmaNode): void => {
    if (
      n.blend_mode === "NORMAL" &&
      n.children.length > 0 &&
      n.children.some(subtreeHasLoweredBlend)
    ) {
      out.push({
        severity: "warning",
        code: "group-isolation-approximated",
        kind: "capture_partial",
        message: `${n.name}: isolate group (explicit NORMAL) has blending descendants; ` +
          `imported without an isolation layer, so they blend against the full backdrop.`,
        path: n.figma_node_id,
      });
    }
    for (const c of n.children) visit(c);
  };
  visit(root);
  return out;
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

// ──────────────────────────────────────────────────────────────────────────
// Strokes → box-border style + preserved provenance + diagnostics.

/// One extracted stroke: the style fields to merge into ExtractedStyle, the
/// namespaced figma:* attributes to merge into node attributes, and the
/// diagnostics the caller pushes (extract-pure stays host-neutral, so it
/// returns them instead of reaching for a WalkCtx).
export interface ExtractedStroke {
  style: Partial<ExtractedStyle>;
  attributes?: Record<string, string>;
  diagnostics: Array<Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message">>;
}

// The node's strokes lowered to Pulp's box-border contract.
//
//   - Uniform weight → the existing `border` shorthand + discrete fields.
//   - Per-side weights (IndividualStrokesMixin: strokeTopWeight …, active when
//     the four sides differ) → border_{top,right,bottom,left}_width with the
//     single Figma stroke color repeated on each painted side; no shorthand,
//     and an explicit 0 side stays 0 (it paints nothing).
//   - A non-empty dashPattern → border_style "dashed" (a box border cannot
//     express the exact array; it is preserved as figma:dash_pattern).
//   - Multiple visible paints / a non-solid top paint flatten to the FIRST
//     SOLID paint with a multi-paint-stroke / complex-stroke-flattened
//     diagnostic — never silently.
//   - strokeAlign (CENTER/OUTSIDE — INSIDE is how a box border already
//     paints), strokeCap, strokeJoin, strokeMiterLimit are preserved as
//     namespaced attributes for path renderers / fidelity tooling; the
//     faithful-SVG capture path bakes them into geometry, so nothing consumes
//     them for box borders (documented in compat/imports.json).
//
// Returns undefined when the node has no visible stroke to state.
export function extractStrokeStyle(n: SceneNode): ExtractedStroke | undefined {
  if (!("strokes" in n) || !Array.isArray(n.strokes) || n.strokes.length === 0) return undefined;
  const visible = (n.strokes as readonly Paint[]).filter((p) => p.visible !== false);
  if (visible.length === 0) return undefined;

  const out: ExtractedStroke = { style: {}, diagnostics: [] };
  const diag = (code: string, message: string) => {
    out.diagnostics.push({ severity: "warning", code, kind: "capture_partial", message });
  };

  // Provenance attributes ride even when the border itself is dropped — the
  // loss report and the preserved data belong together.
  const attrs: Record<string, string> = {};
  const dashes =
    "dashPattern" in n && Array.isArray(n.dashPattern)
      ? (n.dashPattern as readonly number[]).filter((v) => typeof v === "number" && v > 0)
      : [];
  if (dashes.length) attrs["figma:dash_pattern"] = dashes.join(",");
  const align = "strokeAlign" in n ? (n as MinimalStrokesMixin).strokeAlign : undefined;
  if (align === "CENTER" || align === "OUTSIDE") attrs["figma:stroke_align"] = align.toLowerCase();
  const cap = "strokeCap" in n ? (n as SceneNode & { strokeCap?: unknown }).strokeCap : undefined;
  if (typeof cap === "string" && cap !== "NONE") attrs["figma:stroke_cap"] = cap.toLowerCase();
  const join = "strokeJoin" in n ? (n as SceneNode & { strokeJoin?: unknown }).strokeJoin : undefined;
  if (typeof join === "string" && join !== "MITER") attrs["figma:stroke_join"] = join.toLowerCase();
  const miter =
    "strokeMiterLimit" in n ? (n as SceneNode & { strokeMiterLimit?: unknown }).strokeMiterLimit : undefined;
  if (typeof miter === "number" && Math.abs(miter - 4) > 1e-6) {
    attrs["figma:stroke_miter_limit"] = String(Math.round(miter * 100) / 100);
  }
  if (Object.keys(attrs).length > 0) out.attributes = attrs;

  const firstSolid = visible.find((p) => p.type === "SOLID") as SolidPaint | undefined;
  if (visible.length > 1) {
    diag("multi-paint-stroke",
      `"${n.name}": ${visible.length} visible stroke paints; a box border carries one — flattened to the first solid.`);
  }
  if (!firstSolid) {
    diag("complex-stroke-flattened",
      `"${n.name}": ${visible[0].type} stroke has no solid paint to flatten to; the stroke is dropped.`);
    return out;
  }
  if (visible[0] !== firstSolid) {
    diag("complex-stroke-flattened",
      `"${n.name}": ${visible[0].type} top stroke paint is not expressible as a box border; flattened to the first solid.`);
  }

  const color = paintToColor(firstSolid);
  const styleWord = dashes.length ? "dashed" : "solid";
  const sides =
    "strokeTopWeight" in n &&
    typeof (n as IndividualStrokesMixin).strokeTopWeight === "number" &&
    typeof (n as IndividualStrokesMixin).strokeRightWeight === "number" &&
    typeof (n as IndividualStrokesMixin).strokeBottomWeight === "number" &&
    typeof (n as IndividualStrokesMixin).strokeLeftWeight === "number"
      ? {
          top: (n as IndividualStrokesMixin).strokeTopWeight,
          right: (n as IndividualStrokesMixin).strokeRightWeight,
          bottom: (n as IndividualStrokesMixin).strokeBottomWeight,
          left: (n as IndividualStrokesMixin).strokeLeftWeight,
        }
      : undefined;
  const perSide =
    sides !== undefined &&
    !(sides.top === sides.right && sides.right === sides.bottom && sides.bottom === sides.left);
  if (perSide) {
    out.style.border_color = color;
    out.style.border_style = styleWord;
    out.style.border_top_width = sides.top;
    out.style.border_right_width = sides.right;
    out.style.border_bottom_width = sides.bottom;
    out.style.border_left_width = sides.left;
    if (sides.top > 0) out.style.border_top_color = color;
    if (sides.right > 0) out.style.border_right_color = color;
    if (sides.bottom > 0) out.style.border_bottom_color = color;
    if (sides.left > 0) out.style.border_left_color = color;
  } else {
    // Uniform: the four equal side weights are the weight when present
    // (strokeWeight reads figma.mixed exactly when they differ); otherwise
    // strokeWeight itself.
    const weight = sides
      ? sides.top
      : "strokeWeight" in n && typeof n.strokeWeight === "number"
        ? n.strokeWeight
        : 1;
    if (weight > 0) {
      out.style.border = `${weight}px ${styleWord} ${color}`;
      out.style.border_color = color;
      out.style.border_width = weight;
      out.style.border_style = styleWord;
    }
  }
  return out;
}

// Primitive-shape provenance the raster capture cannot carry, preserved as
// namespaced figma:* attributes (same contract as the stroke provenance
// above): the fields a future path renderer needs to rebuild the primitive
// without a re-export from Figma. Nothing consumes these yet — vector-like
// leaves rasterize to PNG and the faithful-SVG capture bakes the final
// outline — so they are provenance for path renderers and fidelity tooling
// (tracked in compat/imports.json).
//
//   - ELLIPSE arcData (radians, clockwise from the x axis; innerRadius 0..1)
//     → figma:arc_data "start,end,inner", only when the sweep is not a plain
//     full circle or the ellipse is a donut. A full circle IS the default —
//     emitting it on every ellipse would bloat envelopes with noise.
//   - STAR pointCount / innerRadius (0..1 spike ratio)
//     → figma:star_point_count / figma:star_inner_radius, always: both are
//     required to rebuild the star, whatever their values.
//   - POLYGON pointCount → figma:polygon_point_count, always (REST spells the
//     node type REGULAR_POLYGON; the attribute key is shared).
//   - cornerSmoothing (0..1 squircle factor, any cornered node) →
//     figma:corner_smoothing, only when > 0 — the per-corner radii already
//     ride in style, so only the non-default smoothing needs preserving.
//   - BOOLEAN_OPERATION booleanOperation → figma:boolean_operation
//     (union/intersect/subtract/exclude, lowercased like the stroke attrs).
//
// Returns undefined when the node carries no primitive metadata to state.
const TWO_PI = Math.PI * 2;

/// Up-to-4-decimal formatting with trailing zeros trimmed, so attr strings
/// stay stable across float widths (kiwi float32 vs Plugin API double).
function fmtGeomNum(v: number): string {
  return String(Math.round(v * 10000) / 10000);
}

export function extractPrimitiveGeometryAttrs(n: SceneNode): Record<string, string> | undefined {
  const attrs: Record<string, string> = {};

  if (n.type === "ELLIPSE" && n.arcData) {
    const { startingAngle, endingAngle, innerRadius } = n.arcData;
    const fullCircle = Math.abs(endingAngle - startingAngle) >= TWO_PI - 1e-4;
    if (!fullCircle || innerRadius > 1e-4) {
      attrs["figma:arc_data"] =
        `${fmtGeomNum(startingAngle)},${fmtGeomNum(endingAngle)},${fmtGeomNum(innerRadius)}`;
    }
  }

  if (n.type === "STAR") {
    if (typeof n.pointCount === "number") {
      attrs["figma:star_point_count"] = String(n.pointCount);
    }
    if (typeof n.innerRadius === "number") {
      attrs["figma:star_inner_radius"] = fmtGeomNum(n.innerRadius);
    }
  }

  if (n.type === "POLYGON" && typeof n.pointCount === "number") {
    attrs["figma:polygon_point_count"] = String(n.pointCount);
  }

  const smoothing =
    "cornerSmoothing" in n ? (n as SceneNode & { cornerSmoothing?: unknown }).cornerSmoothing : undefined;
  if (typeof smoothing === "number" && smoothing > 0) {
    attrs["figma:corner_smoothing"] = fmtGeomNum(smoothing);
  }

  if (n.type === "BOOLEAN_OPERATION" && typeof n.booleanOperation === "string") {
    attrs["figma:boolean_operation"] = n.booleanOperation.toLowerCase();
  }

  return Object.keys(attrs).length > 0 ? attrs : undefined;
}

// ──────────────────────────────────────────────────────────────────────────
// Dev metadata + export settings → namespaced figma:* provenance attributes.

// Dev-mode metadata and authored export settings, preserved as namespaced
// figma:* attributes (same contract as the stroke/primitive provenance above:
// audit "Dev metadata" and "Export settings" rows). PROVENANCE-ONLY by
// design — nothing renders from these, and export settings never override
// Pulp's deterministic PNG/SVG capture policy; they are asset hints and
// round-trip context for dev tooling (tracked in compat/imports.json).
// Emitted only when present and non-default, so envelopes stay lean:
//
//   - PublishableMixin.description (COMPONENT / COMPONENT_SET only among
//     scene nodes) → figma:description, trimmed, non-empty only.
//   - DevStatusMixin.devStatus ({type: "READY_FOR_DEV" | "COMPLETED"}) →
//     figma:dev_status, lowercased like the other enum attrs
//     ("ready_for_dev" / "completed"). The optional per-status description
//     is not preserved. Typings-gated: guarded with a property check.
//   - AnnotationsMixin.annotations → figma:annotations, a compact JSON array
//     of {label, properties, category_id}; `label` falls back to
//     `labelMarkdown`, `properties` keeps the Plugin API's camelCase
//     vocabulary ("fills", "itemSpacing", ...). Entries with nothing to
//     state are dropped. Typings-gated: guarded with a property check.
//   - ExportMixin.exportSettings → figma:export_settings, a compact JSON
//     array of {format, suffix, constraint, contents_only}: format
//     lowercased ("png" / "jpg" / "svg" / "pdf"), suffix only when
//     non-empty, constraint as "scale:2" / "width:512" / "height:512" only
//     when not the SCALE:1 default, contents_only only when explicitly
//     false (true is Figma's default).
//
// Plugin data / shared plugin data and reactions are deliberately NOT
// preserved — arbitrary third-party payloads are noise in the envelope, and
// prototype wiring is out of the importer's scope.
//
// Returns undefined when the node carries no dev metadata to state.
export function extractDevMetadataAttrs(n: SceneNode): Record<string, string> | undefined {
  const attrs: Record<string, string> = {};

  const desc = "description" in n ? (n as SceneNode & { description?: unknown }).description : undefined;
  if (typeof desc === "string" && desc.trim().length > 0) {
    attrs["figma:description"] = desc.trim();
  }

  const status = "devStatus" in n ? (n as SceneNode & { devStatus?: unknown }).devStatus : undefined;
  if (
    status !== null &&
    typeof status === "object" &&
    typeof (status as { type?: unknown }).type === "string"
  ) {
    attrs["figma:dev_status"] = (status as { type: string }).type.toLowerCase();
  }

  const annotations =
    "annotations" in n ? (n as SceneNode & { annotations?: unknown }).annotations : undefined;
  if (Array.isArray(annotations) && annotations.length > 0) {
    const entries: Array<{ label?: string; properties?: string[]; category_id?: string }> = [];
    for (const a of annotations as ReadonlyArray<Annotation>) {
      if (!a || typeof a !== "object") continue;
      const entry: { label?: string; properties?: string[]; category_id?: string } = {};
      const label =
        typeof a.label === "string" && a.label
          ? a.label
          : typeof a.labelMarkdown === "string" && a.labelMarkdown
            ? a.labelMarkdown
            : undefined;
      if (label) entry.label = label;
      const props = Array.isArray(a.properties)
        ? a.properties.map((p) => (p && typeof p.type === "string" ? p.type : "")).filter((t) => t)
        : [];
      if (props.length) entry.properties = props;
      if (typeof a.categoryId === "string" && a.categoryId) entry.category_id = a.categoryId;
      if (Object.keys(entry).length > 0) entries.push(entry);
    }
    if (entries.length) attrs["figma:annotations"] = JSON.stringify(entries);
  }

  const settings =
    "exportSettings" in n ? (n as SceneNode & { exportSettings?: unknown }).exportSettings : undefined;
  if (Array.isArray(settings) && settings.length > 0) {
    const entries: Array<{ format: string; suffix?: string; constraint?: string; contents_only?: boolean }> = [];
    for (const s of settings as ReadonlyArray<ExportSettings>) {
      if (!s || typeof s.format !== "string") continue;
      const entry: { format: string; suffix?: string; constraint?: string; contents_only?: boolean } = {
        format: s.format.toLowerCase(),
      };
      if (typeof s.suffix === "string" && s.suffix) entry.suffix = s.suffix;
      const c = "constraint" in s ? (s as ExportSettingsImage).constraint : undefined;
      if (c && typeof c.type === "string" && typeof c.value === "number") {
        const kind = c.type.toLowerCase();
        if (kind !== "scale" || Math.abs(c.value - 1) > 1e-6) {
          entry.constraint = `${kind}:${fmtGeomNum(c.value)}`;
        }
      }
      if ((s as { contentsOnly?: unknown }).contentsOnly === false) entry.contents_only = false;
      entries.push(entry);
    }
    if (entries.length) attrs["figma:export_settings"] = JSON.stringify(entries);
  }

  return Object.keys(attrs).length > 0 ? attrs : undefined;
}

// ──────────────────────────────────────────────────────────────────────────
// Effects → ordered box-shadow / filter / backdrop-filter + diagnostics.

/// The node's effect stack lowered to CSS, plus the diagnostics the caller
/// pushes (extract-pure stays host-neutral, so it returns them instead of
/// reaching for a WalkCtx — same contract as ExtractedStroke).
export interface LoweredEffects {
  box_shadow?: string;
  filter?: string;
  backdrop_filter?: string;
  diagnostics: Array<Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message">>;
}

// Figma's `effects` is an ordered stack; each family lowers to the CSS
// property the render stack actually consumes:
//
//   - DROP_SHADOW / INNER_SHADOW → comma-joined `box_shadow` layers in array
//     order (parse_css_box_shadow keeps the order; codegen emits every layer).
//   - LAYER_BLUR → `filter: blur(Npx)` — the bridge's setFilter builds a
//     View::FilterOp chain composed via SkImageFilters at paint time.
//   - BACKGROUND_BLUR → `backdrop_filter: blur(Npx)` — routed to
//     View::set_backdrop_blur (frosted-glass compositing layer).
//   - A PROGRESSIVE blur keeps its end radius as a uniform blur, with a
//     capture_partial diagnostic naming the approximation.
//   - Anything else (NOISE, TEXTURE, GLASS, families newer than the pinned
//     typings) has no lowering: an unsupported_property diagnostic, never a
//     silent drop.
//
// Invisible effects (visible === false) are the designer's own off switch and
// are skipped without comment. Multiple blurs of one kind keep array order as
// a space-joined function sequence (setFilter sums blur amounts).
export function lowerEffects(effects: readonly Effect[]): LoweredEffects {
  const out: LoweredEffects = { diagnostics: [] };
  const shadows: string[] = [];
  const filters: string[] = [];
  const backdrops: string[] = [];
  for (const eff of effects) {
    if (eff.visible === false) continue;
    if (eff.type === "DROP_SHADOW" || eff.type === "INNER_SHADOW") {
      const ds = eff as DropShadowEffect | InnerShadowEffect;
      const inner = eff.type === "INNER_SHADOW" ? "inset " : "";
      shadows.push(
        `${inner}${ds.offset.x}px ${ds.offset.y}px ${ds.radius}px ${ds.spread ?? 0}px ${rgbaToCss(ds.color)}`,
      );
    } else if (eff.type === "LAYER_BLUR" || eff.type === "BACKGROUND_BLUR") {
      const blur = eff as BlurEffect;
      (eff.type === "LAYER_BLUR" ? filters : backdrops).push(`blur(${blur.radius}px)`);
      if (blur.blurType === "PROGRESSIVE") {
        out.diagnostics.push({
          severity: "warning",
          code: "progressive-blur-approximated",
          kind: "capture_partial",
          message: `${eff.type} is PROGRESSIVE; approximated as a uniform blur(${blur.radius}px) (its end radius).`,
        });
      }
    } else {
      out.diagnostics.push({
        severity: "warning",
        code: "effect-unsupported",
        kind: "unsupported_property",
        message: `${(eff as Effect).type} effect has no lowering in the render stack; the node composites without it.`,
      });
    }
  }
  if (shadows.length > 0) out.box_shadow = shadows.join(", ");
  if (filters.length > 0) out.filter = filters.join(" ");
  if (backdrops.length > 0) out.backdrop_filter = backdrops.join(" ");
  return out;
}

// Rotation / transform (audit "Rotation / transform" row).

/// A node's 2x3 relativeTransform decoded into the lowering the shared
/// consumer already understands. Three outcomes:
///
///   - "identity": nothing to emit — the matrix is translation-only, or its
///     rotation is a multiple of 90deg. An orthogonal spin keeps the box
///     axis-aligned, and the bounding-box placement the caller already
///     emitted is exact for it; re-applying the angle (plus a center-pivot
///     compensation) only shifts the box off its intended row — the #6277
///     slider-fill regression in the .fig lane. Tolerance mirrors
///     fig/scene.mjs::styleFor field-for-field: 0.5deg on either side of
///     each 90deg multiple.
///   - "rotate": a pure rotation (uniform unit scale, orthogonal columns,
///     no mirroring) at a meaningfully non-orthogonal angle. The caller
///     emits `rotate(<deg>deg)` — the exact spelling the shared codegen
///     lowers to setRotation — plus center-preserving placement.
///   - "unrepresentable": the matrix carries skew, non-unit / non-uniform
///     scale, or a mirror combined with rotation. A single center
///     `rotate()` would be WRONG for these, and a wrong rotation reads as
///     a layout bug, not a dropped property — so the caller must diagnose
///     (`transform-skew-approximated`) and keep the node axis-aligned at
///     its bounding box (today's behavior), preserving the full matrix as
///     the `figma:transform_matrix` provenance attribute instead.
///
/// A pure orthogonal mirror (flip-H/flip-V with no rotation) stays
/// "identity": the axis-aligned box already occupies the right pixels, which
/// matches what every lane ships today, and diagnosing each flipped icon
/// would bury real findings.
export type DecodedTransform =
  | { kind: "identity" }
  | { kind: "rotate"; deg: number; matrixAttr: string }
  | {
      kind: "unrepresentable";
      matrixAttr: string;
      diagnostic: Pick<ExtractedDiagnostic, "severity" | "code" | "kind" | "message">;
    };

/// The full 2x3 affine as a stable attr string: "m00,m01,m02,m10,m11,m12"
/// (row-major, the wire order), numbers trimmed like the geometry attrs.
export function transformMatrixAttr(t: number[][]): string {
  return [t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2]].map(fmtGeomNum).join(",");
}

export function decodeRelativeTransform(
  t: number[][] | null | undefined,
  nodeName: string,
): DecodedTransform {
  if (
    !Array.isArray(t) || t.length !== 2 ||
    !Array.isArray(t[0]) || t[0].length !== 3 ||
    !Array.isArray(t[1]) || t[1].length !== 3 ||
    ![...t[0], ...t[1]].every((v) => typeof v === "number" && Number.isFinite(v))
  ) {
    return { kind: "identity" };
  }
  const [[m00, m01], [m10, m11]] = t;
  // Column lengths = the axis scale factors. A degenerate column means the
  // node renders zero-sized on that axis; there is nothing to rotate.
  const sx = Math.hypot(m00, m10);
  const sy = Math.hypot(m01, m11);
  if (sx < 1e-6 || sy < 1e-6) return { kind: "identity" };

  const deg = (Math.atan2(m10, m00) * 180) / Math.PI;
  const mod90 = Math.abs(deg) % 90;
  const nonOrthogonal = mod90 > 0.5 && mod90 < 89.5;

  // A center rotate() is exact only for a similarity transform with unit
  // scale and no mirror: orthogonal columns (normalized dot ~ 0), both
  // scales ~ 1, positive determinant. Figma's matrices are unit-scale for
  // ordinary layers (resize lands in width/height, not the matrix), so a
  // deviation here means real skew / scale / mirror the design carries.
  const skewed = Math.abs((m00 * m01 + m10 * m11) / (sx * sy)) > 1e-3;
  const scaled = Math.abs(sx - 1) > 1e-3 || Math.abs(sy - 1) > 1e-3;
  const mirrored = m00 * m11 - m01 * m10 < 0;
  if (skewed || scaled || (mirrored && nonOrthogonal)) {
    const parts = [
      skewed ? "skew" : "",
      scaled ? "non-unit scale" : "",
      mirrored ? "mirroring" : "",
    ].filter(Boolean).join(" + ");
    return {
      kind: "unrepresentable",
      matrixAttr: transformMatrixAttr(t),
      diagnostic: {
        severity: "warning",
        code: "transform-skew-approximated",
        kind: "unsupported_property",
        message:
          `"${nodeName}": relativeTransform carries ${parts}, which a single center ` +
          `rotate() cannot represent. Rendered axis-aligned at its bounding box; the ` +
          `full matrix is preserved as figma:transform_matrix.`,
      },
    };
  }
  if (!nonOrthogonal) return { kind: "identity" };
  return { kind: "rotate", deg, matrixAttr: transformMatrixAttr(t) };
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
// Sibling-mask lowering (audit item 9).
//
// A Figma child with `isMask: true` paints NOWHERE — its outline CLIPS the
// siblings painted after it (above it in paint order) in the same parent,
// until the next mask or the parent's end. The walker (extract.ts) moves
// those siblings into a synthetic wrapper that spans the parent and carries
// the mask's outline as a CSS clip-path — the consumer contract the engine
// already has end-to-end (IRStyle::clip_path → setClipPath →
// SkPath::FromSVGString), and the exact wrapper shape the .fig decoder emits
// (fig/scene.mjs::walkChildren). The helpers here are the pure parts:
// parent-space transform resolution, outline synthesis, and fidelity
// assessment. Kept in lockstep with the REST port
// (tools/import-design/figma_rest_export.py).

/// 2×3 affine row-major matrix, the Plugin API `Transform` shape:
/// [[m00, m01, m02], [m10, m11, m12]] mapping (x, y) → (m00x + m01y + m02,
/// m10x + m11y + m12).
export type AffineTransform = readonly [
  readonly [number, number, number],
  readonly [number, number, number],
];

const IDENTITY_TRANSFORM: AffineTransform = [[1, 0, 0], [0, 1, 0]];

function isAffine(t: unknown): t is AffineTransform {
  return (
    Array.isArray(t) && t.length === 2 &&
    t.every((row) => Array.isArray(row) && row.length === 3 &&
      row.every((v: unknown) => typeof v === "number" && isFinite(v)))
  );
}

/// a ∘ b — apply b first, then a.
export function composeAffine(a: AffineTransform, b: AffineTransform): AffineTransform {
  return [
    [
      a[0][0] * b[0][0] + a[0][1] * b[1][0],
      a[0][0] * b[0][1] + a[0][1] * b[1][1],
      a[0][0] * b[0][2] + a[0][1] * b[1][2] + a[0][2],
    ],
    [
      a[1][0] * b[0][0] + a[1][1] * b[1][0],
      a[1][0] * b[0][1] + a[1][1] * b[1][1],
      a[1][0] * b[0][2] + a[1][1] * b[1][2] + a[1][2],
    ],
  ];
}

/// Inverse of an affine transform, or null when degenerate (zero-area scale).
export function invertAffine(m: AffineTransform): AffineTransform | null {
  const det = m[0][0] * m[1][1] - m[0][1] * m[1][0];
  if (!isFinite(det) || Math.abs(det) < 1e-12) return null;
  const a = m[1][1] / det;
  const b = -m[0][1] / det;
  const c = -m[1][0] / det;
  const d = m[0][0] / det;
  return [
    [a, b, -(a * m[0][2] + b * m[1][2])],
    [c, d, -(c * m[0][2] + d * m[1][2])],
  ];
}

/// The transform that places a mask node's LOCAL geometry in its parent's
/// border-box space — the space a CSS clip-path on a wrapper spanning the
/// parent is consumed in. Preference order:
///   1. inv(parent.absoluteTransform) ∘ node.absoluteTransform — exact and
///      group-proof (relativeTransform is relative to the nearest non-group
///      container, NOT the direct parent, so it is wrong inside a GROUP).
///   2. node.relativeTransform — right whenever the parent is that container.
///   3. Pure translation from absoluteBoundingBox deltas — loses rotation but
///      still lands the clip at the right offset.
export function maskParentSpaceTransform(
  node: SceneNode,
  parent: SceneNode | null,
): AffineTransform {
  const abs = (n: SceneNode | null): AffineTransform | null => {
    if (!n) return null;
    const t = (n as SceneNode & { absoluteTransform?: unknown }).absoluteTransform;
    return isAffine(t) ? t : null;
  };
  const nodeAbs = abs(node);
  const parentAbs = abs(parent);
  if (nodeAbs && parentAbs) {
    const inv = invertAffine(parentAbs);
    if (inv) return composeAffine(inv, nodeAbs);
  }
  const rel = (node as SceneNode & { relativeTransform?: unknown }).relativeTransform;
  if (isAffine(rel)) return rel;
  const nb = "absoluteBoundingBox" in node ? node.absoluteBoundingBox : null;
  const pb = parent && "absoluteBoundingBox" in parent
    ? (parent as SceneNode & { absoluteBoundingBox: Rect | null }).absoluteBoundingBox
    : null;
  if (nb && pb) return [[1, 0, nb.x - pb.x], [0, 1, nb.y - pb.y]];
  return IDENTITY_TRANSFORM;
}

function roundCoord(v: number): number {
  return Math.round(v * 100) / 100;
}

/// Apply an affine transform to SVG path data. Handles the command set Figma
/// geometry uses (M, L, C, Q, Z) plus H/V/S/T and the relative forms
/// defensively; H/V become L because a horizontal segment stops being
/// horizontal under rotation. Arcs (A/a) don't survive a general affine
/// without re-deriving axes, so they return null and the caller falls back to
/// the box outline — an approximate clip region beats dropping the clip.
export function transformSvgPathData(d: string, m: AffineTransform): string | null {
  const tokens = d.match(/[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/g);
  if (!tokens || tokens.length === 0) return null;
  const out: string[] = [];
  // Current absolute point, tracked so H/V can be rewritten as L.
  let cx = 0, cy = 0;       // current point
  let sx = 0, sy = 0;       // subpath start (for Z)
  let i = 0;
  const num = (): number | null => {
    if (i >= tokens.length || /^[A-Za-z]$/.test(tokens[i])) return null;
    return parseFloat(tokens[i++]);
  };
  const mapAbs = (x: number, y: number): string =>
    `${roundCoord(m[0][0] * x + m[0][1] * y + m[0][2])} ${roundCoord(m[1][0] * x + m[1][1] * y + m[1][2])}`;
  while (i < tokens.length) {
    const cmd = tokens[i++];
    if (!/^[A-Za-z]$/.test(cmd)) return null; // stray number: malformed data
    const rel = cmd >= "a" && cmd <= "z";
    const op = cmd.toUpperCase();
    if (op === "A") return null;
    if (op === "Z") { out.push("Z"); cx = sx; cy = sy; continue; }
    // Consume coordinate groups until the next command letter; every SVG
    // command may carry repeated argument groups.
    let groups = 0;
    for (;;) {
      const peek = i < tokens.length ? tokens[i] : null;
      if (peek === null || /^[A-Za-z]$/.test(peek)) {
        if (groups === 0) return null; // command with no arguments: malformed
        break;
      }
      groups++;
      // Resolve this group to absolute endpoint(s), then transform.
      if (op === "H" || op === "V") {
        const v = num();
        if (v === null) return null;
        const x = op === "H" ? (rel ? cx + v : v) : cx;
        const y = op === "V" ? (rel ? cy + v : v) : cy;
        out.push(`L${mapAbs(x, y)}`);
        cx = x; cy = y;
        continue;
      }
      const pairs = op === "C" ? 3 : op === "S" || op === "Q" ? 2 : 1;
      const pts: Array<[number, number]> = [];
      for (let p = 0; p < pairs; p++) {
        const x = num(); const y = num();
        if (x === null || y === null) return null;
        pts.push(rel ? [cx + x, cy + y] : [x, y]);
      }
      // Per the SVG grammar an M's extra coordinate pairs are implicit L
      // commands, so only the FIRST group after an M keeps the M.
      const emitOp = op === "M" && groups > 1 ? "L" : op;
      const emitted = pts.map(([x, y]) => mapAbs(x, y)).join(" ");
      out.push(`${emitOp}${emitted}`);
      const [ex, ey] = pts[pts.length - 1];
      cx = ex; cy = ey;
      if (emitOp === "M") { sx = ex; sy = ey; }
    }
  }
  return out.join(" ");
}

/// Per-corner radii of a box-model node, clamped later by the outline
/// builder. Returns null when the node carries no numeric radii (a plain
/// sharp rectangle / frame).
function boxCornerRadii(node: SceneNode): { tl: number; tr: number; br: number; bl: number } {
  const n = node as SceneNode & {
    cornerRadius?: number | symbol;
    topLeftRadius?: number; topRightRadius?: number;
    bottomRightRadius?: number; bottomLeftRadius?: number;
  };
  const uniform = typeof n.cornerRadius === "number" ? n.cornerRadius : 0;
  return {
    tl: typeof n.topLeftRadius === "number" ? n.topLeftRadius : uniform,
    tr: typeof n.topRightRadius === "number" ? n.topRightRadius : uniform,
    br: typeof n.bottomRightRadius === "number" ? n.bottomRightRadius : uniform,
    bl: typeof n.bottomLeftRadius === "number" ? n.bottomLeftRadius : uniform,
  };
}

/// A box-model node's outline as SVG path data through the given transform —
/// for a rectangle / rounded rectangle / ellipse / frame used as a mask,
/// whose geometry derives from the box. Every point (cubic control points
/// included — an affine maps a bezier's control polygon exactly) goes through
/// the transform, so a rotated mask clips where the design rotated it.
/// Mirrors fig/scene.mjs::boxMaskOutline.
export function boxOutlinePath(
  width: number,
  height: number,
  m: AffineTransform,
  opts: { isEllipse?: boolean; radii?: { tl: number; tr: number; br: number; bl: number } } = {},
): string | null {
  const w = width, h = height;
  if (!(w > 0) || !(h > 0)) return null;
  const pt = (x: number, y: number): string =>
    `${roundCoord(m[0][0] * x + m[0][1] * y + m[0][2])} ${roundCoord(m[1][0] * x + m[1][1] * y + m[1][2])}`;
  // Circular-arc-from-cubic constant; the same approximation every renderer's
  // rounded rect uses, and exact enough that no display resolves the error.
  const k = 0.5522847498;
  if (opts.isEllipse) {
    const rx = w / 2, ry = h / 2;
    return `M${pt(rx, 0)} `
      + `C${pt(rx + k * rx, 0)} ${pt(w, ry - k * ry)} ${pt(w, ry)} `
      + `C${pt(w, ry + k * ry)} ${pt(rx + k * rx, h)} ${pt(rx, h)} `
      + `C${pt(rx - k * rx, h)} ${pt(0, ry + k * ry)} ${pt(0, ry)} `
      + `C${pt(0, ry - k * ry)} ${pt(rx - k * rx, 0)} ${pt(rx, 0)} Z`;
  }
  const cap = Math.min(w, h) / 2;
  const r = (v: number): number => Math.min(Math.max(v || 0, 0), cap);
  const tl = r(opts.radii?.tl ?? 0);
  const tr = r(opts.radii?.tr ?? 0);
  const br = r(opts.radii?.br ?? 0);
  const bl = r(opts.radii?.bl ?? 0);
  return `M${pt(tl, 0)} L${pt(w - tr, 0)} `
    + (tr ? `C${pt(w - tr + k * tr, 0)} ${pt(w, tr - k * tr)} ${pt(w, tr)} ` : "")
    + `L${pt(w, h - br)} `
    + (br ? `C${pt(w, h - br + k * br)} ${pt(w - br + k * br, h)} ${pt(w - br, h)} ` : "")
    + `L${pt(bl, h)} `
    + (bl ? `C${pt(bl - k * bl, h)} ${pt(0, h - bl + k * bl)} ${pt(0, h - bl)} ` : "")
    + `L${pt(0, tl)} `
    + (tl ? `C${pt(0, tl - k * tl)} ${pt(tl - k * tl, 0)} ${pt(tl, 0)} ` : "")
    + "Z";
}

/// The mask's clip outline in its parent's border-box space: the node's own
/// vector geometry (fillGeometry, else strokeGeometry) transformed into
/// parent space when it carries one, else the box-model outline synthesized
/// from its size and corner radii (a rectangle or ellipse used as a mask).
/// What clips is the mask's SHAPE — fill visibility on a mask changes its
/// alpha semantics, not its outline — hence fillGeometry first regardless of
/// paint visibility. Mirrors fig/scene.mjs::maskClipOutline.
export function maskClipOutline(node: SceneNode, parent: SceneNode | null): string | null {
  const m = maskParentSpaceTransform(node, parent);
  const geo = node as SceneNode & {
    fillGeometry?: readonly { data: string }[];
    strokeGeometry?: readonly { data: string }[];
  };
  const paths = (geo.fillGeometry && geo.fillGeometry.length ? geo.fillGeometry : undefined)
    ?? (geo.strokeGeometry && geo.strokeGeometry.length ? geo.strokeGeometry : undefined);
  if (paths) {
    const parts: string[] = [];
    let ok = true;
    for (const p of paths) {
      if (typeof p.data !== "string" || !p.data) { ok = false; break; }
      const t = transformSvgPathData(p.data, m);
      if (t === null) { ok = false; break; }
      parts.push(t);
    }
    // An untransformable path falls through to the box outline: an
    // approximate clip region beats painting the mask or dropping the clip.
    if (ok && parts.length) return parts.join(" ");
  }
  const w = "width" in node ? node.width : 0;
  const h = "height" in node ? node.height : 0;
  return boxOutlinePath(w, h, m, {
    isEllipse: node.type === "ELLIPSE",
    radii: boxCornerRadii(node),
  });
}

/// How faithful an outline clip-path is to this mask's real semantics.
///   geometric — outline (VECTOR) masks, and ALPHA masks whose content is one
///               fully opaque solid: the hard outline IS the mask.
///   luminance — pixel brightness modulates alpha; an outline can't carry it.
///   soft_alpha — ALPHA mask with soft or partial coverage (image / gradient
///               fill, partial paint or node opacity, or no visible fill):
///               flattening to the outline clips harder than the design.
export type MaskFidelity = "geometric" | "luminance" | "soft_alpha";

export function assessMaskFidelity(node: SceneNode): MaskFidelity {
  const n = node as SceneNode & { maskType?: string; opacity?: number; fills?: unknown };
  if (n.maskType === "LUMINANCE") return "luminance";
  if (n.maskType === "VECTOR") return "geometric";
  // ALPHA (or a typings vintage without maskType): exact only when the mask's
  // content is one fully opaque solid at full node opacity.
  const fills = Array.isArray(n.fills) ? (n.fills as Paint[]) : [];
  const paint = fills.find((p) => p && p.visible !== false);
  const soft =
    !paint || paint.type !== "SOLID" ||
    (paint.opacity ?? 1) < 1 ||
    (typeof n.opacity === "number" && n.opacity < 1);
  return soft ? "soft_alpha" : "geometric";
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
