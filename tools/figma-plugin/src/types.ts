// Hand-authored TYPES FOR PLUGIN ↔ UI MESSAGING.
// The EXPORT ENVELOPE types live in types.generated.ts (auto-generated from
// schema/figma-plugin-export-v1.json — see scripts/gen-types.mjs).

export type PulpFigmaUIMessage =
  | { type: "ping" }
  | { type: "get-selection-summary" }
  | { type: "close" };

export type PulpSandboxMessage =
  | { type: "ready"; pluginVersion: string }
  | { type: "pong"; figmaVersion: string; editorType: string; documentName: string }
  | { type: "selection-summary"; count: number; names: string[]; firstTypeIfAny: string | null }
  | { type: "error"; message: string };
