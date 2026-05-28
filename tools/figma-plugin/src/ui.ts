// Pulp Figma Plugin — UI iframe code.
// Has DOM access. NO figma.* APIs. Talks to the sandbox via window.parent.postMessage.

import type { PulpFigmaUIMessage, PulpSandboxMessage } from "./types";

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

window.onmessage = (e: MessageEvent) => {
  const msg = e.data?.pluginMessage as PulpSandboxMessage | undefined;
  if (!msg) return;
  switch (msg.type) {
    case "ready":
      showStatus(`Plugin v${msg.pluginVersion} ready`);
      send({ type: "ping" });
      break;
    case "pong":
      showInfo(
        `Figma API ${msg.figmaVersion} · editor=${msg.editorType} · doc="${msg.documentName}"`,
      );
      break;
    case "selection-summary":
      if (msg.count === 0) {
        showStatus("Select a frame in Figma to export it.");
      } else {
        showStatus(`${msg.count} node(s) selected. First: "${msg.names[0] ?? ""}" (${msg.firstTypeIfAny ?? "?"})`);
      }
      break;
    case "error":
      showStatus(`Error: ${msg.message}`);
      break;
  }
};

document.addEventListener("DOMContentLoaded", () => {
  el("btn-refresh").addEventListener("click", () => {
    send({ type: "get-selection-summary" });
  });
  el("btn-close").addEventListener("click", () => {
    send({ type: "close" });
  });
});
