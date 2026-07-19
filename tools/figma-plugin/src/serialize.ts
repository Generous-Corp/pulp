// Serialize an extracted scene into the v1 JSON envelope declared in
// schema/figma-plugin-export-v1.json. This is mostly a passthrough since the
// extractor's in-memory model already mirrors the envelope shape; the
// serializer's job is to add the envelope-level fields (format_version,
// provenance, library_manifest snapshot, asset_manifest, diagnostics) and
// to drop noise.

import type { ExtractedFigmaNode, ExtractedDiagnostic } from "./extract-model";
import type { AssetCache } from "./assets";
import type { ExtractedTokens } from "./tokens";
import type { FontFamilyAsset } from "./extract";
import type { UserFontCache } from "./user-fonts";

export interface SerializeContext {
  fileKey: string;
  rootNodeId: string;
  pluginVersion: string;
  libraryManifest?: LibraryManifestSnapshot;
  assets: AssetCache;
  tokens: ExtractedTokens;
  /// Deduplicated font catalog produced by extractScene.
  /// Empty array → no text nodes in the selection (or no font names
  /// captured). Emitted at envelope root as `font_family_assets`.
  fontFamilyAssets?: FontFamilyAsset[];
  /// User-supplied font bytes captured via the drag-drop UI.
  /// When present, every `font_family_assets` entry whose (family, style)
  /// matches a cache entry gets `asset_id` stamped, and the bytes flow
  /// through the asset_manifest into the `.pulp.zip`. Optional — empty
  /// or undefined cache is a no-op (the envelope still emits the
  /// metadata-only catalog).
  userFonts?: UserFontCache;
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
  // Orphan-font check: if the user dropped a TTF/OTF for
  // (family, style) tuples that don't appear in font_family_assets
  // (drop happened before scan, or selection changed before export),
  // the bytes still ride in asset_manifest but no catalog entry
  // references them. The runtime can find them by hash, but the dead
  // weight + lack of pointer is worth surfacing so the user knows
  // they bundled bytes that won't auto-resolve.
  if (ctx.userFonts && ctx.userFonts.size() > 0) {
    const stampedKeys = new Set<string>();
    for (const f of ctx.fontFamilyAssets ?? []) {
      if (ctx.userFonts.lookup(f.family, f.style)) {
        stampedKeys.add(`${f.family}|${f.style}`);
      }
    }
    for (const userFont of ctx.userFonts.entries()) {
      const key = `${userFont.family}|${userFont.style}`;
      if (!stampedKeys.has(key)) {
        diagnostics.push({
          severity: "info",
          code: "userfont-orphan",
          kind: "fallback_used",
          message: `User-supplied font "${userFont.family} ${userFont.style}" ` +
                   `(${userFont.original_filename}) is bundled in asset_manifest ` +
                   `but no text node in this selection references that (family, style). ` +
                   `Runtime can still locate it by content_hash; consider re-scanning fonts before export.`,
          path: "/font_family_assets",
        });
      }
    }
  }

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
      assets: [
        ...ctx.assets.entries().map((a) => ({
          asset_id: a.asset_id,
          original_uri: a.original_uri,
          original_uri_aliases: a.original_uri_aliases,
          local_path: `assets/${a.content_hash}.${extOf(a.mime)}`,
          content_hash: a.content_hash,
          mime: a.mime,
          width: a.width,
          height: a.height,
        })),
        // User-supplied fonts ride alongside the image / vector
        // assets in the same manifest. The local_path always ends in
        // .ttf / .otf so the zip writer and downstream consumers can
        // pick them out by extension without re-sniffing the mime.
        ...(ctx.userFonts?.entries() ?? []).map((f) => ({
          asset_id: f.asset_id,
          original_uri: `userfont://${encodeURIComponent(f.original_filename)}`,
          original_uri_aliases: [],
          local_path: `assets/${f.content_hash}.${extOfFontMime(f.mime)}`,
          content_hash: f.content_hash,
          mime: f.mime,
        })),
      ],
    },
    // Top-level font catalog. Each entry holds the family
    // name + style + (optional) weight + (optional) italic flag for every
    // font referenced by text nodes. `asset_id` is populated only by the
    // drag-drop escape hatch for user-supplied TTF bundling — the
    // Figma plugin API does not expose font binaries directly. Runtime
    // consumer uses via Skia's SkFontMgr system-font matcher
    // with the bundled OFL set as fallback.
    font_family_assets: stampUserFonts(ctx.fontFamilyAssets ?? [], ctx.userFonts),
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

function extOfFontMime(mime: string): string {
  switch (mime) {
    case "font/ttf":   return "ttf";
    case "font/otf":   return "otf";
    case "font/woff":  return "woff";
    case "font/woff2": return "woff2";
    default: return "ttf";
  }
}

/// For every font_family_assets entry, if the user has
/// supplied a matching (family, style) cache entry, stamp it with
/// the cached asset_id. Pure function; the original list is preserved
/// otherwise so the metadata-only catalog is unchanged
/// when no user fonts are present.
function stampUserFonts(
  fonts: FontFamilyAsset[],
  cache: UserFontCache | undefined,
): FontFamilyAsset[] {
  if (!cache || cache.size() === 0) return fonts;
  return fonts.map((f) => {
    const match = cache.lookup(f.family, f.style);
    if (!match) return f;
    return { ...f, asset_id: match.asset_id };
  });
}

function toEnvelopeNode(n: ExtractedFigmaNode): unknown {
  const out: Record<string, unknown> = {
    type: n.type,
    name: n.name,
    figma_node_id: n.figma_node_id,
  };
  if (n.content !== undefined) out.content = n.content;
  if (n.asset_ref) out.asset_ref = n.asset_ref;

  // Faithful-vector: emit the render-mode + SVG asset + typed
  // interactive overlays the C++ materializer consumes (parse_ir_node already
  // reads these keys). Only present on a faithful_svg node.
  if (n.render_mode) out.render_mode = n.render_mode;
  if (n.svg_asset_id) out.svg_asset_id = n.svg_asset_id;
  if (n.interactive_elements && n.interactive_elements.length > 0) {
    out.interactive_elements = n.interactive_elements;
  }

  // Emit audio-widget metadata at the IR node root. The C++
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
    // XYPad carries a second-axis binding alongside the primary `binding`.
    // Lands in IRNode.attributes.binding_y; codegen consumes it when
    // audio_widget="xy_pad" routes two parameter targets.
    if (n.audio_binding_y !== undefined) attrs.binding_y = n.audio_binding_y;
    out.attributes = attrs;
  }

  // Style: pass through truthy fields only (envelope schema says additionalProperties:false)
  const styleEntries = Object.entries(n.style).filter(([, v]) => v !== undefined && v !== null && v !== "");
  if (styleEntries.length > 0) out.style = Object.fromEntries(styleEntries);

  // Layout: same
  const layoutEntries = Object.entries(n.layout).filter(([, v]) => v !== undefined && v !== null);
  if (layoutEntries.length > 0) out.layout = Object.fromEntries(layoutEntries);

  // Resize constraints at the node root — the shared producer shape all three
  // Figma lanes (.fig decoder, plugin, REST) emit, and the first place
  // design_ir_json.cpp::parse_ir_node looks. Figma spelling passes through
  // untranslated; the importer owns normalization.
  if (n.constraints) out.constraints = n.constraints;

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
  // Only emitted when true: a local component omits the flag entirely, so the
  // consumer treats presence as the remote-library signal.
  if (n.remote_library === true) figma.remote_library = true;
  if (n.library_widget_kind) figma.library_widget_kind = n.library_widget_kind;
  if (n.library_version) figma.library_version = n.library_version;
  if (n.component_properties) figma.component_properties = n.component_properties;
  if (n.variant_properties) figma.variant_properties = n.variant_properties;
  // Variable bindings: {property: canonical token name}. Pairs with the
  // envelope-level `tokens` maps — the binding names an entry there, so a
  // downstream consumer can re-theme by swapping the token's value.
  if (n.bound_variables && Object.keys(n.bound_variables).length > 0) {
    figma.bound_variables = n.bound_variables;
  }
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
