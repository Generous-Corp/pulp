// Phase 2a — Figma scene walker.
//
// Walks a Figma selection and produces an `ExtractedFigmaNode` tree matching the
// shape declared in schema/figma-plugin-export-v1.json. This is the in-memory
// model that downstream phases consume:
//   - Phase 2b serializes it to JSON
//   - Phase 3 mutates it to swap library-component frames into widget nodes
//
// SLICE 1 of Phase 2a (this file): geometry, frames + auto-layout, text +
// dominant typography, basic style (fills as flat color or linear gradient,
// strokes, corner radius, opacity, blend mode), and recursive children.
// Images / vectors / instance-component-key lookup defer to slice 2.

import type {
  ExtractedFigmaNode,
  ExtractedStyle,
  ExtractedLayout,
  ExtractedDiagnostic,
} from "./extract-model";

// ──────────────────────────────────────────────────────────────────────────
// Public entry point

export interface ExtractOptions {
  /// Whether to include children of hidden layers in the export.
  includeHidden?: boolean;
  /// Max nodes before the walker bails out with a diagnostic (perf safety).
  maxNodes?: number;
}

export interface ExtractResult {
  roots: ExtractedFigmaNode[];
  /// Diagnostics raised during traversal (unsupported features, capture gaps).
  diagnostics: ExtractedDiagnostic[];
  /// Total node count walked (after filters).
  nodeCount: number;
  /// True if maxNodes was hit and the result is incomplete.
  truncated: boolean;
}

/// Walk the given Figma scene nodes into a Pulp-shaped tree.
/// `nodes` is typically `figma.currentPage.selection`.
export async function extractScene(
  nodes: readonly SceneNode[],
  opts: ExtractOptions = {},
): Promise<ExtractResult> {
  const cfg = {
    includeHidden: opts.includeHidden ?? false,
    maxNodes: opts.maxNodes ?? 5000,
  };
  const diagnostics: ExtractedDiagnostic[] = [];
  const ctx: WalkCtx = {
    cfg,
    diagnostics,
    nodeCount: 0,
    truncated: false,
    pathStack: [],
  };

  const roots: ExtractedFigmaNode[] = [];
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (ctx.truncated) break;
    ctx.pathStack.push(`/root[${i}]`);
    const extracted = await walk(n, null, i, ctx);
    ctx.pathStack.pop();
    if (extracted) roots.push(extracted);
  }

  return {
    roots,
    diagnostics: ctx.diagnostics,
    nodeCount: ctx.nodeCount,
    truncated: ctx.truncated,
  };
}

// ──────────────────────────────────────────────────────────────────────────
// Internal walker

interface WalkCtx {
  cfg: Required<ExtractOptions>;
  diagnostics: ExtractedDiagnostic[];
  nodeCount: number;
  truncated: boolean;
  pathStack: string[];
}

async function walk(
  node: SceneNode,
  parentId: string | null,
  zOrder: number,
  ctx: WalkCtx,
): Promise<ExtractedFigmaNode | null> {
  if (ctx.truncated) return null;
  if (!ctx.cfg.includeHidden && "visible" in node && node.visible === false) {
    return null;
  }
  if (ctx.nodeCount >= ctx.cfg.maxNodes) {
    ctx.truncated = true;
    ctx.diagnostics.push({
      severity: "warning",
      code: "max-nodes-exceeded",
      kind: "capture_partial",
      path: pathOf(ctx),
      message: `Hit ${ctx.cfg.maxNodes} node limit; remaining children skipped.`,
    });
    return null;
  }
  ctx.nodeCount++;

  const type = mapNodeType(node);
  const ex: ExtractedFigmaNode = {
    type,
    figma_type: node.type,
    name: node.name,
    figma_node_id: node.id,
    parent_id: parentId,
    z_order: zOrder,
    absolute_bounds: readAbsoluteBounds(node),
    relative_transform: readRelativeTransform(node),
    visible: "visible" in node ? !!node.visible : true,
    locked: "locked" in node ? !!node.locked : false,
    opacity: "opacity" in node ? (node as BlendMixin).opacity : 1,
    blend_mode: "blendMode" in node ? ((node as BlendMixin).blendMode ?? "PASS_THROUGH") : "PASS_THROUGH",
    style: extractStyle(node, ctx),
    layout: extractLayout(node, ctx),
    children: [],
  };

  // Text content + dominant style
  if (node.type === "TEXT") {
    ex.content = (node as TextNode).characters;
    extractTextStyle(node as TextNode, ex.style, ctx);
  }

  // Recurse for container nodes that have children.
  if ("children" in node) {
    const children = (node as ChildrenMixin).children;
    for (let i = 0; i < children.length; i++) {
      ctx.pathStack.push(`/children[${i}]`);
      const child = await walk(children[i], node.id, i, ctx);
      ctx.pathStack.pop();
      if (child) ex.children.push(child);
    }
  }

  return ex;
}

// ──────────────────────────────────────────────────────────────────────────
// Type mapping

function mapNodeType(n: SceneNode): string {
  switch (n.type) {
    case "FRAME":
    case "GROUP":
    case "SECTION":
      return "frame";
    case "COMPONENT":
    case "COMPONENT_SET":
      return "frame"; // Phase 3 will promote recognized instances to widget kinds
    case "INSTANCE":
      return "frame"; // ditto
    case "TEXT":
      return "text";
    case "RECTANGLE":
    case "ELLIPSE":
    case "POLYGON":
    case "STAR":
    case "LINE":
      return "frame";
    case "VECTOR":
    case "BOOLEAN_OPERATION":
      return "vector";
    case "SLICE":
      return "frame";
    default:
      return "frame";
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Geometry

function readAbsoluteBounds(n: SceneNode): ExtractedFigmaNode["absolute_bounds"] {
  if ("absoluteBoundingBox" in n && n.absoluteBoundingBox) {
    const b = n.absoluteBoundingBox;
    return { x: b.x, y: b.y, w: b.width, h: b.height };
  }
  if ("width" in n && "height" in n && "x" in n && "y" in n) {
    return { x: n.x, y: n.y, w: n.width, h: n.height };
  }
  return { x: 0, y: 0, w: 0, h: 0 };
}

function readRelativeTransform(n: SceneNode): number[][] {
  if ("relativeTransform" in n && n.relativeTransform) {
    const t = n.relativeTransform;
    return [
      [t[0][0], t[0][1], t[0][2]],
      [t[1][0], t[1][1], t[1][2]],
    ];
  }
  return [
    [1, 0, 0],
    [0, 1, 0],
  ];
}

// ──────────────────────────────────────────────────────────────────────────
// Style extraction

function extractStyle(n: SceneNode, ctx: WalkCtx): ExtractedStyle {
  const s: ExtractedStyle = {};

  if ("absoluteBoundingBox" in n && n.absoluteBoundingBox) {
    s.width = n.absoluteBoundingBox.width;
    s.height = n.absoluteBoundingBox.height;
  }

  // Fills
  if ("fills" in n && Array.isArray(n.fills) && n.fills.length > 0) {
    const fills = n.fills as readonly Paint[];
    const visible = fills.filter((p) => p.visible !== false);
    const first = visible[0];
    if (first) {
      if (first.type === "SOLID") {
        s.background_color = paintToColor(first as SolidPaint);
      } else if (first.type === "GRADIENT_LINEAR") {
        s.background_gradient = gradientToCss(first as GradientPaint);
      } else if (first.type === "IMAGE") {
        // image fill — Phase 2a slice 2 handles asset export; record marker now
        s.background_image = "image-fill"; // placeholder; slice 2 fills in asset_ref
        pushDiag(ctx, "info", "image-fill-deferred", "capture_partial",
          "Image fill present; asset export not implemented yet (slice 2).");
      } else if (first.type === "GRADIENT_RADIAL" || first.type === "GRADIENT_ANGULAR" || first.type === "GRADIENT_DIAMOND") {
        pushDiag(ctx, "warning", "complex-gradient", "unsupported_property",
          `${first.type} not supported; emitting flat first color fallback.`);
        s.background_gradient = gradientFallbackFlat(first as GradientPaint);
      }
    }
  }

  // Strokes (border)
  if ("strokes" in n && Array.isArray(n.strokes) && n.strokes.length > 0) {
    const strokes = n.strokes as readonly Paint[];
    const first = strokes.find((p) => p.visible !== false);
    if (first && first.type === "SOLID") {
      const color = paintToColor(first as SolidPaint);
      const weight = "strokeWeight" in n && typeof n.strokeWeight === "number" ? n.strokeWeight : 1;
      s.border = `${weight}px solid ${color}`;
      s.border_color = color;
      s.border_width = weight;
      s.border_style = "solid";
    }
  }

  // Corner radius
  if ("cornerRadius" in n && typeof n.cornerRadius === "number") {
    s.border_radius = n.cornerRadius;
  }

  // Opacity
  if ("opacity" in n && (n as BlendMixin).opacity !== undefined && (n as BlendMixin).opacity < 1) {
    s.opacity = (n as BlendMixin).opacity;
  }

  // Effects — drop/inner shadow → box_shadow; blur → filter
  if ("effects" in n && Array.isArray(n.effects)) {
    const effects = n.effects as readonly Effect[];
    const shadows: string[] = [];
    let filter: string | undefined;
    for (const eff of effects) {
      if (eff.visible === false) continue;
      if (eff.type === "DROP_SHADOW" || eff.type === "INNER_SHADOW") {
        const ds = eff as DropShadowEffect | InnerShadowEffect;
        const inner = eff.type === "INNER_SHADOW" ? "inset " : "";
        shadows.push(
          `${inner}${ds.offset.x}px ${ds.offset.y}px ${ds.radius}px ${ds.spread ?? 0}px ${rgbaToCss(ds.color)}`,
        );
      } else if (eff.type === "LAYER_BLUR") {
        filter = `blur(${(eff as BlurEffect).radius}px)`;
      } else if (eff.type === "BACKGROUND_BLUR") {
        s.backdrop_filter = `blur(${(eff as BlurEffect).radius}px)`;
      }
    }
    if (shadows.length > 0) s.box_shadow = shadows.join(", ");
    if (filter) s.filter = filter;
  }

  // Overflow — Figma's clipsContent maps loosely to overflow: clip
  if ("clipsContent" in n && (n as FrameNode).clipsContent === true) {
    s.overflow = "clip";
  }

  return s;
}

function extractTextStyle(t: TextNode, s: ExtractedStyle, ctx: WalkCtx): void {
  // Read the "first character" style as the dominant style. Per-range
  // emission is a follow-up.
  const charLen = t.characters.length;
  if (charLen === 0) return;
  if (typeof t.fontSize === "number") s.font_size = t.fontSize;
  if (typeof t.fontName === "object" && t.fontName) {
    s.font_family = t.fontName.family;
    s.font_style = /italic/i.test(t.fontName.style) ? "italic" : "normal";
  }
  if (typeof t.fontWeight === "number") s.font_weight = t.fontWeight;
  if (typeof t.letterSpacing === "object" && t.letterSpacing?.unit === "PIXELS") {
    s.letter_spacing = (t.letterSpacing as { value: number }).value;
  }
  if (typeof t.lineHeight === "object" && t.lineHeight) {
    if (t.lineHeight.unit === "PIXELS") s.line_height = (t.lineHeight as { value: number }).value;
  }
  if (t.textAlignHorizontal) {
    s.text_align = (t.textAlignHorizontal as string).toLowerCase();
  }
  if (t.textCase === "UPPER") s.text_transform = "uppercase";
  else if (t.textCase === "LOWER") s.text_transform = "lowercase";
  else if (t.textCase === "TITLE") s.text_transform = "capitalize";
  // text color = first solid fill
  if (Array.isArray(t.fills) && t.fills.length > 0) {
    const first = (t.fills as readonly Paint[]).find((p) => p.type === "SOLID" && p.visible !== false);
    if (first) {
      s.color = paintToColor(first as SolidPaint);
      // Clear the fill we set as background_color earlier — text uses color, not bg.
      delete s.background_color;
    }
  }
  // Per-range note for follow-up
  if (charLen > 0) {
    const hasMultiRangeFonts = false; // TODO slice 2: scan getRangeFontName
    if (hasMultiRangeFonts) {
      pushDiag(ctx, "info", "text-ranges-flattened", "capture_partial",
        "Mixed font ranges in text node flattened to dominant style.");
    }
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Layout extraction (auto-layout)

function extractLayout(n: SceneNode, ctx: WalkCtx): ExtractedLayout {
  const l: ExtractedLayout = {};
  if (n.type !== "FRAME" && n.type !== "COMPONENT" && n.type !== "INSTANCE" && n.type !== "COMPONENT_SET") {
    return l;
  }
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
    l.width_mode = mapAxisSize(f.layoutSizingHorizontal);
    l.height_mode = mapAxisSize(f.layoutSizingVertical);
  } else if (f.layoutMode === "NONE" || f.layoutMode === undefined) {
    // Children positioned absolutely — emit no display/direction
    l.width_mode = "fixed";
    l.height_mode = "fixed";
  }
  return l;
}

function mapPrimaryAxisAlign(v: FrameNode["primaryAxisAlignItems"]): ExtractedLayout["justify"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "SPACE_BETWEEN": return "space_between";
    default: return "flex_start";
  }
}

function mapCounterAxisAlign(v: FrameNode["counterAxisAlignItems"]): ExtractedLayout["align"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "BASELINE": return "flex_start"; // Pulp Yoga doesn't model baseline; closest fallback
    default: return "stretch";
  }
}

function mapAxisSize(v: FrameNode["layoutSizingHorizontal"]): ExtractedLayout["width_mode"] {
  switch (v) {
    case "HUG": return "hug";
    case "FILL": return "fill";
    case "FIXED":
    default: return "fixed";
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Color helpers

function paintToColor(p: SolidPaint): string {
  const c = p.color;
  const a = p.opacity !== undefined ? p.opacity : 1;
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  if (a >= 1) return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  return `rgba(${r}, ${g}, ${b}, ${a.toFixed(3)})`;
}

function rgbaToCss(c: RGBA): string {
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  return `rgba(${r}, ${g}, ${b}, ${c.a.toFixed(3)})`;
}

function hex2(n: number): string {
  return n.toString(16).padStart(2, "0");
}

function gradientToCss(p: GradientPaint): string {
  if (!p.gradientStops || p.gradientStops.length === 0) return "linear-gradient(transparent, transparent)";
  const stops = p.gradientStops
    .map((s) => `${rgbaToCss(s.color)} ${(s.position * 100).toFixed(1)}%`)
    .join(", ");
  // Figma gradient transforms are 2x3 matrices that we'd need to decompose into an
  // angle. For slice 1 emit a simple top→bottom; slice 2 handles arbitrary angles.
  return `linear-gradient(to bottom, ${stops})`;
}

function gradientFallbackFlat(p: GradientPaint): string {
  const first = p.gradientStops?.[0]?.color;
  if (!first) return "transparent";
  return rgbaToCss(first);
}

// ──────────────────────────────────────────────────────────────────────────
// Diagnostic helpers

function pushDiag(
  ctx: WalkCtx,
  severity: ExtractedDiagnostic["severity"],
  code: string,
  kind: ExtractedDiagnostic["kind"],
  message: string,
): void {
  ctx.diagnostics.push({ severity, code, kind, message, path: pathOf(ctx) });
}

function pathOf(ctx: WalkCtx): string {
  return ctx.pathStack.join("");
}
