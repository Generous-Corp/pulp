// Figma scene walker.
//
// Walks a Figma selection and produces an `ExtractedFigmaNode` tree matching the
// shape declared in schema/figma-plugin-export-v1.json. This is the in-memory
// model serialized to JSON and consumed by the design importer. It captures
// geometry, frames + auto-layout, text + dominant typography, style, recursive
// children, assets, vector exports, and library component metadata.

import type {
  ExtractedFigmaNode,
  ExtractedStyle,
  ExtractedLayout,
  ExtractedDiagnostic,
} from "./extract-model";
import { AssetCache } from "./assets";
import {
  parseFrameKnobs,
  parsePanelBounds,
  detectOverlayControls,
  labelAndBindKnobs,
  decodeSvgBytes,
} from "./faithful-vector";
import { assessResolution } from "./resolve-control";
import { extractTokens, type ExtractedTokens } from "./tokens";
import {
  widgetKindByLibraryKey,
  widgetKindByNamePrefix,
  LIBRARY_VERSION,
} from "./library-registry";
import {
  paintToColor,
  rgbaToCss,
  gradientToCss,
  gradientFallbackFlat,
  mapNodeType,
  mapPrimaryAxisAlign,
  mapCounterAxisAlign,
  mapAxisSize,
  audioWidgetKindFromName,
  isPureVectorIllustration,
  collectFontFamilyAssets,
  extractConstraints,
} from "./extract-pure";

// ──────────────────────────────────────────────────────────────────────────
// Public entry point

export interface ExtractOptions {
  /// Whether to include children of hidden layers in the export.
  includeHidden?: boolean;
  /// Max nodes before the walker bails out with a diagnostic (perf safety).
  maxNodes?: number;
  /// Export each top-level frame's own SVG and render it pixel-faithfully via
  /// DesignFrameView, with knobs auto-detected from the SVG. Off by default
  /// for the widget-recognition lane.
  faithfulVector?: boolean;
}

export interface ExtractResult {
  roots: ExtractedFigmaNode[];
  /// Diagnostics raised during traversal (unsupported features, capture gaps).
  diagnostics: ExtractedDiagnostic[];
  /// Total node count walked (after filters).
  nodeCount: number;
  /// True if maxNodes was hit and the result is incomplete.
  truncated: boolean;
  /// Captured image/vector assets. Empty when no fills/vectors were exported.
  assets: AssetCache;
  /// Captured design tokens from Figma variables.
  tokens: ExtractedTokens;
  /// Deduplicated list of every font family/style/weight tuple referenced
  /// by text nodes in the extracted tree. Drives Pulp's runtime font
  /// resolution. Note: Figma's plugin API does NOT
  /// expose font binaries — `asset_id` stays empty for plain captures;
  /// it's populated only when the user supplies a TTF via the drag-drop
  /// escape hatch.
  font_family_assets: FontFamilyAsset[];
}

/// One row in the deduplicated font catalog carried on the envelope's
/// top-level `font_family_assets` field. See ExtractResult for context.
export interface FontFamilyAsset {
  /// Figma font family — e.g. "Inter", "Clash Grotesk".
  family: string;
  /// Figma style string — "Regular", "Semi Bold", "Italic", etc. Verbatim from `fontName.style`.
  style: string;
  /// Numeric font-weight when figma exposes it on the text node
  /// (`TextNode.fontWeight`). Omitted when not numeric (e.g. mixed weight).
  weight?: number;
  /// True when the style string indicates italic (case-insensitive
  /// substring match on "italic"). Lets the runtime resolve italic
  /// variants without re-parsing the style string.
  italic?: boolean;
  /// Set only when the user supplied a TTF/OTF via the drag-drop escape
  /// hatch. Points into the envelope's
  /// `asset_manifest` so the runtime can locate the bundled font file.
  asset_id?: string;
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
    // Faithful-vector is the DEFAULT import lane (matches the REST exporter's
    // --faithful-vector default-on): the frame renders its own SVG pixel-
    // faithfully with auto-detected INTERACTIVE overlays. Pass
    // `faithfulVector: false` for the legacy flat, static node tree.
    faithfulVector: opts.faithfulVector ?? true,
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
    if (extracted) {
      if (cfg.faithfulVector) await applyFaithfulVector(extracted, n, ctx);
      roots.push(extracted);
    }
  }

  // Collect the unique font-family/style/weight tuples used by text
  // nodes in the extracted tree. Runs after the walk because every text
  // node has already had its style normalized via extractTextStyle.
  const fontFamilyAssets = collectFontFamilyAssets(roots);

  return {
    roots,
    diagnostics: ctx.diagnostics,
    nodeCount: ctx.nodeCount,
    truncated: ctx.truncated,
    assets,
    tokens,
    font_family_assets: fontFamilyAssets,
  };
}

/// Walk the extracted IR once and produce a deduplicated catalog of
/// every (family, style, weight, italic) tuple referenced by text nodes.
/// Order is stable: families appear in first-encounter order, styles
/// within a family in first-encounter order. That stability matters for
/// snapshot tests that compare envelope output.
// Font-family collection stays in extract-pure.ts so this file stays focused on
// async Figma API calls and tree assembly.

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

  // Resize constraints — same gate as absolute positioning (plus the
  // layoutPositioning opt-out): constraints govern a node placed in its
  // parent's coordinate space. A FLOWING auto-layout child is sized by the
  // stack (layout.width_mode/height_mode above), and its stale constraints
  // would fight that with margins/grow the design never asked for.
  const absoluteInStack =
    "layoutPositioning" in node &&
    (node as SceneNode & { layoutPositioning?: string }).layoutPositioning === "ABSOLUTE";
  if (parent !== null && (!parentIsAutoLayout(parent) || absoluteInStack)) {
    const constraints = extractConstraints(node);
    if (constraints) ex.constraints = constraints;
  }

  // Text content + dominant style
  if (node.type === "TEXT") {
    ex.content = (node as TextNode).characters;
    extractTextStyle(node as TextNode, ex.style, ctx);
  }

  // INSTANCE: capture component metadata so Pulp library widgets can be recognized.
  if (node.type === "INSTANCE") {
    await captureInstanceMetadata(node as InstanceNode, ex, ctx);

    // Pulp Library component recognition.
    //
    // Recognition order:
    //   1. Authoritative key-based match: ex.component_key matches a
    //      Pulp-library component_set_key from library-manifest.json.
    //      This is the canonical path — designs that pulled in
    //      the published Pulp library hit this regardless of layer name.
    //   2. Name-prefix fallback: name starts with a manifest-registered
    //      prefix (e.g. "Pulp / Knob"). Lets designs use the convention
    //      without depending on the published library file.
    //   3. Permissive name match: the broader audioWidgetKindFromName()
    //      heuristic ("knob" / "fader" / "meter" appearing anywhere in
    //      the name) — preserves sprite-strip detection on ad-hoc designs.
    //
    // The first match wins. When a library or prefix match fires we
    // also stamp ex.library_version so the importer can tell apart
    // "real published library" instances from heuristic detections.
    let widgetKind = widgetKindByLibraryKey(ex.component_key);
    if (widgetKind) {
      ex.library_version = LIBRARY_VERSION;
    } else {
      widgetKind = widgetKindByNamePrefix(ex.main_component_name ?? node.name);
      if (widgetKind) {
        ex.library_version = LIBRARY_VERSION;
      } else {
        widgetKind = audioWidgetKindFromName(
          ex.main_component_name ?? node.name,
        );
      }
    }

    // When recognized, extract the structured property values (label /
    // min / max / value / units / binding) from componentProperties so
    // the downstream serializer can emit them at the IR node root for
    // design_import.cpp::parse_ir_node to pick up.
    if (widgetKind) {
      extractAudioPropsFromComponentProperties(ex);
    }

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

  // Vector-like nodes → exported asset.
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
        // Extract image fill bytes via Figma's imageHash, cache by sha256.
        const imgHash = (first as ImagePaint).imageHash;
        if (imgHash) {
          // Schedule asset capture; rejoined via a microtask so we don't block this synchronous walk.
          // Note: extractStyle is synchronous; the caller will retroactively call
          // captureImageFill via the deferred path. Mark with a placeholder
          // and use a side-channel — but the simpler thing is to make extractStyle async-aware.
          // SEE: image fills are wired via captureImageFillsForNode after style extraction.
          s.background_image = `pending:${imgHash}`;
        }
      } else if (first.type === "GRADIENT_RADIAL" || first.type === "GRADIENT_ANGULAR" || first.type === "GRADIENT_DIAMOND") {
        pushDiag(ctx, "warning", "complex-gradient", "unsupported_property",
          `${first.type} not supported; emitting flat first color fallback.`);
        // Store the flat fallback as background_color, NOT background_gradient.
        // The codegen routes background_gradient through setBackgroundGradient,
        // which expects a linear-gradient(...) string and fails to parse a
        // bare hex/rgba value — visible effect: the color never paints.
        // Subregion tints inside ELYSIUM cells (Frame 482 #2d2d2d) were lost
        // this way until the fix.
        s.background_color = gradientFallbackFlat(first as GradientPaint);
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
  // Per-corner radii. The Figma plugin API exposes each corner separately when
  // they differ (n.cornerRadius reads as `figma.mixed` then). The producer only
  // read the uniform value, so an asymmetric card imported via the plugin lane
  // lost its rounding — the shared per-corner codegen had nothing to emit. The
  // C++ parse_ir_style already reads these four fields.
  const tl = (n as { topLeftRadius?: number }).topLeftRadius;
  const tr = (n as { topRightRadius?: number }).topRightRadius;
  const br = (n as { bottomRightRadius?: number }).bottomRightRadius;
  const bl = (n as { bottomLeftRadius?: number }).bottomLeftRadius;
  if (
    typeof tl === "number" && typeof tr === "number" &&
    typeof br === "number" && typeof bl === "number" &&
    !(tl === tr && tr === br && br === bl)
  ) {
    s.border_top_left_radius = tl;
    s.border_top_right_radius = tr;
    s.border_bottom_right_radius = br;
    s.border_bottom_left_radius = bl;
    delete s.border_radius;
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
  // Read the "first character" style as the dominant style; range-specific
  // emission is not wired through this envelope yet.
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
  // Detect once range-specific font capture is wired.
  if (charLen > 0) {
    const hasMultiRangeFonts = false; // TODO: scan getRangeFontName
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

// Faithful-vector capture: export the frame's own SVG, register it as an
// image/svg+xml asset, and attach the render-mode + auto-detected interactive
// knobs the C++ DesignFrameView consumes. A capture failure leaves the node on
// the normal widget-recognition lane (diagnostic only) — the import degrades, it
// never blanks.
async function applyFaithfulVector(
  node: ExtractedFigmaNode,
  sceneNode: SceneNode,
  ctx: WalkCtx,
): Promise<void> {
  const res = await ctx.assets.captureExportedNode(sceneNode, "SVG");
  if ("error" in res) {
    pushDiag(ctx, "warning", "faithful-svg-export-failed", "capture_partial",
      `Faithful-vector frame ${sceneNode.name}: ${res.error}`);
    return;
  }
  node.render_mode = "faithful_svg";
  node.svg_asset_id = res.assetId;
  const entry = ctx.assets.entries().find((e) => e.asset_id === res.assetId);
  if (entry) {
    const svg = decodeSvgBytes(entry.bytes);
    // Geometry knobs from the SVG + source-metadata overlays from the node tree
    // (search/dropdown/stepper/tabs), mapped into the SVG's panel space — kept in
    // lockstep with the REST lane (figma_rest_export.py).
    const knobs = parseFrameKnobs(svg);
    // Knobs are geometry-detected (dome+needle in the SVG), with no node name.
    // Stamp them as affordance-resolved so the import report lists every
    // control, not just the overlays. A geometric knob's bounds are its hit
    // circle, so it is square by construction: no conflict, full confidence.
    for (let ki = 0; ki < knobs.length; ki++) {
      const k = knobs[ki];
      const d = 2 * (k.hit_radius || 0);
      const rep = assessResolution("knob", "", { w: d, h: d }, "affordance");
      k.resolution_rung = rep.resolution_rung;
      k.confidence_score = rep.confidence_score;
      if (rep.conflict_signals.length > 0) k.conflict_signals = rep.conflict_signals;
      if (!rep.verification_pass) k.verification_pass = false;
    }
    const panel = parsePanelBounds(svg);
    const overlays = detectOverlayControls(
      node,
      [node.absolute_bounds.x, node.absolute_bounds.y],
      [panel[0], panel[1]],
    );
    const all = knobs.concat(overlays);
    // Resolve each control against the frame's node tree: stamp a human `label`,
    // provenance `source_node_id` (for the manifest binding lane), and an opt-in
    // host-param `param_key` from a layer-name sigil. Node centers are compared in
    // the SVG space parseFrameKnobs uses, so pass the same `panel` origin the
    // overlay detector got. Lockstep with the REST lane's _label_elements.
    labelAndBindKnobs(all, node, [panel[0], panel[1]]);
    if (all.length > 0) node.interactive_elements = all;
  }
}

function pathOf(ctx: WalkCtx): string {
  return ctx.pathStack.join("");
}

/// Read the structured audio-widget properties (label, min, max, value, units,
/// binding) off `ex.component_properties` and stamp them onto `ex.audio_*`
/// fields so the serializer can emit them at the IR node root for
/// design_import.cpp::parse_ir_node to consume.
///
/// componentProperties keys carry a "#<unique-id>" suffix (e.g.
/// "binding#01:02"); we match on the prefix before the "#".
function extractAudioPropsFromComponentProperties(
  ex: ExtractedFigmaNode,
): void {
  if (!ex.component_properties) return;
  const cp = ex.component_properties;

  function getRawString(propName: string): string | undefined {
    for (const key of Object.keys(cp)) {
      const base = key.split("#")[0];
      if (base !== propName) continue;
      const entry = cp[key];
      if (!entry || entry.type !== "TEXT") continue;
      const v = entry.value;
      return typeof v === "string" ? v : String(v);
    }
    return undefined;
  }
  function getNumeric(propName: string): number | undefined {
    const s = getRawString(propName);
    if (s === undefined || s.length === 0) return undefined;
    const n = parseFloat(s);
    return Number.isFinite(n) ? n : undefined;
  }

  const label = getRawString("label");
  if (label !== undefined) ex.audio_label = label;
  const min = getNumeric("min");
  if (min !== undefined) ex.audio_min = min;
  const max = getNumeric("max");
  if (max !== undefined) ex.audio_max = max;
  const value = getNumeric("value");
  if (value !== undefined) ex.audio_default = value;
  const units = getRawString("units");
  if (units !== undefined && units.length > 0) ex.audio_units = units;
  const binding = getRawString("binding");
  if (binding !== undefined && binding.length > 0) ex.audio_binding = binding;
  // XYPad has a second binding for the Y axis. Only the XYPad library variant
  // defines this property; other widgets fall through.
  const binding_y = getRawString("binding_y");
  if (binding_y !== undefined && binding_y.length > 0) ex.audio_binding_y = binding_y;
}

// ──────────────────────────────────────────────────────────────────────────
// Instance metadata capture

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
