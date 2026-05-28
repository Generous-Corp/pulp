// Pulp Figma Plugin — sandbox main code (runs in Figma's JS sandbox).
//
// Phase 2a slice 1 scope: extract selection → in-memory model → serialize to
// JSON envelope → post to UI for download. Tokens, assets, component
// recognition all defer to slice 2 / Phase 3.

import { extractScene } from "./extract";
import { serializeExport, type LibraryManifestSnapshot } from "./serialize";
import type { PulpFigmaUIMessage, PulpSandboxMessage } from "./types";

const PLUGIN_VERSION = "0.1.0";

// library-manifest.json is embedded into the bundle by esbuild's import-attribute
// path. For now we hand-author the snapshot here; a follow-up build-step
// improvement reads tools/figma-plugin/library-manifest.json directly.
const LIBRARY_MANIFEST: LibraryManifestSnapshot = {
  library_version: "0.1.0",
  required_plugin_version: ">=0.1.0",
  widget_keys: {
    knob: "TBD-paste-after-library-publish",
  },
};

figma.showUI(__html__, { width: 360, height: 520, themeColors: true });

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

      case "get-selection-summary":
        reply({
          type: "selection-summary",
          count: figma.currentPage.selection.length,
          names: figma.currentPage.selection.map((n) => n.name).slice(0, 8),
          firstTypeIfAny: figma.currentPage.selection[0]?.type ?? null,
        });
        break;

      case "export":
        await handleExport();
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

async function handleExport(): Promise<void> {
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
    message: `Walked ${result.nodeCount} nodes; serializing…`,
  });

  const envelope = serializeExport(result.roots, result.diagnostics, {
    fileKey: figma.fileKey ?? "unknown",
    rootNodeId: sel[0].id,
    pluginVersion: PLUGIN_VERSION,
    libraryManifest: LIBRARY_MANIFEST,
  });

  const json = JSON.stringify(envelope, null, 2);
  const suggestedName = `${sanitizeFilename(sel[0].name) || "pulp-export"}.pulp.json`;

  reply({
    type: "export-result",
    nodeCount: result.nodeCount,
    diagnosticCount: result.diagnostics.length,
    truncated: result.truncated,
    suggestedName,
    json,
  });
}

function sanitizeFilename(s: string): string {
  return s.replace(/[^A-Za-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 64);
}

function reply(msg: PulpSandboxMessage): void {
  figma.ui.postMessage(msg);
}

// Boot — let the UI know we're alive.
reply({ type: "ready", pluginVersion: PLUGIN_VERSION });
