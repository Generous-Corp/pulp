// Hand-authored TYPES FOR PLUGIN ↔ UI MESSAGING.

export type PulpFigmaUIMessage =
  | { type: "ping" }
  | { type: "report-file-key"; fileKey: string | null }
  | { type: "get-selection-summary" }
  | { type: "export" }
  /// User dragged a TTF/OTF onto the plugin UI to satisfy a non-system font.
  /// `bytes` is the raw font file as a number[] (postMessage-safe transit).
  | {
      type: "user-font";
      family: string;
      style: string;
      bytes: number[];
      filename: string;
    }
  /// UI asks the sandbox to scan the current selection's text nodes and report
  /// back the unique font tuples so it can render per-family drop zones.
  | { type: "scan-fonts" }
  | { type: "close" };

export interface AssetBundle {
  content_hash: string;
  mime: string;
  bytes: number[]; // Uint8Array serialized as plain array for postMessage transit
}

export interface FontFamilyAssetSummary {
  family: string;
  style: string;
  weight?: number;
  italic?: boolean;
  /// True when the sandbox already has a user-supplied TTF/OTF cached
  /// for this (family, style) tuple. The UI uses this to render
  /// "✓ bundled" instead of an empty drop zone.
  has_user_font?: boolean;
}

export type PulpSandboxMessage =
  | { type: "ready"; pluginVersion: string }
  | { type: "pong"; figmaVersion: string; editorType: string; documentName: string }
  | { type: "selection-summary"; count: number; names: string[]; firstTypeIfAny: string | null }
  | { type: "progress"; stage: "extracting" | "serializing"; message: string }
  /// Sandbox responds to scan-fonts with the unique (family, style, weight?,
  /// italic?) tuples referenced by the current selection's text nodes, plus a
  /// flag for which are already bundled via a user drop.
  | { type: "fonts-detected"; fonts: FontFamilyAssetSummary[] }
  /// Sandbox acknowledges a user-font upload. The UI uses this to flip the row
  /// to "✓ bundled" state. `asset_id` is the content-hashed handle the envelope
  /// will carry on the matching font_family_assets entry.
  | { type: "user-font-staged"; family: string; style: string; asset_id: string }
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
