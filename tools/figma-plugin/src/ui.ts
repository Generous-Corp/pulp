// Pulp Figma Plugin — UI iframe code.

import { zipSync } from "fflate";
import type {
  AssetBundle,
  FontFamilyAssetSummary,
  PulpFigmaUIMessage,
  PulpSandboxMessage,
} from "./types";

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
    // User-supplied font assets ride alongside image assets in the same bundle
    // list; the zip writer needs to recognize them.
    case "font/ttf": return "ttf";
    case "font/otf": return "otf";
    case "font/woff": return "woff";
    case "font/woff2": return "woff2";
    default: return "bin";
  }
}

// Font drop-zone state. `bundled` reflects sandbox confirmation (we asked for a
// stage and got back `user-font-staged`); the row turns green when this fires.
// Keyed by the same `family|style` tuple the sandbox uses.
const bundledFonts = new Set<string>();
const fontKey = (family: string, style: string): string => `${family}|${style}`;

function renderFonts(fonts: FontFamilyAssetSummary[]): void {
  const list = el("fonts-list");
  while (list.firstChild) list.removeChild(list.firstChild);

  if (fonts.length === 0) {
    const empty = document.createElement("div");
    empty.id = "fonts-empty";
    empty.textContent = "No fonts detected in the current selection.";
    list.appendChild(empty);
    return;
  }

  for (const f of fonts) {
    const key = fontKey(f.family, f.style);
    if (f.has_user_font) bundledFonts.add(key);

    const row = document.createElement("div");
    row.className = "font-row";
    if (bundledFonts.has(key)) row.classList.add("bundled");
    row.dataset.family = f.family;
    row.dataset.style = f.style;

    const left = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = f.family;
    left.appendChild(name);
    const meta = document.createElement("span");
    meta.className = "meta";
    const bits: string[] = [f.style];
    if (typeof f.weight === "number") bits.push(`w${f.weight}`);
    if (f.italic) bits.push("italic");
    meta.textContent = ` · ${bits.join(" · ")}`;
    left.appendChild(meta);
    row.appendChild(left);

    const badge = document.createElement("span");
    badge.className = "badge";
    badge.textContent = bundledFonts.has(key) ? "✓ bundled" : "drop .ttf";
    row.appendChild(badge);

    wireDropZone(row, f);
    list.appendChild(row);
  }
}

function wireDropZone(row: HTMLDivElement, font: FontFamilyAssetSummary): void {
  row.addEventListener("dragover", (e) => {
    e.preventDefault();
    row.classList.add("dragover");
  });
  row.addEventListener("dragleave", () => row.classList.remove("dragover"));
  row.addEventListener("drop", async (e) => {
    e.preventDefault();
    row.classList.remove("dragover");
    const file = e.dataTransfer?.files?.[0];
    if (!file) return;
    const ab = await file.arrayBuffer();
    const bytes = Array.from(new Uint8Array(ab));
    send({
      type: "user-font",
      family: font.family,
      style: font.style,
      bytes,
      filename: file.name,
    });
  });
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
    case "fonts-detected":
      renderFonts(msg.fonts);
      break;
    case "user-font-staged": {
      // Sandbox accepted the bundled font; flip the row state.
      bundledFonts.add(fontKey(msg.family, msg.style));
      const row = el("fonts-list").querySelector<HTMLDivElement>(
        `.font-row[data-family="${cssEscape(msg.family)}"][data-style="${cssEscape(msg.style)}"]`,
      );
      if (row) {
        row.classList.add("bundled");
        const badge = row.querySelector(".badge");
        if (badge) badge.textContent = "✓ bundled";
      }
      break;
    }
    case "error":
      showStatus(`Error: ${msg.message}`);
      break;
  }
};

// CSS.escape may not exist in older WebViews; provide a tiny fallback.
function cssEscape(s: string): string {
  if (typeof CSS !== "undefined" && typeof CSS.escape === "function") return CSS.escape(s);
  return s.replace(/["\\]/g, "\\$&");
}

document.addEventListener("DOMContentLoaded", () => {
  el("btn-refresh").addEventListener("click", () => send({ type: "get-selection-summary" }));
  el("btn-export").addEventListener("click", () => send({ type: "export" }));
  el("btn-close").addEventListener("click", () => send({ type: "close" }));
  el("btn-scan-fonts").addEventListener("click", () => send({ type: "scan-fonts" }));
});
