import type { AssetBundle } from "./types";

export const FORGE_CLIPBOARD_FORMAT = "pulp-figma-clipboard-v1";
export const FORGE_CLIPBOARD_MAX_BYTES = 32 * 1024 * 1024;
export const FORGE_CLIPBOARD_MAX_NODES = 10_000;

export interface ForgeClipboardAsset {
  content_hash: string;
  mime: string;
  base64: string;
}

export interface FigmaToForgeClipboard {
  format: typeof FORGE_CLIPBOARD_FORMAT;
  kind: "figma-plugin-export";
  scene_json: string;
  assets: ForgeClipboardAsset[];
}

export interface ForgeToFigmaClipboard {
  format: typeof FORGE_CLIPBOARD_FORMAT;
  kind: "design-ir";
  design_ir_json: string;
}

export type ForgeClipboardEnvelope =
  | FigmaToForgeClipboard
  | ForgeToFigmaClipboard;

export interface ForgeFigmaPlanNode {
  kind: "frame" | "text" | "ellipse" | "rectangle";
  name: string;
  content?: string;
  x: number;
  y: number;
  width: number;
  height: number;
  backgroundColor?: string;
  textColor?: string;
  fontSize?: number;
  borderColor?: string;
  borderWidth?: number;
  cornerRadius?: number;
  layoutMode?: "HORIZONTAL" | "VERTICAL";
  itemSpacing?: number;
  paddingTop?: number;
  paddingRight?: number;
  paddingBottom?: number;
  paddingLeft?: number;
  audioWidget?: string;
  audioLabel?: string;
  audioMin?: number;
  audioMax?: number;
  audioDefault?: number;
  audioUnits?: string;
  binding?: string;
  bindingY?: string;
  sourceNodeId?: string;
  stableAnchorId?: string;
  children: ForgeFigmaPlanNode[];
}

export interface ForgeFigmaPlan {
  root: ForgeFigmaPlanNode;
  nodeCount: number;
  audioWidgetCount: number;
}

const AUDIO_WIDGETS = new Set([
  "knob",
  "fader",
  "meter",
  "xy_pad",
  "waveform",
  "spectrum",
]);

function bytesToBase64(bytes: number[]): string {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.slice(i, i + chunk));
  }
  return btoa(binary);
}

function utf8ByteLength(text: string): number {
  return new TextEncoder().encode(text).byteLength;
}

export function encodeFigmaSelectionForForge(
  sceneJson: string,
  assets: AssetBundle[],
): string {
  const envelope: FigmaToForgeClipboard = {
    format: FORGE_CLIPBOARD_FORMAT,
    kind: "figma-plugin-export",
    scene_json: sceneJson,
    assets: assets.map((asset) => ({
      content_hash: asset.content_hash,
      mime: asset.mime,
      base64: bytesToBase64(asset.bytes),
    })),
  };
  const encoded = JSON.stringify(envelope);
  const encodedBytes = utf8ByteLength(encoded);
  if (encodedBytes > FORGE_CLIPBOARD_MAX_BYTES) {
    throw new Error(
      `Clipboard payload is ${encodedBytes} bytes; the ${FORGE_CLIPBOARD_MAX_BYTES}-byte limit protects both apps.`,
    );
  }
  return encoded;
}

function finiteNumber(value: unknown, fallback: number): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

function record(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function toPlanNode(
  input: unknown,
  counters: { nodes: number; widgets: number },
): ForgeFigmaPlanNode {
  if (counters.nodes >= FORGE_CLIPBOARD_MAX_NODES) {
    throw new Error(
      `Design exceeds the ${FORGE_CLIPBOARD_MAX_NODES}-node clipboard limit.`,
    );
  }
  counters.nodes++;

  const node = record(input);
  const style = record(node.style);
  const layout = record(node.layout);
  const attrs = record(node.attributes);
  const type = optionalString(node.type) ?? "frame";
  const audioWidget = optionalString(node.audioWidget) ??
    optionalString(node.audio_widget);
  if (audioWidget && !AUDIO_WIDGETS.has(audioWidget)) {
    throw new Error(`Unsupported audio widget "${audioWidget}".`);
  }
  if (audioWidget) counters.widgets++;

  let kind: ForgeFigmaPlanNode["kind"] = "frame";
  if (type === "text") kind = "text";
  else if (type === "ellipse" || type === "circle") kind = "ellipse";
  else if (type === "rectangle" || type === "rect") kind = "rectangle";

  const direction = optionalString(layout.direction);
  const children = Array.isArray(node.children)
    ? node.children.map((child) => toPlanNode(child, counters))
    : [];
  if (children.length > 0 && kind !== "frame") kind = "frame";

  return {
    kind,
    name: optionalString(node.name) ??
      optionalString(node.label) ??
      (audioWidget ? `Pulp / ${audioWidget}` : type),
    content: optionalString(node.content),
    x: finiteNumber(style.left, 0),
    y: finiteNumber(style.top, 0),
    width: Math.max(1, finiteNumber(style.width, kind === "text" ? 120 : 100)),
    height: Math.max(1, finiteNumber(style.height, kind === "text" ? 24 : 100)),
    backgroundColor: optionalString(style.backgroundColor) ??
      optionalString(style.background_color),
    textColor: optionalString(style.color),
    fontSize: typeof style.fontSize === "number"
      ? finiteNumber(style.fontSize, 12)
      : typeof style.font_size === "number"
        ? finiteNumber(style.font_size, 12)
        : undefined,
    borderColor: optionalString(style.borderColor) ??
      optionalString(style.border_color),
    borderWidth: typeof style.borderWidth === "number"
      ? finiteNumber(style.borderWidth, 0)
      : typeof style.border_width === "number"
        ? finiteNumber(style.border_width, 0)
        : undefined,
    cornerRadius: typeof style.borderRadius === "number"
      ? finiteNumber(style.borderRadius, 0)
      : typeof style.border_radius === "number"
        ? finiteNumber(style.border_radius, 0)
        : undefined,
    layoutMode: direction === "row" ? "HORIZONTAL"
      : direction === "column" ? "VERTICAL"
      : undefined,
    itemSpacing: typeof layout.gap === "number"
      ? finiteNumber(layout.gap, 0)
      : undefined,
    paddingTop: typeof layout.paddingTop === "number"
      ? finiteNumber(layout.paddingTop, 0)
      : undefined,
    paddingRight: typeof layout.paddingRight === "number"
      ? finiteNumber(layout.paddingRight, 0)
      : undefined,
    paddingBottom: typeof layout.paddingBottom === "number"
      ? finiteNumber(layout.paddingBottom, 0)
      : undefined,
    paddingLeft: typeof layout.paddingLeft === "number"
      ? finiteNumber(layout.paddingLeft, 0)
      : undefined,
    audioWidget,
    audioLabel: optionalString(node.label),
    audioMin: typeof node.min === "number"
      ? finiteNumber(node.min, 0)
      : undefined,
    audioMax: typeof node.max === "number"
      ? finiteNumber(node.max, 1)
      : undefined,
    audioDefault: typeof node.default === "number"
      ? finiteNumber(node.default, 0)
      : undefined,
    audioUnits: optionalString(attrs.units),
    binding: optionalString(attrs.binding) ??
      optionalString(attrs.pulpParamKey),
    bindingY: optionalString(attrs.binding_y) ??
      optionalString(attrs.pulpParamKeyY),
    sourceNodeId: optionalString(node.source_node_id) ??
      optionalString(node.sourceNodeId),
    stableAnchorId: optionalString(node.stable_anchor_id) ??
      optionalString(node.stableAnchorId),
    children,
  };
}

export function decodeForgeDesignForFigma(text: string): ForgeFigmaPlan {
  if (utf8ByteLength(text) > FORGE_CLIPBOARD_MAX_BYTES) {
    throw new Error(
      `Clipboard payload exceeds the ${FORGE_CLIPBOARD_MAX_BYTES}-byte limit.`,
    );
  }
  const parsed = JSON.parse(text) as Partial<ForgeToFigmaClipboard>;
  if (parsed.format !== FORGE_CLIPBOARD_FORMAT || parsed.kind !== "design-ir") {
    throw new Error(
      `Clipboard does not contain a ${FORGE_CLIPBOARD_FORMAT} DesignIR payload.`,
    );
  }
  if (typeof parsed.design_ir_json !== "string") {
    throw new Error("Clipboard DesignIR payload is missing design_ir_json.");
  }
  const design = JSON.parse(parsed.design_ir_json) as Record<string, unknown>;
  if (!design.root) throw new Error("Clipboard DesignIR document has no root.");
  const counters = { nodes: 0, widgets: 0 };
  const root = toPlanNode(design.root, counters);
  return {
    root,
    nodeCount: counters.nodes,
    audioWidgetCount: counters.widgets,
  };
}
