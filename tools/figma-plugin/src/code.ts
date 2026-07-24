// Pulp Figma Plugin — sandbox main code (runs in Figma's JS sandbox).

import { extractScene } from "./extract";
import { serializeExport, type LibraryManifestSnapshot } from "./serialize";
import type { FontFamilyAssetSummary, PulpFigmaUIMessage, PulpSandboxMessage } from "./types";
import { UserFontCache } from "./user-fonts";
import {
  decodeForgeDesignForFigma,
  type ForgeFigmaPlanNode,
} from "./forge-roundtrip";

const PLUGIN_VERSION = "0.2.0";

const LIBRARY_MANIFEST: LibraryManifestSnapshot = {
  library_version: "0.1.0",
  required_plugin_version: ">=0.1.0",
  widget_keys: {
    knob: "TBD-paste-after-library-publish",
  },
};

let knownFileKey: string | null = null;

/// Sandbox-side store of user-dropped TTF/OTF bytes, keyed by (family, style).
/// Survives across export attempts within a single plugin session so the user
/// can drop, preview, export, drop more, re-export. Discarded when the plugin
/// closes.
const userFonts = new UserFontCache();

figma.showUI(__html__, { width: 360, height: 540, themeColors: true });

figma.ui.onmessage = async (msg: PulpFigmaUIMessage) => {
  try {
    switch (msg.type) {
      case "ping":
        reply({
          type: "pong",
          figmaVersion: figma.apiVersion,
          editorType: figma.editorType,
          documentName: figma.root.name,
        });
        break;

      case "report-file-key":
        if (msg.fileKey) knownFileKey = msg.fileKey;
        break;

      case "get-selection-summary":
        reply({
          type: "selection-summary",
          count: figma.currentPage.selection.length,
          names: figma.currentPage.selection.map((n) => n.name).slice(0, 8),
          firstTypeIfAny: figma.currentPage.selection[0]?.type ?? null,
        });
        break;

      case "export":
        await handleExport("download");
        break;

      case "copy-selection-for-forge":
        await handleExport("clipboard");
        break;

      case "import-forge-design":
        await handleForgeImport(msg.clipboardText);
        break;

      case "scan-fonts":
        await handleScanFonts();
        break;

      case "user-font":
        await handleUserFont(msg);
        break;

      case "close":
        figma.closePlugin();
        break;

      default:
        reply({ type: "error", message: `Unknown UI message: ${(msg as { type: string }).type}` });
    }
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    reply({ type: "error", message });
    console.error("[pulp-figma-plugin]", err);
  }
};

async function handleExport(delivery: "download" | "clipboard"): Promise<void> {
  const sel = figma.currentPage.selection;
  if (sel.length === 0) {
    reply({ type: "error", message: "Select at least one frame in Figma before exporting." });
    return;
  }

  reply({ type: "progress", stage: "extracting", message: `Walking ${sel.length} root(s)…` });
  const result = await extractScene(sel);

  reply({
    type: "progress",
    stage: "serializing",
    message: `Walked ${result.nodeCount} nodes, ${result.assets.size()} asset(s); serializing…`,
  });

  // Resolve file key: published plugins get figma.fileKey; local dev installs
  // get null. The UI iframe parses window.location and posts back via
  // 'report-file-key'. Fall back to the document name encoded as a URN.
  const fileKey =
    figma.fileKey ??
    knownFileKey ??
    `local-${encodeURIComponent(figma.root.name).slice(0, 32)}`;

  const envelope = serializeExport(result.roots, result.diagnostics, {
    fileKey,
    rootNodeId: sel[0].id,
    pluginVersion: PLUGIN_VERSION,
    libraryManifest: LIBRARY_MANIFEST,
    assets: result.assets,
    tokens: result.tokens,
    fontFamilyAssets: result.font_family_assets,
    userFonts,
  });

  const json = JSON.stringify(envelope, null, 2);
  const suggestedName = `${sanitizeFilename(sel[0].name) || "pulp-export"}`;

  // Hand the assets to the UI as { content_hash, mime, bytes } records.
  // Bytes are transferred as plain arrays to keep postMessage compatibility;
  // the UI converts back to Uint8Array for the zip writer. The user-font
  // entries ride in the same bundle list — the UI doesn't need to distinguish
  // them; the zip writer picks the file extension from mime.
  const assetBundles = [
    ...result.assets.entries().map((a) => ({
      content_hash: a.content_hash,
      mime: a.mime,
      bytes: Array.from(a.bytes), // postMessage-safe; ~1.5x size overhead vs raw buffer
    })),
    ...userFonts.entries().map((f) => ({
      content_hash: f.content_hash,
      mime: f.mime,
      bytes: Array.from(f.bytes),
    })),
  ];

  const totalAssetCount = result.assets.size() + userFonts.size();

  reply({
    type: "export-result",
    nodeCount: result.nodeCount,
    diagnosticCount: result.diagnostics.length,
    assetCount: totalAssetCount,
    tokenCount:
      Object.keys(result.tokens.colors).length +
      Object.keys(result.tokens.dimensions).length +
      Object.keys(result.tokens.strings).length,
    truncated: result.truncated,
    suggestedName,
    json,
    assets: assetBundles,
    delivery,
  });
}

function parseHexColor(value: string | undefined): RGB | null {
  if (!value) return null;
  const match = /^#([0-9a-f]{6})$/i.exec(value.trim());
  if (!match) return null;
  const raw = match[1];
  return {
    r: parseInt(raw.slice(0, 2), 16) / 255,
    g: parseInt(raw.slice(2, 4), 16) / 255,
    b: parseInt(raw.slice(4, 6), 16) / 255,
  };
}

function applyRoundtripMetadata(node: SceneNode, plan: ForgeFigmaPlanNode): void {
  if (!("setPluginData" in node)) return;
  if (plan.audioWidget) node.setPluginData("pulp.audio_widget", plan.audioWidget);
  if (plan.audioLabel) node.setPluginData("pulp.audio_label", plan.audioLabel);
  if (plan.audioMin !== undefined)
    node.setPluginData("pulp.audio_min", String(plan.audioMin));
  if (plan.audioMax !== undefined)
    node.setPluginData("pulp.audio_max", String(plan.audioMax));
  if (plan.audioDefault !== undefined)
    node.setPluginData("pulp.audio_default", String(plan.audioDefault));
  if (plan.audioUnits) node.setPluginData("pulp.audio_units", plan.audioUnits);
  if (plan.binding) node.setPluginData("pulp.binding", plan.binding);
  if (plan.bindingY) node.setPluginData("pulp.binding_y", plan.bindingY);
  if (plan.sourceNodeId) node.setPluginData("pulp.source_node_id", plan.sourceNodeId);
  if (plan.stableAnchorId) node.setPluginData("pulp.stable_anchor_id", plan.stableAnchorId);
}

async function materializeForgeNode(
  plan: ForgeFigmaPlanNode,
  parent: ChildrenMixin,
): Promise<SceneNode> {
  let node: SceneNode;
  if (plan.kind === "text") {
    await figma.loadFontAsync({ family: "Inter", style: "Regular" });
    const text = figma.createText();
    text.fontName = { family: "Inter", style: "Regular" };
    text.characters = plan.content ?? plan.name;
    if (plan.fontSize) text.fontSize = plan.fontSize;
    const color = parseHexColor(plan.textColor);
    if (color) text.fills = [{ type: "SOLID", color }];
    node = text;
  } else if (plan.kind === "ellipse") {
    node = figma.createEllipse();
  } else if (plan.kind === "rectangle") {
    node = figma.createRectangle();
  } else {
    const frame = figma.createFrame();
    if (plan.layoutMode) {
      frame.layoutMode = plan.layoutMode;
      frame.itemSpacing = plan.itemSpacing ?? 0;
      frame.paddingTop = plan.paddingTop ?? 0;
      frame.paddingRight = plan.paddingRight ?? 0;
      frame.paddingBottom = plan.paddingBottom ?? 0;
      frame.paddingLeft = plan.paddingLeft ?? 0;
    }
    node = frame;
  }

  node.name = plan.name;
  parent.appendChild(node);
  if ("resize" in node) node.resize(plan.width, plan.height);
  const parentLaysOutChildren =
    "layoutMode" in parent && parent.layoutMode !== "NONE";
  if (!parentLaysOutChildren) {
    node.x = plan.x;
    node.y = plan.y;
  }

  if ("fills" in node && plan.kind !== "text") {
    const fill = parseHexColor(plan.backgroundColor);
    node.fills = fill ? [{ type: "SOLID", color: fill }] : [];
  }
  if ("strokes" in node) {
    const stroke = parseHexColor(plan.borderColor);
    if (stroke && (plan.borderWidth ?? 0) > 0) {
      node.strokes = [{ type: "SOLID", color: stroke }];
      node.strokeWeight = plan.borderWidth ?? 1;
    }
  }
  if ("cornerRadius" in node && plan.cornerRadius !== undefined) {
    node.cornerRadius = plan.cornerRadius;
  }
  applyRoundtripMetadata(node, plan);

  if ("appendChild" in node) {
    for (const child of plan.children) {
      await materializeForgeNode(child, node as FrameNode);
    }
  }
  return node;
}

async function handleForgeImport(clipboardText: string): Promise<void> {
  const plan = decodeForgeDesignForFigma(clipboardText);
  const root = await materializeForgeNode(plan.root, figma.currentPage);
  figma.currentPage.selection = [root];
  figma.viewport.scrollAndZoomIntoView([root]);
  reply({
    type: "forge-import-result",
    nodeCount: plan.nodeCount,
    audioWidgetCount: plan.audioWidgetCount,
  });
}

/// Walk the current selection, collect the unique (family, style, weight?,
/// italic?) tuples referenced by text nodes, and reply with a `fonts-detected`
/// message annotated with which entries already have user-supplied bytes in the
/// cache. Invoked early so the UI can render drop zones before the user commits
/// to a full export.
async function handleScanFonts(): Promise<void> {
  const sel = figma.currentPage.selection;
  if (sel.length === 0) {
    reply({ type: "fonts-detected", fonts: [] });
    return;
  }
  const result = await extractScene(sel);
  const fonts: FontFamilyAssetSummary[] = result.font_family_assets.map((f) => ({
    family: f.family,
    style: f.style,
    weight: f.weight,
    italic: f.italic,
    has_user_font: !!userFonts.lookup(f.family, f.style),
  }));
  reply({ type: "fonts-detected", fonts });
}

/// User dropped a TTF/OTF onto the UI. Store it in the cache and ack with the
/// asset_id so the UI can flip the row state. The cache survives until plugin
/// close.
async function handleUserFont(msg: {
  family: string;
  style: string;
  bytes: number[];
  filename: string;
}): Promise<void> {
  const bytes = new Uint8Array(msg.bytes);
  const entry = await userFonts.add(msg.family, msg.style, bytes, msg.filename);
  reply({
    type: "user-font-staged",
    family: entry.family,
    style: entry.style,
    asset_id: entry.asset_id,
  });
}

function sanitizeFilename(s: string): string {
  return s.replace(/[^A-Za-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 64);
}

function reply(msg: PulpSandboxMessage): void {
  figma.ui.postMessage(msg);
}

reply({ type: "ready", pluginVersion: PLUGIN_VERSION });
