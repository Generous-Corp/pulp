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
import { AssetCache } from "./assets";
import { extractTokens, type ExtractedTokens } from "./tokens";

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
  /// Captured assets (slice 2). Empty when no fills/vectors were exported.
  assets: AssetCache;
  /// Captured design tokens from Figma variables.
  tokens: ExtractedTokens;
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
  const assets = new AssetCache();
  const tokens = await extractTokens(diagnostics);
  const ctx: WalkCtx = {
    cfg,
    diagnostics,
    nodeCount: 0,
    truncated: false,
    pathStack: [],
    assets,
    tokens,
  };

  const roots: ExtractedFigmaNode[] = [];
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (ctx.truncated) break;
    ctx.pathStack.push(`/root[${i}]`);
    const extracted = await walk(n, null, i, ctx, null);
    ctx.pathStack.pop();
    if (extracted) roots.push(extracted);
  }

  return {
    roots,
    diagnostics: ctx.diagnostics,
    nodeCount: ctx.nodeCount,
    truncated: ctx.truncated,
    assets,
    tokens,
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
  assets: AssetCache;
  tokens: ExtractedTokens;
}

/// Whether the parent uses auto-layout (flex). When false, children need
/// position:absolute + top/left to reproduce the Figma layout.
function parentIsAutoLayout(parent: SceneNode | null): boolean {
  if (!parent) return false;
  if (parent.type !== "FRAME" && parent.type !== "COMPONENT" && parent.type !== "INSTANCE" && parent.type !== "COMPONENT_SET") {
    return false;
  }
  const mode = (parent as FrameNode).layoutMode;
  return mode === "HORIZONTAL" || mode === "VERTICAL";
}

async function walk(
  node: SceneNode,
  parentId: string | null,
  zOrder: number,
  ctx: WalkCtx,
  parent: SceneNode | null = null,
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

  // Position: if parent has no auto-layout, child needs absolute positioning.
  // Compute position from absoluteBoundingBox deltas rather than node.x/y
  // because node.x is in the IMMEDIATE parent's coord space — for Figma
  // GROUP parents (which don't have their own coord space) node.x is
  // actually in the group's grandparent space, which would double-count
  // the group's offset.
  if (!parentIsAutoLayout(parent) && parent !== null) {
    const childBB = "absoluteBoundingBox" in node ? node.absoluteBoundingBox : null;
    const parentBB =
      "absoluteBoundingBox" in parent
        ? (parent as SceneNode & { absoluteBoundingBox: Rect | null }).absoluteBoundingBox
        : null;
    if (childBB && parentBB) {
      ex.style.position = "absolute";
      ex.style.left = childBB.x - parentBB.x;
      ex.style.top = childBB.y - parentBB.y;
    } else if ("x" in node && "y" in node) {
      // Fallback: node.x/y. Correct for Frame parents; off by parent
      // offset for Group parents, but the absoluteBoundingBox path above
      // should catch most cases.
      ex.style.position = "absolute";
      ex.style.left = node.x;
      ex.style.top = node.y;
    }
  }

  // Text content + dominant style
  if (node.type === "TEXT") {
    ex.content = (node as TextNode).characters;
    extractTextStyle(node as TextNode, ex.style, ctx);
  }

  // INSTANCE: capture component metadata so Phase 3 can recognize Pulp library widgets.
  if (node.type === "INSTANCE") {
    await captureInstanceMetadata(node as InstanceNode, ex, ctx);

    // Track A2 — audio-widget instances export as a single PNG so the
    // import lane can attach them as a sprite-strip skin via the A1
    // setKnobSpriteStrip bridge. The native widget keeps full
    // interactivity; the PNG provides Figma-quality visual fidelity.
    // Detection is conservative (name-based) and ONLY fires for INSTANCE
    // nodes whose main-component name maps to a known audio-widget kind.
    // Non-audio instances continue to walk their child structure as
    // normal.
    const widgetKind = audioWidgetKindFromName(
      ex.main_component_name ?? node.name,
    );
    if (widgetKind) {
      const res = await ctx.assets.captureExportedNode(node, "PNG");
      if ("assetId" in res) {
        ex.asset_ref = res.assetId;
        ex.library_widget_kind = widgetKind;
        // Drop children so the importer doesn't double-render the
        // body's vector layers underneath the skinned widget.
        ex.children = [];
        return ex;
      } else {
        pushDiag(ctx, "info", "widget-export-failed", "capture_partial",
          `Audio-widget instance ${node.name}: ${res.error}`);
      }
    }
  }

  // Resolve any pending image fills now that we're async.
  if (ex.style.background_image && ex.style.background_image.startsWith("pending:")) {
    const imgHash = ex.style.background_image.substring("pending:".length);
    const assetId = await ctx.assets.captureImageFill(imgHash);
    delete ex.style.background_image;
    if (assetId) {
      ex.asset_ref = assetId;
      ex.type = "image";
    } else {
      pushDiag(ctx, "warning", "image-fill-unresolved", "unresolved_asset",
        `Image fill with hash ${imgHash} could not be fetched.`);
    }
  }

  // Vector-like nodes → SVG asset export (Phase 2a slice 2).
  const isVectorLike =
    node.type === "VECTOR" ||
    node.type === "BOOLEAN_OPERATION" ||
    node.type === "STAR" ||
    node.type === "POLYGON" ||
    node.type === "LINE";
  if (isVectorLike) {
    // Skip exports for degenerate (sub-pixel) vectors — they're invisible strokes / artifacts.
    const bounds = ex.absolute_bounds;
    const tiny = bounds.w < 1 && bounds.h < 1;
    if (!tiny) {
      const res = await ctx.assets.captureExportedNode(node, "PNG");
      if ("assetId" in res) {
        ex.asset_ref = res.assetId;
        ex.type = "image";
      } else {
        pushDiag(ctx, "info", "vector-export-failed", "capture_partial",
          `Vector ${node.name}: ${res.error}`);
      }
    }
  }

  // FRAME-as-illustration heuristic: if a frame's recursive content is purely
  // vector nodes (no text, no instances, no nested non-vector frames), export
  // the whole frame as a single SVG. Catches "shape illustration" frames like
  // Torus / Triangle / Pentagon / Cube where Figma stitches several vectors
  // into one visual.
  if (
    !isVectorLike &&
    !ex.asset_ref &&
    (node.type === "FRAME" || node.type === "GROUP") &&
    "children" in node &&
    (node as ChildrenMixin).children.length > 0 &&
    isPureVectorIllustration(node)
  ) {
    const res = await ctx.assets.captureExportedNode(node, "PNG");
    if ("assetId" in res) {
      ex.asset_ref = res.assetId;
      ex.type = "image";
      // We replaced the frame with its rasterized illustration; drop the children
      // so the importer doesn't double-render.
      ex.children = [];
      return ex;
    } else {
      pushDiag(ctx, "info", "illustration-export-failed", "capture_partial",
        `Illustration frame ${node.name}: ${res.error}`);
    }
  }

  // Recurse for container nodes that have children.
  if ("children" in node) {
    const children = (node as ChildrenMixin).children;
    for (let i = 0; i < children.length; i++) {
      ctx.pathStack.push(`/children[${i}]`);
      const child = await walk(children[i], node.id, i, ctx, node);
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

  // Capture absoluteRenderBounds for nodes with effects (drop shadows
  // especially). The render-bounds extend past the bounding box by the
  // bleed of any visual effect. Downstream uses:
  //  1. widgets.cpp Knob::paint draws PNGs at their natural render-bounds
  //     size instead of dividing PNG-pixels by a hardcoded export scale.
  //  2. tools/figma-plugin asset-bleed lint flags assets where the bleed
  //     ratio (render/bounding) exceeds a threshold so the importer can
  //     react before the user sees a squished knob.
  if (
    "absoluteRenderBounds" in n &&
    n.absoluteRenderBounds &&
    "absoluteBoundingBox" in n &&
    n.absoluteBoundingBox
  ) {
    const r = n.absoluteRenderBounds;
    const b = n.absoluteBoundingBox;
    // Only emit when the render-bounds materially exceed the bounding box
    // (drop-shadow bleed, stroke extending beyond, etc.). Skips noise on
    // exact-fit nodes where the two are identical.
    const inflated =
      r.width > b.width + 0.5 ||
      r.height > b.height + 0.5 ||
      r.x < b.x - 0.5 ||
      r.y < b.y - 0.5;
    if (inflated) {
      s.render_bounds = {
        w: r.width,
        h: r.height,
        dx: r.x - b.x,  // negative = bleed extends LEFT of bounding box
        dy: r.y - b.y,  // negative = bleed extends ABOVE bounding box
      };
      // Asset-bleed lint — surface the outlier nodes at extraction time so
      // the importer can react before the user sees a squished knob. The
      // 1.5× threshold catches drop shadows (typical 2-3× horiz expansion
      // on knobs) without warning on every node that has any bleed at all.
      const ratioW = b.width > 0 ? r.width / b.width : 1;
      const ratioH = b.height > 0 ? r.height / b.height : 1;
      const peak = Math.max(ratioW, ratioH);
      if (peak >= 1.5) {
        pushDiag(
          ctx,
          "info",
          "asset.bleed",
          "capture_partial",
          `bleed: "${n.name}" layout ${b.width.toFixed(0)}×${b.height.toFixed(0)} ` +
            `→ render ${r.width.toFixed(0)}×${r.height.toFixed(0)} ` +
            `(${ratioW.toFixed(1)}× × ${ratioH.toFixed(1)}×)`,
        );
      }
    }
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
        // Slice 2: extract image fill bytes via Figma's imageHash, cache by sha256.
        const imgHash = (first as ImagePaint).imageHash;
        if (imgHash) {
          // Schedule asset capture; rejoined via a microtask so we don't block this synchronous walk.
          // Note: extractStyle is synchronous; the caller will retroactively call
          // captureImageFill via the deferred path. For slice 2, we mark with a placeholder
          // and use a side-channel — but the simpler thing is to make extractStyle async-aware.
          // SEE: image fills are wired via captureImageFillsForNode after style extraction.
          s.background_image = `pending:${imgHash}`;
        }
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
  if (typeof t.letterSpacing === "object" && t.letterSpacing) {
    const ls = t.letterSpacing as { value: number; unit: "PIXELS" | "PERCENT" };
    if (ls.unit === "PIXELS") {
      s.letter_spacing = ls.value;
    } else if (ls.unit === "PERCENT" && typeof t.fontSize === "number") {
      // Figma encodes "tracking" as percent-of-font-size. Convert to pixels
      // so downstream consumers don't need to know about the percent unit.
      s.letter_spacing = (ls.value / 100) * t.fontSize;
    }
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
  if (c.a === undefined || c.a >= 1) {
    return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  }
  return `rgba(${r}, ${g}, ${b}, ${c.a.toFixed(3)})`;
}

function hex2(n: number): string {
  return n.toString(16).padStart(2, "0");
}

function gradientToCss(p: GradientPaint): string {
  if (!p.gradientStops || p.gradientStops.length === 0) return "linear-gradient(transparent, transparent)";
  // Pulp's setBackgroundGradient bridge takes color stop positions implicitly by
  // index, and its parseColor doesn't strip trailing `Npc%` from a token.
  // Emit colors only (no inline percentages).
  const stops = p.gradientStops.map((s) => rgbaToCss(s.color)).join(", ");
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

/// Detect audio widget kind from an instance's main-component name. Used
/// to flatten widget instances to a PNG skin (Track A2). Patterns match
/// Pulp's IR-side detect_audio_widget() (core/view/src/design_import.cpp).
function audioWidgetKindFromName(name: string): import("./extract-model").AudioWidgetKind | undefined {
  const lower = name.toLowerCase();
  if (lower.includes("knob") || lower.includes("dial")) return "knob";
  if (lower.includes("fader") || lower.includes("slider")) return "fader";
  if (lower.includes("meter") || lower.includes("level") || lower.includes("vu")) return "meter";
  if (lower.includes("xypad") || lower.includes("xy_pad") || lower.includes("xy-pad")) return "xy_pad";
  if (lower.includes("waveform")) return "waveform";
  if (lower.includes("spectrum")) return "spectrum";
  return undefined;
}

/// Heuristic: returns true if the node and all its recursive descendants are
/// vector-like primitives (or empty frames wrapping them). Used to detect
/// "shape illustration" frames that should export as a single SVG.
function isPureVectorIllustration(node: SceneNode): boolean {
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
// Instance metadata capture (Phase 2a slice 2)

async function captureInstanceMetadata(
  inst: InstanceNode,
  ex: ExtractedFigmaNode,
  ctx: WalkCtx,
): Promise<void> {
  let main: ComponentNode | null = null;
  try {
    main = await inst.getMainComponentAsync();
  } catch {
    return;
  }
  if (!main) return;

  ex.main_component_id = main.id;
  ex.main_component_name = main.name;
  ex.remote_library = main.remote === true;

  // ComponentSet (variant) parent — for grouped components like Pulp / Knob
  const parent = main.parent;
  if (parent && parent.type === "COMPONENT_SET") {
    ex.component_set_name = parent.name;
    if ("key" in parent && typeof parent.key === "string") {
      ex.component_key = parent.key;
    }
  } else {
    if ("key" in main && typeof main.key === "string") {
      ex.component_key = main.key;
    }
  }

  // Instance prop values (the actual values the designer typed in)
  try {
    const props = inst.componentProperties;
    if (props) {
      const out: Record<string, { type: string; value: string | number | boolean }> = {};
      for (const key of Object.keys(props)) {
        const p = props[key];
        out[key] = { type: p.type, value: p.value as string | number | boolean };
      }
      ex.component_properties = out;
    }
  } catch {
    // ignore
  }

  // Variant axis selections (e.g. size=sm, state=default)
  try {
    const variants = inst.variantProperties;
    if (variants) {
      ex.variant_properties = { ...variants };
    }
  } catch {
    // ignore
  }
}
