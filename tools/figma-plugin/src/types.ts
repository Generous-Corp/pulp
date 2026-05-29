// Hand-authored TYPES FOR PLUGIN ↔ UI MESSAGING.

export type PulpFigmaUIMessage =
  | { type: "ping" }
  | { type: "report-file-key"; fileKey: string | null }
  | { type: "get-selection-summary" }
  | { type: "export" }
  | { type: "close" };

export interface AssetBundle {
  content_hash: string;
  mime: string;
  bytes: number[]; // Uint8Array serialized as plain array for postMessage transit
}

export type PulpSandboxMessage =
  | { type: "ready"; pluginVersion: string }
  | { type: "pong"; figmaVersion: string; editorType: string; documentName: string }
  | { type: "selection-summary"; count: number; names: string[]; firstTypeIfAny: string | null }
  | { type: "progress"; stage: "extracting" | "serializing"; message: string }
  | {
      type: "export-result";
      nodeCount: number;
      diagnosticCount: number;
      assetCount: number;
      tokenCount: number;
      truncated: boolean;
      suggestedName: string;
      json: string;
      assets: AssetBundle[];
    }
  | { type: "error"; message: string };
