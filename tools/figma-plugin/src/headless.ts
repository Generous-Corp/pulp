// Pulp Figma Plugin — HEADLESS extractor entry point.
//
// Same `extractScene` + `serializeExport` core as the published plugin
// (`code.ts`), but without `figma.showUI()` + the UI <-> sandbox message
// loop. Designed to be inlined as a single esbuild IIFE bundle and run
// through Figma MCP's `use_figma` tool by an agent or driver script;
// the returned value IS the envelope plus serialised asset bytes,
// suitable for assembling into a `.pulp.zip` outside the sandbox.
//
// Purpose (#3225-era reframe): the official REST headless path lives at
// `tools/import-design/figma_rest_export.py`. This bundle is the
// *plugin-side* conformance oracle — it produces byte-identical output to
// the published plugin's `Export to Pulp`, so the conformance test in
// `test/test_design_import_sources.cpp` can detect drift between the Python REST
// port and the in-Figma plugin.
//
// Usage from a driver:
//   await mcp.use_figma({
//     fileKey,
//     description: "headless plugin export",
//     code: `const TARGET_NODE_ID = ${JSON.stringify(nodeId)}; ${HEADLESS_BUNDLE}`,
//   });
// The bundle reads the optional `TARGET_NODE_ID` global; if unset it
// falls back to the current page selection. The result is a plain object
// (postMessage-safe — numbers/strings/arrays only, no Uint8Array).

import { extractScene } from "./extract";
import { serializeExport, type LibraryManifestSnapshot } from "./serialize";
import { UserFontCache } from "./user-fonts";

const PLUGIN_VERSION = "0.2.0-headless";

const LIBRARY_MANIFEST: LibraryManifestSnapshot = {
  library_version: "0.3.0",
  required_plugin_version: ">=0.1.0",
  widget_keys: {
    knob: "f74264ffa9108521fb0d3398dc8f5ea88e23a84e",
    fader: "1c2b727f0c0e11026512725aeb546997f16042bd",
    meter: "52e1636086b855cb2d20d341d4cfa15e94151eef",
    xy_pad: "9dc09d4cbf65341f12c21ece408ad653886059b9",
    waveform: "2c0797af5c939638ec6a89d893ba310a088ce46c",
    spectrum: "f6730821fc7557e93f904d171a45339207abf9e3",
  },
};

declare const TARGET_NODE_ID: string | undefined;
// Faithful-vector lane: the default. Each top-level frame exports its own SVG
// and renders via DesignFrameView with auto-detected interactive overlays,
// instead of the legacy widget-recognition rebuild. The injected prelude sets
// FAITHFUL_VECTOR = false to opt out (legacy flat tree).
declare const FAITHFUL_VECTOR: boolean | undefined;

interface HeadlessAssetBundle {
  content_hash: string;
  mime: string;
  bytes: number[];
}

interface HeadlessResult {
  envelope: unknown;
  envelope_json: string;
  assets: HeadlessAssetBundle[];
  node_count: number;
  diagnostic_count: number;
  asset_count: number;
  truncated: boolean;
  suggested_name: string;
  target_node_id: string;
}

// Top-level await is allowed in esbuild IIFE bundles when target is recent
// enough. The headless caller awaits this implicitly because use_figma
// returns the resolved Promise value.
async function run(): Promise<HeadlessResult> {
  // Resolve the selection. Prefer an explicit TARGET_NODE_ID (preferred for
  // agent-driven extraction — deterministic), then fall back to whatever
  // the user has selected in Figma (matches the UI plugin's behaviour).
  let roots: readonly SceneNode[];
  let resolvedId: string;

  if (typeof TARGET_NODE_ID === "string" && TARGET_NODE_ID.length > 0) {
    const node = await figma.getNodeByIdAsync(TARGET_NODE_ID);
    if (!node) {
      throw new Error(`Node ${TARGET_NODE_ID} not found`);
    }
    if (!isSceneNode(node)) {
      throw new Error(`Node ${TARGET_NODE_ID} is type ${node.type}; expected SceneNode`);
    }
    roots = [node];
    resolvedId = node.id;
  } else {
    const sel = figma.currentPage.selection;
    if (sel.length === 0) {
      throw new Error("No TARGET_NODE_ID provided and current page selection is empty.");
    }
    roots = sel;
    resolvedId = sel[0].id;
  }

  const result = await extractScene(roots as readonly SceneNode[], {
    faithfulVector: typeof FAITHFUL_VECTOR === "undefined" ? true : FAITHFUL_VECTOR !== false,
  });

  const fileKey =
    figma.fileKey ??
    `local-${encodeURIComponent(figma.root.name).slice(0, 32)}`;

  // The published plugin supports drag-drop user fonts; the headless path has no
  // UI, so the cache is always empty. We pass an empty cache so the serializer's
  // metadata-only path runs (matches the byte shape of an "Export to Pulp" with
  // no user fonts supplied).
  const userFonts = new UserFontCache();

  const envelope = serializeExport(result.roots, result.diagnostics, {
    fileKey,
    rootNodeId: resolvedId,
    pluginVersion: PLUGIN_VERSION,
    libraryManifest: LIBRARY_MANIFEST,
    assets: result.assets,
    tokens: result.tokens,
    fontFamilyAssets: result.font_family_assets,
    userFonts,
  });

  const envelope_json = JSON.stringify(envelope, null, 2);

  // Same shape as the UI plugin's export-result message — the driver can
  // pack these into the .pulp.zip identically.
  const assetBundles: HeadlessAssetBundle[] = result.assets.entries().map((a) => ({
    content_hash: a.content_hash,
    mime: a.mime,
    bytes: Array.from(a.bytes),
  }));

  const firstName = (roots[0] as { name?: string }).name ?? "pulp-export";
  const suggested_name = sanitizeFilename(firstName) || "pulp-export";

  return {
    envelope,
    envelope_json,
    assets: assetBundles,
    node_count: result.nodeCount,
    diagnostic_count: result.diagnostics.length,
    asset_count: result.assets.size(),
    truncated: result.truncated,
    suggested_name,
    target_node_id: resolvedId,
  };
}

function isSceneNode(n: BaseNode): n is SceneNode {
  return "visible" in n && "absoluteBoundingBox" in n;
}

function sanitizeFilename(s: string): string {
  return s.replace(/[^A-Za-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 64);
}

// esbuild wraps the bundle in an IIFE — the inner `run()` Promise isn't
// addressable from outside the wrapper. Surface it as a side-effect on
// globalThis so the agent-side driver can `return await
// globalThis.__pulp_headless_result;` after evaluating the bundle.
(globalThis as unknown as { __pulp_headless_result?: Promise<HeadlessResult> })
  .__pulp_headless_result = run();
