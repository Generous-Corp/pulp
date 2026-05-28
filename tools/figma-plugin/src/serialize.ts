// Phase 2b — serialize an extracted scene into the v1 JSON envelope declared in
// schema/figma-plugin-export-v1.json. This is mostly a passthrough since the
// extractor's in-memory model already mirrors the envelope shape; the
// serializer's job is to add the envelope-level fields (format_version,
// provenance, library_manifest snapshot, asset_manifest, diagnostics) and
// to drop noise.

import type { ExtractedFigmaNode, ExtractedDiagnostic } from "./extract-model";

export interface SerializeContext {
  fileKey: string;
  rootNodeId: string;
  pluginVersion: string;
  libraryManifest?: LibraryManifestSnapshot;
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
    tokens: { colors: {}, dimensions: {}, strings: {} }, // slice 2 wires real variables
    asset_manifest: { version: 1, assets: [] },           // slice 2 wires real assets
    diagnostics: diagnostics.map(toEnvelopeDiagnostic),
    root,
  };
}

function toEnvelopeNode(n: ExtractedFigmaNode): unknown {
  const out: Record<string, unknown> = {
    type: n.type,
    name: n.name,
    figma_node_id: n.figma_node_id,
  };
  if (n.content !== undefined) out.content = n.content;

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
