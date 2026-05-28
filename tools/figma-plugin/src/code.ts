// Pulp Figma Plugin — sandbox main code (runs in Figma's JS sandbox).
//
// The plugin has two halves:
//   - src/code.ts (this file): the SANDBOX HALF — has figma.* APIs, no DOM
//   - src/ui.ts:               the IFRAME HALF — has DOM + fetch, no scene access
// They communicate via figma.ui.postMessage / window.parent.postMessage.
//
// Phase 1 scope (this slice): scaffold only — show the UI, echo the current
// selection name back, no extraction yet. Phase 2a adds the real walker.

import type { PulpFigmaUIMessage, PulpSandboxMessage } from "./types";

figma.showUI(__html__, { width: 360, height: 480, themeColors: true });

figma.ui.onmessage = (msg: PulpFigmaUIMessage) => {
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
    case "close":
      figma.closePlugin();
      break;
    default:
      reply({ type: "error", message: `Unknown UI message: ${(msg as any).type}` });
  }
};

function reply(msg: PulpSandboxMessage): void {
  figma.ui.postMessage(msg);
}

// Boot — let the UI know we're alive.
reply({ type: "ready", pluginVersion: "0.1.0" });
