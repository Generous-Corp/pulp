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
  ExtractedDiagnostic,
  ExtractedTextRun,
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
  gradientToCss,
  gradientFallbackFlat,
  lowerFillPaints,
  lowerLayerBlendMode,
  collectGroupIsolationDiagnostics,
  scaleModeToObjectFit,
  dispatchNodeType,
  extractLayout,
  parentLayoutMode,
  audioWidgetKindFromName,
  isPureVectorIllustration,
  collectFontFamilyAssets,
  extractConstraints,
  extractStrokeStyle,
  lowerEffects,
  maskClipOutline,
  assessMaskFidelity,
  extractPrimitiveGeometryAttrs,
  extractDevMetadataAttrs,
  decodeRelativeTransform,
  extractBoundVariableBindings,
  correlateNormalizedColorBindings,
  stripNormalizedColorBindings,
  utf16ToUtf8ByteOffsets,
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
    diagnosedVariableIds: new Set(),
  };

  const roots: ExtractedFigmaNode[] = [];
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (ctx.truncated) break;
    ctx.pathStack.push(`/root[${i}]`);
    const extracted = await walk(n, null, i, ctx, null);
    ctx.pathStack.pop();
    if (extracted) {
      if (cfg.faithfulVector) {
        await applyFaithfulVector(extracted, n, ctx);
        if (extracted.render_mode === "faithful_svg")
          stripNormalizedColorBindings(extracted);
      }
      roots.push(extracted);
    }
  }

  // Isolate groups (explicit NORMAL on a container with blending
  // descendants) lose their isolation layer in the flat lowering — a
  // post-order pass over the finished tree states each one. Runs after the
  // walk because the condition reads the children's lowered blend modes.
  for (const root of roots) {
    for (const d of collectGroupIsolationDiagnostics(root)) {
      ctx.diagnostics.push(d);
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
  /// Variable ids already diagnosed as unresolvable bindings — one diagnostic
  /// per variable per export, not one per node that binds it.
  diagnosedVariableIds: Set<string>;
}

/// Whether the parent lays its children out itself — flex (HORIZONTAL /
/// VERTICAL) or GRID cell placement. When false, children need
/// position:absolute + top/left to reproduce the Figma layout.
function parentIsAutoLayout(parent: SceneNode | null): boolean {
  return parentLayoutMode(parent) !== null;
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
  // Exhaustive node-type dispatch (extract-pure.ts). Skips happen BEFORE the
  // node-count budget: a skipped SLICE or sticky emits no node, so it must not
  // consume maxNodes headroom either. Every skip and every fallback carries a
  // diagnostic — no node family reaches the envelope silently re-typed.
  const dispatch = dispatchNodeType(node.type, node.name);
  if (dispatch.diagnostic) {
    const d = dispatch.diagnostic;
    pushDiag(ctx, d.severity, d.code, d.kind, d.message);
  }
  if (dispatch.action === "skip") return null;
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

  const type = dispatch.type;
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
    layout: extractLayout(node, parent),
    children: [],
  };

  // Forge copy-back creates ordinary Figma nodes and records the DesignIR
  // semantics as plugin-owned metadata. Read that metadata before the normal
  // component-key/name recognizers so the next export retains widget kind and
  // parameter binding without guessing from pixels or layer names.
  if ("getPluginData" in node) {
    const roundtripKind = node.getPluginData("pulp.audio_widget");
    if (roundtripKind === "knob" || roundtripKind === "fader" ||
        roundtripKind === "meter" || roundtripKind === "xy_pad" ||
        roundtripKind === "waveform" || roundtripKind === "spectrum") {
      ex.library_widget_kind = roundtripKind;
      ex.audio_label = node.getPluginData("pulp.audio_label") || undefined;
      const readRoundtripNumber = (key: string): number | undefined => {
        const value = node.getPluginData(key);
        if (!value) return undefined;
        const parsed = Number(value);
        return Number.isFinite(parsed) ? parsed : undefined;
      };
      ex.audio_min = readRoundtripNumber("pulp.audio_min");
      ex.audio_max = readRoundtripNumber("pulp.audio_max");
      ex.audio_default = readRoundtripNumber("pulp.audio_default");
      ex.audio_units = node.getPluginData("pulp.audio_units") || undefined;
      ex.audio_binding = node.getPluginData("pulp.binding") || undefined;
      ex.audio_binding_y = node.getPluginData("pulp.binding_y") || undefined;
      ex.library_version = "forge-roundtrip-v1";
    }
    const sourceNodeId = node.getPluginData("pulp.source_node_id");
    const stableAnchorId = node.getPluginData("pulp.stable_anchor_id");
    if (sourceNodeId) {
      ex.attributes = {
        ...(ex.attributes ?? {}),
        "pulp:roundtrip_source_node_id": sourceNodeId,
      };
    }
    if (stableAnchorId) {
      ex.attributes = {
        ...(ex.attributes ?? {}),
        "pulp:roundtrip_stable_anchor_id": stableAnchorId,
      };
    }
  }

  // Strokes → box border (uniform or per-side), preserved figma:* stroke
  // provenance, and the multi-paint / complex-stroke diagnostics. Lives
  // outside extractStyle because the result spans style + attributes +
  // diagnostics.
  const stroke = extractStrokeStyle(node);
  if (stroke) {
    Object.assign(ex.style, stroke.style);
    if (stroke.attributes) ex.attributes = { ...(ex.attributes ?? {}), ...stroke.attributes };
    for (const d of stroke.diagnostics) pushDiag(ctx, d.severity, d.code, d.kind, d.message);
  }

  // Primitive-shape provenance (arc/donut data, star/polygon point counts,
  // corner smoothing, boolean operation) — namespaced figma:* attributes a
  // future path renderer needs to rebuild the primitive without a re-export.
  // The PNG capture below preserves the pixels; these preserve the semantics.
  const primitive = extractPrimitiveGeometryAttrs(node);
  if (primitive) ex.attributes = { ...(ex.attributes ?? {}), ...primitive };

  // Dev-mode metadata + authored export settings (description, dev status,
  // annotations, export settings) — provenance-only namespaced figma:* attrs.
  // Nothing renders from these, and export settings never override Pulp's
  // deterministic PNG/SVG capture policy: they are asset hints and round-trip
  // context for dev tooling.
  const devMeta = extractDevMetadataAttrs(node);
  if (devMeta) ex.attributes = { ...(ex.attributes ?? {}), ...devMeta };

  // Min/max sizing — style-level clamps parse_ir_style reads and the flex
  // engines lower to min/max width/height. Figma already honored them while
  // solving, so they cannot move the design-size replay; they bind on resize.
  const sized = node as SceneNode & {
    minWidth?: number | null; maxWidth?: number | null;
    minHeight?: number | null; maxHeight?: number | null;
  };
  if ("minWidth" in node) {
    if (typeof sized.minWidth === "number" && sized.minWidth > 0) ex.style.min_width = sized.minWidth;
    if (typeof sized.maxWidth === "number" && sized.maxWidth > 0) ex.style.max_width = sized.maxWidth;
    if (typeof sized.minHeight === "number" && sized.minHeight > 0) ex.style.min_height = sized.minHeight;
    if (typeof sized.maxHeight === "number" && sized.maxHeight > 0) ex.style.max_height = sized.maxHeight;
  }

  // Position: when the parent doesn't lay this child out — no auto-layout,
  // or the child opted out of the stack with layoutPositioning ABSOLUTE
  // (which Figma positions in the parent's coordinate space even inside a
  // flex/grid parent) — the child needs absolute positioning.
  // Compute position from absoluteBoundingBox deltas rather than node.x/y
  // because node.x is in the IMMEDIATE parent's coord space — for Figma
  // GROUP parents (which don't have their own coord space) node.x is
  // actually in the group's grandparent space, which would double-count
  // the group's offset.
  const absoluteInStack =
    "layoutPositioning" in node &&
    (node as SceneNode & { layoutPositioning?: string }).layoutPositioning === "ABSOLUTE";
  if (parent !== null && (!parentIsAutoLayout(parent) || absoluteInStack)) {
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

  // Resize constraints — same gate as absolute positioning: constraints
  // govern a node placed in its parent's coordinate space. A FLOWING
  // auto-layout child is sized by the stack (layout grow/align/width_mode
  // above), and its stale constraints would fight that with margins/grow
  // the design never asked for.
  if (parent !== null && (!parentIsAutoLayout(parent) || absoluteInStack)) {
    const constraints = extractConstraints(node);
    if (constraints) ex.constraints = constraints;
  }

  // Variable bindings — which token is bound to which property. The tokens
  // pass already built variableIdToName (Figma variable id → canonical token
  // name); this is the consumer that was missing: without it the map was
  // built and the per-node `boundVariables` never read, so every binding was
  // lost even though its token definition survived. An alias the token pass
  // didn't capture (remote-library / deleted variable) is skipped — the
  // envelope never carries a dangling id — and diagnosed once per variable.
  if ("boundVariables" in node) {
    const bound = extractBoundVariableBindings(
      (node as SceneNode & { boundVariables?: unknown }).boundVariables,
      ctx.tokens.variableIdToName,
      {
        resolvedModeByCollection:
          (node as SceneNode & { resolvedVariableModes?: Record<string, string> })
            .resolvedVariableModes ?? {},
        variableIdToCollectionId: ctx.tokens.variableIdToCollectionId ?? {},
        variableIdToModeName: ctx.tokens.variableIdToModeName ?? {},
      },
    );
    if (bound) {
      if (Object.keys(bound.bindings).length > 0) ex.bound_variables = bound.bindings;
      for (const id of bound.unresolved) {
        if (ctx.diagnosedVariableIds.has(id)) continue;
        ctx.diagnosedVariableIds.add(id);
        pushDiag(ctx, "warning", "variable-binding-unresolved", "capture_partial",
          `Variable ${id} is bound to a property of "${node.name}" but was not captured ` +
          `by the token pass (remote-library or deleted variable); the binding is dropped.`);
      }
    }
  }

  // Text content + dominant style. TEXT_PATH shares the text mixins
  // (characters, fontName, fills, ...) — its content must ride along even
  // though dispatch already diagnosed the flattened on-path layout.
  if (node.type === "TEXT" || node.type === "TEXT_PATH") {
    ex.content = (node as TextNode).characters;
    extractTextStyle(node as unknown as TextNode, ex, ctx);
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

  // Correlate source paint bindings only after every asset-promotion path.
  // Rasterized widgets/vectors/illustrations bake their color into an asset and
  // therefore cannot advertise a mutable normalized literal.
  if (ex.bound_variables && ex.type !== "image" && !ex.asset_ref) {
    correlateNormalizedColorBindings(
      node.type,
      "fills" in node ? (node as GeometryMixin).fills : undefined,
      "strokes" in node ? (node as GeometryMixin).strokes : undefined,
      ex.bound_variables,
      ex.style,
    );
  }

  // Rotation (audit "Rotation / transform" row). The affine's rotation lowers
  // to the CSS `rotate(<deg>deg)` the shared codegen already maps to
  // setRotation — the same lowering the .fig lane ships
  // (fig/scene.mjs::styleFor), with its guards mirrored field-for-field:
  //   - only a node WE positioned absolutely (a flowing auto-layout child's
  //     transform is layout output, not input — same gate as position and
  //     constraints above);
  //   - only meaningfully non-orthogonal angles (decodeRelativeTransform):
  //     a 90deg multiple keeps the box axis-aligned and the bounding-box
  //     placement above is already exact for it — re-rotating was the .fig
  //     lane's #6277 slider-fill regression;
  //   - never a vector-like node: its exportAsync capture renders the
  //     rotation INTO the pixels and absolute_bounds is the rotated AABB, so
  //     a second rotate() double-rotates (the .fig lane's VECTOR_LIKE
  //     exclusion). Widget-instance and illustration-frame exports returned
  //     early above for the same reason. An image FILL asset is different —
  //     the raw fill bytes are not a node render — so an image-filled box
  //     still rotates.
  // A skewed / scaled / mirrored matrix is NOT a pure rotation: diagnose
  // (transform-skew-approximated) and preserve the full matrix as
  // figma:transform_matrix instead of faking an angle.
  if (ex.style.position === "absolute" && !isVectorLike) {
    const decoded = decodeRelativeTransform(ex.relative_transform, node.name);
    if (decoded.kind === "rotate" &&
        "width" in node && typeof node.width === "number" &&
        "height" in node && typeof node.height === "number") {
      // Untransformed size: node.width/height. extractStyle used the rotated
      // AABB — right for an axis-aligned box, wrong once we rotate.
      const w = node.width;
      const h = node.height;
      const bb = "absoluteBoundingBox" in node ? node.absoluteBoundingBox : null;
      ex.style.width = w;
      ex.style.height = h;
      if (bb) {
        // The rotated AABB's center IS the node's center, and the renderer
        // pivots rotate() on the element center — so centering the unrotated
        // box in the AABB reproduces Figma's rendering exactly. style.left/
        // top above are AABB-relative already; shift by the half-size delta.
        ex.style.left = (ex.style.left as number) + (bb.width - w) / 2;
        ex.style.top = (ex.style.top as number) + (bb.height - h) / 2;
      } else {
        // node.x/y fallback: the transform's translation column, which Figma
        // pivots on the LOCAL origin. Solve left/top so the renderer's
        // center pivot lands the box where the origin pivot did
        // (fig/scene.mjs::styleFor formula): center = (x, y) + R(θ)·(w/2, h/2).
        const rad = (decoded.deg * Math.PI) / 180;
        const c = Math.cos(rad);
        const s = Math.sin(rad);
        ex.style.left = (ex.style.left as number) + c * (w / 2) - s * (h / 2) - w / 2;
        ex.style.top = (ex.style.top as number) + s * (w / 2) + c * (h / 2) - h / 2;
      }
      ex.style.transform = `rotate(${decoded.deg.toFixed(2)}deg)`;
      ex.attributes = { ...(ex.attributes ?? {}), "figma:transform_matrix": decoded.matrixAttr };
    } else if (decoded.kind === "unrepresentable") {
      const d = decoded.diagnostic;
      pushDiag(ctx, d.severity, d.code, d.kind, d.message);
      ex.attributes = { ...(ex.attributes ?? {}), "figma:transform_matrix": decoded.matrixAttr };
    }
  }

  // Recurse for container nodes that have children, honoring Figma's `isMask`
  // flag: a mask child is painted NOWHERE — its outline CLIPS the siblings
  // painted after it (above it in paint order), and its own fill never
  // reaches the canvas. Materializing the flag as a normal child would paint
  // an opaque panel OVER the very content the design clips. Lowering mirrors
  // the .fig decoder (fig/scene.mjs::walkChildren): the siblings above the
  // mask move into a synthetic wrapper that spans the parent and carries the
  // mask outline as a CSS clip-path. Siblings BELOW the mask stay outside the
  // wrapper, unclipped — exactly Figma's scope — and a second mask opens a
  // second wrapper inside the first, so stacked masks intersect the way
  // nested clips do.
  if ("children" in node) {
    const children = (node as ChildrenMixin).children;
    // Open mask scopes: synthetic wrappers, pruned when they end up empty.
    const scopes: Array<{ wrapper: ExtractedFigmaNode; holder: ExtractedFigmaNode[] }> = [];
    let target = ex.children;
    for (let i = 0; i < children.length; i++) {
      const child = children[i];
      if ("isMask" in child && (child as SceneNode & { isMask?: boolean }).isMask === true) {
        // A hidden mask neither paints nor clips.
        if (child.visible === false) continue;
        ctx.pathStack.push(`/children[${i}]`);
        const wrapper = beginMaskScope(child, node, i, ctx);
        ctx.pathStack.pop();
        if (wrapper) {
          target.push(wrapper);
          scopes.push({ wrapper, holder: target });
          target = wrapper.children;
        }
        continue;
      }
      ctx.pathStack.push(`/children[${i}]`);
      const walked = await walk(child, node.id, i, ctx, node);
      ctx.pathStack.pop();
      if (walked) target.push(walked);
    }
    // A scope with nothing above the mask paints nothing. Deepest-first, so a
    // scope left holding only an emptied deeper scope collapses too.
    for (let i = scopes.length - 1; i >= 0; i--) {
      const { wrapper, holder } = scopes[i];
      if (wrapper.children.length === 0) holder.splice(holder.indexOf(wrapper), 1);
    }
  }

  return ex;
}

/// Lower one sibling mask into its synthetic clip wrapper, or return null
/// (with a diagnostic) when no lowering exists — the siblings then render
/// unmasked rather than occluded. The wrapper spans the parent from (0, 0),
/// so the outline (already in the parent's space) clips exactly where the
/// design put the mask and the masked siblings keep their coordinates.
function beginMaskScope(
  mask: SceneNode,
  parent: SceneNode,
  zOrder: number,
  ctx: WalkCtx,
): ExtractedFigmaNode | null {
  if (parentIsAutoLayout(parent)) {
    // The wrapper is absolutely placed, which would yank flowed siblings out
    // of the flex pass. No lowering — but never paint the mask.
    pushDiag(ctx, "warning", "mask-approximated", "unsupported_property",
      `Mask "${mask.name}" inside an auto-layout parent has no lowering; siblings flow unmasked.`);
    return null;
  }
  const d = maskClipOutline(mask, parent);
  if (!d) {
    pushDiag(ctx, "warning", "mask-approximated", "unsupported_property",
      `Mask "${mask.name}" outline unresolvable; siblings render unmasked.`);
    return null;
  }
  // An outline clip is exact for an outline (VECTOR) mask and for an alpha
  // mask whose content is one opaque solid. Anything softer flattens to the
  // hard outline — say so: a mask that clips harder than the design intended
  // looks like a cropping bug, not a dropped property.
  const fidelity = assessMaskFidelity(mask);
  if (fidelity === "luminance") {
    pushDiag(ctx, "warning", "mask-luminance-approximated", "unsupported_property",
      `Luminance mask "${mask.name}" flattened to its outline clip; pixel-brightness alpha is not reproduced.`);
  } else if (fidelity === "soft_alpha") {
    pushDiag(ctx, "warning", "complex-mask-flattened", "fallback_used",
      `Alpha mask "${mask.name}" flattened to its outline; soft or partial alpha is not reproduced.`);
  }
  const pw = "width" in parent ? parent.width : 0;
  const ph = "height" in parent ? parent.height : 0;
  const pb = "absoluteBoundingBox" in parent
    ? (parent as SceneNode & { absoluteBoundingBox: Rect | null }).absoluteBoundingBox
    : null;
  return {
    type: "frame",
    figma_type: "FRAME",
    name: `${mask.name || "mask"} (mask scope)`,
    // Synthetic, so it must not collide with any real node in tools that join
    // on node id, and must never be name-guessed into a widget.
    figma_node_id: `${mask.id}/mask-scope`,
    parent_id: parent.id,
    z_order: zOrder,
    absolute_bounds: pb
      ? { x: pb.x, y: pb.y, w: pb.width, h: pb.height }
      : { x: 0, y: 0, w: pw, h: ph },
    relative_transform: [[1, 0, 0], [0, 1, 0]],
    visible: true,
    locked: false,
    opacity: 1,
    blend_mode: "PASS_THROUGH",
    style: {
      width: Math.round(pw),
      height: Math.round(ph),
      position: "absolute",
      left: 0,
      top: 0,
      clip_path: `path("${d}")`,
    },
    layout: {},
    audio_widget: "none",
    children: [],
  };
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

  // Fills — ordered paint-stack lowering (extract-pure.ts::lowerFillPaints).
  // Figma renders `fills` bottom→top; the IR has one color + one gradient +
  // one image background slot painted in that order, so a [solid…, gradient?,
  // image?] prefix lowers exactly (leading solids composite source-over) and
  // anything beyond it raises multi-paint-flattened / unsupported-paint-type
  // instead of vanishing behind a first-visible-paint-wins pick.
  if ("fills" in n && Array.isArray(n.fills) && n.fills.length > 0) {
    const lowered = lowerFillPaints(n.fills as readonly Paint[]);
    for (const d of lowered.diagnostics) {
      pushDiag(ctx, d.severity, d.code, d.kind, `${n.name}: ${d.message}`);
    }
    if (lowered.backgroundColor) s.background_color = lowered.backgroundColor;
    const grad = lowered.gradientPaint;
    if (grad) {
      if (grad.type === "GRADIENT_LINEAR") {
        s.background_gradient = gradientToCss(grad);
      } else if (s.background_color) {
        // A radial/angular/diamond gradient stacked over a solid: the flat
        // fallback would overwrite the exact composite below it, so keep the
        // solid and state the gradient's loss instead.
        pushDiag(ctx, "warning", "complex-gradient", "unsupported_property",
          `${n.name}: ${grad.type} over a solid fill not supported; solid kept, gradient dropped.`);
      } else {
        pushDiag(ctx, "warning", "complex-gradient", "unsupported_property",
          `${grad.type} not supported; emitting flat first color fallback.`);
        // Store the flat fallback as background_color, NOT background_gradient.
        // The codegen routes background_gradient through setBackgroundGradient,
        // which expects a linear-gradient(...) string and fails to parse a
        // bare hex/rgba value — visible effect: the color never paints.
        // Subregion tints inside ELYSIUM cells (Frame 482 #2d2d2d) were lost
        // this way until the fix.
        s.background_color = gradientFallbackFlat(grad);
      }
    }
    const image = lowered.imagePaint;
    if (image && image.imageHash) {
      // Schedule asset capture; rejoined via a microtask so we don't block this
      // synchronous walk. extractStyle is synchronous; the caller retroactively
      // resolves the placeholder via captureImageFill after style extraction.
      s.background_image = `pending:${image.imageHash}`;
      // Scale mode → CSS object-fit, honored by ImageView::paint once the fill
      // becomes an image node (FILL → cover, FIT → contain). CROP approximates
      // with cover (the imageTransform crop window has no CSS equivalent) and
      // TILE keeps the stretch default — both say so.
      const { fit, exact } = scaleModeToObjectFit(image.scaleMode);
      if (fit) s.object_fit = fit;
      if (!exact) {
        pushDiag(ctx, "warning", "image-scale-approximated", "capture_partial",
          `${n.name}: image fill scale mode ${image.scaleMode} has no exact ` +
          `equivalent; ${fit ? `approximated as object-fit: ${fit}` : "rendered stretched to the box"}.`);
      }
      // Paint-level opacity — distinct from layer opacity. For a childless
      // node the fill IS the node's only content, so folding it into the
      // layer opacity composites identically; with children present the fold
      // would fade them too, so the loss is diagnosed instead.
      const imgOpacity = image.opacity !== undefined ? image.opacity : 1;
      if (imgOpacity < 1) {
        const childCount = "children" in n ? (n as ChildrenMixin).children.length : 0;
        if (childCount === 0) {
          s.opacity = (s.opacity !== undefined ? s.opacity : 1) * imgOpacity;
        } else {
          pushDiag(ctx, "warning", "image-opacity-dropped", "unsupported_property",
            `${n.name}: image fill opacity ${imgOpacity.toFixed(3)} cannot fold ` +
            `into layer opacity (node has children); image renders opaque.`);
        }
      }
    }
  }

  // Strokes (border) are extracted by extractStrokeStyle (extract-pure.ts) at
  // the walk level — the per-side / dashed / multi-paint handling also emits
  // node attributes and diagnostics, which a style-only extractor cannot carry.

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

  // Layer blend mode — normalized to the CSS keyword here (matching the .fig
  // lane) so the consumer reads all three lanes' `style` channel identically;
  // the raw Figma mode still rides in the `figma` block for provenance. A mode
  // outside the shared supported-blend table lowers to nothing WITH a
  // diagnostic — silently ignoring it still paints, confidently wrong.
  if ("blendMode" in n) {
    const blend = lowerLayerBlendMode((n as BlendMixin).blendMode);
    if (blend.css) s.mix_blend_mode = blend.css;
    if (blend.diagnostic) {
      const d = blend.diagnostic;
      pushDiag(ctx, d.severity, d.code, d.kind, `${n.name}: ${d.message}`);
    }
  }

  // Effects — the ordered stack lowers in extract-pure (shadows → box_shadow,
  // layer blur → filter, background blur → backdrop_filter); anything with no
  // lowering (NOISE, TEXTURE, GLASS, newer families) comes back as a
  // diagnostic so the drop is never silent.
  if ("effects" in n && Array.isArray(n.effects)) {
    const lowered = lowerEffects(n.effects as readonly Effect[]);
    if (lowered.box_shadow) s.box_shadow = lowered.box_shadow;
    if (lowered.filter) s.filter = lowered.filter;
    if (lowered.backdrop_filter) s.backdrop_filter = lowered.backdrop_filter;
    for (const d of lowered.diagnostics) {
      pushDiag(ctx, d.severity, d.code, d.kind, `${n.name}: ${d.message}`);
    }
  }

  // Overflow — Figma's clipsContent maps loosely to overflow: clip
  if ("clipsContent" in n && (n as FrameNode).clipsContent === true) {
    s.overflow = "clip";
  }

  return s;
}

// The letter-spacing shape the Plugin API returns (per node and per segment).
// Figma encodes "tracking" either in pixels or as percent-of-font-size.
interface FigmaLetterSpacing {
  value: number;
  unit: "PIXELS" | "PERCENT";
}

function letterSpacingPx(
  ls: FigmaLetterSpacing | undefined,
  fontSize: number | undefined,
): number | undefined {
  if (!ls || typeof ls.value !== "number") return undefined;
  if (ls.unit === "PIXELS") return ls.value;
  if (ls.unit === "PERCENT" && typeof fontSize === "number") {
    return (ls.value / 100) * fontSize;
  }
  return undefined;
}

// One styled segment as returned by TextNode.getStyledTextSegments with the
// field list requested below. `start`/`end` are UTF-16 code-unit indices.
interface StyledSegment {
  characters: string;
  start: number;
  end: number;
  fontSize: number;
  fontName: FontName;
  fontWeight: number;
  fills: readonly Paint[];
  letterSpacing: FigmaLetterSpacing;
  textDecoration: "NONE" | "UNDERLINE" | "STRIKETHROUGH";
}

function segmentColor(fills: readonly Paint[] | undefined): string | undefined {
  if (!Array.isArray(fills)) return undefined;
  const first = fills.find((p) => p.type === "SOLID" && p.visible !== false);
  return first ? paintToColor(first as SolidPaint) : undefined;
}

/// Mixed-style capture: segment the node with getStyledTextSegments and emit
/// the ordered per-range deltas against the dominant style, with UTF-8 byte
/// offsets (converted from the API's UTF-16 code-unit indices). A homogeneous
/// node returns no runs — the flat dominant style stays the whole story, which
/// is the path the consumer prefers for single-style text.
function extractTextRuns(t: TextNode, s: ExtractedStyle): ExtractedTextRun[] {
  const getSegments = (t as unknown as {
    getStyledTextSegments?: (fields: string[]) => StyledSegment[];
  }).getStyledTextSegments;
  if (typeof getSegments !== "function") return [];
  let segments: StyledSegment[];
  try {
    segments = getSegments.call(t, [
      "fontSize", "fontName", "fontWeight", "fills",
      "letterSpacing", "textDecoration",
    ]);
  } catch {
    return [];
  }
  if (!Array.isArray(segments) || segments.length < 2) return [];

  // Backfill the dominant style from the first segment wherever the
  // node-level property read `figma.mixed` (a symbol) and left the base field
  // unset — that first-character style is the dominant the flat path
  // promises, and the run deltas below are computed against it.
  const first = segments[0];
  if (s.font_size === undefined && typeof first.fontSize === "number") {
    s.font_size = first.fontSize;
  }
  if (s.font_weight === undefined && typeof first.fontWeight === "number") {
    s.font_weight = first.fontWeight;
  }
  if (s.font_family === undefined && first.fontName &&
      typeof first.fontName.family === "string") {
    s.font_family = first.fontName.family;
    s.font_style = /italic/i.test(first.fontName.style) ? "italic" : "normal";
  }
  if (s.color === undefined) {
    const c = segmentColor(first.fills);
    if (c) s.color = c;
  }
  if (s.letter_spacing === undefined) {
    const ls = letterSpacingPx(first.letterSpacing, first.fontSize);
    if (ls !== undefined && ls !== 0) s.letter_spacing = ls;
  }

  const toByte = utf16ToUtf8ByteOffsets(t.characters);
  const byteAt = (u16: number): number =>
    u16 >= 0 && u16 < toByte.length ? toByte[u16] : toByte[toByte.length - 1];

  const runs: ExtractedTextRun[] = [];
  for (const seg of segments) {
    const run: ExtractedTextRun = {
      start: byteAt(seg.start),
      end: byteAt(seg.end),
    };
    if (run.end <= run.start) continue;
    // Style delta vs the node's dominant style — only overridden fields are
    // emitted; everything else inherits (mirrors REST's override-table runs).
    if (typeof seg.fontSize === "number" && seg.fontSize !== s.font_size) {
      run.fontSize = seg.fontSize;
    }
    if (typeof seg.fontWeight === "number" && seg.fontWeight !== s.font_weight) {
      run.fontWeight = seg.fontWeight;
    }
    if (seg.fontName && typeof seg.fontName.style === "string") {
      const italic = /italic/i.test(seg.fontName.style) ? "italic" : "normal";
      if (italic !== (s.font_style ?? "normal")) run.fontStyle = italic;
    }
    const color = segmentColor(seg.fills);
    if (color && color !== s.color) run.color = color;
    const ls = letterSpacingPx(seg.letterSpacing, seg.fontSize);
    if (ls !== undefined && ls !== 0 && ls !== s.letter_spacing) {
      run.letterSpacing = ls;
    }
    if (seg.textDecoration === "UNDERLINE") run.textDecoration = "underline";
    else if (seg.textDecoration === "STRIKETHROUGH") run.textDecoration = "line-through";
    if (Object.keys(run).length > 2) runs.push(run);
  }
  return runs;
}

function extractTextStyle(t: TextNode, ex: ExtractedFigmaNode, ctx: WalkCtx): void {
  const s = ex.style;
  // Read the "first character" style as the dominant style; mixed-style
  // ranges are then captured as per-range deltas in `runs` below.
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
  // Node-level vertical alignment within the design-reserved text slot.
  // Design authority — codegen honors it over the tall-slot heuristic
  // (including an explicit "top", which suppresses derived centering).
  if (t.textAlignVertical === "CENTER") s.vertical_align = "middle";
  else if (t.textAlignVertical === "BOTTOM") s.vertical_align = "bottom";
  else if (t.textAlignVertical === "TOP") s.vertical_align = "top";

  // Mixed styled ranges → ordered per-range deltas with UTF-8 byte offsets.
  // A homogeneous node emits no runs and keeps the flat single-style path.
  const runs = extractTextRuns(t, s);
  if (runs.length > 0) {
    ex.runs = runs;
  } else if (typeof (t as unknown as { getStyledTextSegments?: unknown })
      .getStyledTextSegments !== "function") {
    // Only reachable on a host without the segmentation API — say so instead
    // of silently flattening (the pre-capture behavior).
    pushDiag(ctx, "info", "text-ranges-flattened", "capture_partial",
      "Mixed font ranges in text node flattened to dominant style.");
  }

  // Preserved-not-lowered text metadata, namespaced so nothing downstream
  // mistakes it for a lowered style. Rendering support is tracked as partial
  // in compat/imports.json; defaults are omitted to keep envelopes lean.
  const preserved: Record<string, string> = {};
  const autoResize = (t as unknown as { textAutoResize?: string }).textAutoResize;
  if (typeof autoResize === "string" && autoResize !== "NONE") {
    preserved["figma:text_auto_resize"] = autoResize.toLowerCase();
  }
  const truncation = (t as unknown as { textTruncation?: string }).textTruncation;
  if (truncation === "ENDING" || autoResize === "TRUNCATE") {
    preserved["figma:text_truncation"] = "ending";
  }
  const maxLines = (t as unknown as { maxLines?: number | null }).maxLines;
  if (typeof maxLines === "number" && maxLines > 0) {
    preserved["figma:max_lines"] = String(maxLines);
  }
  const getHyperlink = (t as unknown as {
    getRangeHyperlink?: (start: number, end: number) => { type: string; value: string } | null | symbol;
  }).getRangeHyperlink;
  if (typeof getHyperlink === "function") {
    try {
      const link = getHyperlink.call(t, 0, charLen);
      // A symbol means figma.mixed (per-range links) — preserved only when the
      // whole node shares one URL target.
      if (link && typeof link === "object" && link.type === "URL" &&
          typeof link.value === "string" && link.value) {
        preserved["figma:hyperlink"] = link.value;
      }
    } catch {
      // Defensive: some node states throw on range queries; nothing to preserve.
    }
  }
  if (Object.keys(preserved).length > 0) {
    ex.attributes = { ...(ex.attributes ?? {}), ...preserved };
  }
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
