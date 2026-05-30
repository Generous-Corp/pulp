// Phase 2b — serialize an extracted scene into the v1 JSON envelope declared in
// schema/figma-plugin-export-v1.json. This is mostly a passthrough since the
// extractor's in-memory model already mirrors the envelope shape; the
// serializer's job is to add the envelope-level fields (format_version,
// provenance, library_manifest snapshot, asset_manifest, diagnostics) and
// to drop noise.

import type { ExtractedFigmaNode, ExtractedDiagnostic } from "./extract-model";
import type { AssetCache } from "./assets";
import type { ExtractedTokens } from "./tokens";
import type { FontFamilyAsset } from "./extract";

export interface SerializeContext {
  fileKey: string;
  rootNodeId: string;
  pluginVersion: string;
  libraryManifest?: LibraryManifestSnapshot;
  assets: AssetCache;
  tokens: ExtractedTokens;
  /// Deduplicated font catalogue produced by extractScene (#43a-rev).
  /// Empty array → no text nodes in the selection (or no font names
  /// captured). Emitted at envelope root as `font_family_assets`.
  fontFamilyAssets?: FontFamilyAsset[];
}

export interface LibraryManifestSnapshot {
  library_version: string;
  required_plugin_version: string;
  widget_keys: Record<string, string>;
}

export function serializeExport(
  roots: ExtractedFigmaNode[],
  diagnostics: ExtractedDiagnostic[],
  ctx: SerializeContext,
): unknown {
  // Multi-root: wrap in a synthetic frame so the schema's single-root contract holds.
  const root = roots.length === 1
    ? toEnvelopeNode(roots[0])
    : {
        type: "frame",
        name: "<multi-export>",
        figma_node_id: ctx.rootNodeId,
        style: {},
        layout: {},
        children: roots.map(toEnvelopeNode),
      };

  return {
    $schema: "https://pulp.dev/schemas/figma-plugin-export-v1.json",
    format_version: "2026.05-figma-plugin-v1",
    parser_version: "0.1.0",
    compat_schema_version: "0.3",
    provenance: {
      adapter: "figma-plugin",
      version: ctx.pluginVersion,
      source_uri: `figma://${ctx.fileKey}/${ctx.rootNodeId}`,
      exported_at: new Date().toISOString(),
    },
    library_manifest: ctx.libraryManifest ?? null,
    tokens: {
      colors: ctx.tokens.colors,
      dimensions: ctx.tokens.dimensions,
      strings: ctx.tokens.strings,
    },
    asset_manifest: {
      version: 1,
      assets: ctx.assets.entries().map((a) => ({
        asset_id: a.asset_id,
        original_uri: a.original_uri,
        original_uri_aliases: a.original_uri_aliases,
        local_path: `assets/${a.content_hash}.${extOf(a.mime)}`,
        content_hash: a.content_hash,
        mime: a.mime,
        width: a.width,
        height: a.height,
      })),
    },
    // #43a-rev: top-level font catalogue. Each entry holds the family
    // name + style + (optional) weight + (optional) italic flag for every
    // font referenced by text nodes. `asset_id` is populated only by the
    // drag-drop escape hatch (#43c) for user-supplied TTF bundling — the
    // Figma plugin API does not expose font binaries directly. Runtime
    // (Agent A's #43b) consumes via Skia's SkFontMgr system-font matcher
    // with the bundled OFL set as fallback.
    font_family_assets: ctx.fontFamilyAssets ?? [],
    diagnostics: diagnostics.map(toEnvelopeDiagnostic),
    root,
  };
}

function extOf(mime: string): string {
  switch (mime) {
    case "image/png": return "png";
    case "image/jpeg": return "jpg";
    case "image/gif": return "gif";
    case "image/webp": return "webp";
    case "image/svg+xml": return "svg";
    default: return "bin";
  }
}

function toEnvelopeNode(n: ExtractedFigmaNode): unknown {
  const out: Record<string, unknown> = {
    type: n.type,
    name: n.name,
    figma_node_id: n.figma_node_id,
  };
  if (n.content !== undefined) out.content = n.content;
  if (n.asset_ref) out.asset_ref = n.asset_ref;

  // Phase 3 — emit audio-widget metadata at the IR node root. The C++
  // parser (design_ir_json.cpp::parse_ir_node) reads:
  //   audio_widget  → IRNode.audio_widget enum
  //   label         → IRNode.audio_label
  //   min/max/default → IRNode.audio_min/max/default (float)
  //   attributes.* → IRNode.attributes (free-form passthrough)
  if (n.library_widget_kind) out.audio_widget = n.library_widget_kind;
  if (n.audio_label !== undefined) out.label = n.audio_label;
  if (n.audio_min !== undefined) out.min = n.audio_min;
  if (n.audio_max !== undefined) out.max = n.audio_max;
  if (n.audio_default !== undefined) out.default = n.audio_default;
  if (n.audio_units !== undefined ||
      n.audio_binding !== undefined ||
      n.audio_binding_y !== undefined) {
    const attrs: Record<string, string> = {};
    if (n.audio_units !== undefined) attrs.units = n.audio_units;
    if (n.audio_binding !== undefined) attrs.binding = n.audio_binding;
    // Phase 5: XYPad carries a second-axis binding alongside the primary
    // `binding`. Lands in IRNode.attributes.binding_y; codegen consumes
    // when the audio_widget="xy_pad" branch is wired up to route two
    // parameter targets (see planning/2026-05-30-figma-import-fidelity-coordination.md).
    if (n.audio_binding_y !== undefined) attrs.binding_y = n.audio_binding_y;
    out.attributes = attrs;
  }

  // Style: pass through truthy fields only (envelope schema says additionalProperties:false)
  const styleEntries = Object.entries(n.style).filter(([, v]) => v !== undefined && v !== null && v !== "");
  if (styleEntries.length > 0) out.style = Object.fromEntries(styleEntries);

  // Layout: same
  const layoutEntries = Object.entries(n.layout).filter(([, v]) => v !== undefined && v !== null);
  if (layoutEntries.length > 0) out.layout = Object.fromEntries(layoutEntries);

  // Figma metadata — pack non-essential identity / provenance into the figma sub-object
  const figma: Record<string, unknown> = {
    parent_id: n.parent_id,
    z_order: n.z_order,
    absolute_transform: n.relative_transform,
    visible: n.visible,
    locked: n.locked,
    blend_mode: n.blend_mode,
  };
  if (n.component_key) figma.component_key = n.component_key;
  if (n.component_set_name) figma.component_set_name = n.component_set_name;
  if (n.main_component_id) figma.main_component_id = n.main_component_id;
  if (n.main_component_name) figma.main_component_name = n.main_component_name;
  if (n.library_widget_kind) figma.library_widget_kind = n.library_widget_kind;
  if (n.library_version) figma.library_version = n.library_version;
  if (n.component_properties) figma.component_properties = n.component_properties;
  if (n.variant_properties) figma.variant_properties = n.variant_properties;
  out.figma = figma;

  if (n.children.length > 0) {
    out.children = n.children.map(toEnvelopeNode);
  }
  return out;
}

function toEnvelopeDiagnostic(d: ExtractedDiagnostic): unknown {
  return {
    severity: d.severity,
    code: d.code,
    kind: d.kind,
    message: d.message,
    path: d.path,
  };
}
