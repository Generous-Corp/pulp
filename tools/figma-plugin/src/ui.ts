// Pulp Figma Plugin — UI iframe code.

import { zipSync } from "fflate";
import type { AssetBundle, PulpFigmaUIMessage, PulpSandboxMessage } from "./types";

const el = (id: string): HTMLElement => {
  const e = document.getElementById(id);
  if (!e) throw new Error(`element #${id} not found`);
  return e;
};

function send(msg: PulpFigmaUIMessage): void {
  parent.postMessage({ pluginMessage: msg }, "*");
}

function showStatus(text: string): void {
  el("status").textContent = text;
}

function showInfo(text: string): void {
  el("info").textContent = text;
}

function detectFileKey(): string | null {
  // The plugin iframe lives at a sandboxed origin; parent.location is cross-origin.
  // But document.referrer is set to the parent Figma URL (file URL) when allowed.
  // Try both; null on failure.
  try {
    const ref = document.referrer || "";
    const m = ref.match(/figma\.com\/(?:design|file)\/([A-Za-z0-9]+)/);
    if (m) return m[1];
  } catch {
    /* ignore */
  }
  return null;
}

function downloadBlob(name: string, blob: Blob): void {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  setTimeout(() => {
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }, 0);
}

function buildZip(json: string, assets: AssetBundle[], extOfMime: (m: string) => string): Uint8Array {
  const entries: Record<string, Uint8Array> = {};
  entries["scene.pulp.json"] = new TextEncoder().encode(json);
  for (const a of assets) {
    const path = `assets/${a.content_hash}.${extOfMime(a.mime)}`;
    entries[path] = new Uint8Array(a.bytes);
  }
  return zipSync(entries);
}

function extOfMime(m: string): string {
  switch (m) {
    case "image/png": return "png";
    case "image/jpeg": return "jpg";
    case "image/gif": return "gif";
    case "image/webp": return "webp";
    case "image/svg+xml": return "svg";
    default: return "bin";
  }
}

window.onmessage = (e: MessageEvent) => {
  const msg = e.data?.pluginMessage as PulpSandboxMessage | undefined;
  if (!msg) return;
  switch (msg.type) {
    case "ready":
      showStatus(`Plugin v${msg.pluginVersion} ready`);
      send({ type: "report-file-key", fileKey: detectFileKey() });
      send({ type: "ping" });
      send({ type: "get-selection-summary" });
      break;
    case "pong":
      showInfo(`Figma API ${msg.figmaVersion} · editor=${msg.editorType} · doc="${msg.documentName}"`);
      break;
    case "selection-summary":
      if (msg.count === 0) {
        showStatus("Select a frame in Figma to export it.");
      } else {
        showStatus(`${msg.count} node(s) selected. First: "${msg.names[0] ?? ""}" (${msg.firstTypeIfAny ?? "?"})`);
      }
      break;
    case "progress":
      showStatus(msg.message);
      break;
    case "export-result": {
      const summary =
        `Exported ${msg.nodeCount} nodes · ${msg.assetCount} asset(s) · ${msg.tokenCount} token(s)` +
        (msg.diagnosticCount > 0 ? ` · ${msg.diagnosticCount} diagnostic(s)` : "") +
        (msg.truncated ? " · truncated" : "");
      showStatus(summary);

      if (msg.assetCount > 0) {
        const zipped = buildZip(msg.json, msg.assets, extOfMime);
        // Cast through ArrayBuffer slice for Blob compatibility on strict TS5+
        const zipBlob = new Blob([zipped.buffer.slice(zipped.byteOffset, zipped.byteOffset + zipped.byteLength) as ArrayBuffer], { type: "application/zip" });
        downloadBlob(`${msg.suggestedName}.pulp.zip`, zipBlob);
      } else {
        downloadBlob(`${msg.suggestedName}.pulp.json`, new Blob([msg.json], { type: "application/json" }));
      }
      break;
    }
    case "error":
      showStatus(`Error: ${msg.message}`);
      break;
  }
};

document.addEventListener("DOMContentLoaded", () => {
  el("btn-refresh").addEventListener("click", () => send({ type: "get-selection-summary" }));
  el("btn-export").addEventListener("click", () => send({ type: "export" }));
  el("btn-close").addEventListener("click", () => send({ type: "close" }));
});
