import type { AssetBundle } from "./types";
import type { InteractiveElement } from "./faithful-vector";

export const FORGE_CLIPBOARD_FORMAT = "pulp-figma-clipboard-v1";
export const FORGE_CLIPBOARD_MAX_BYTES = 32 * 1024 * 1024;
// Figma caps each plugin-data value at 100 kB. Faithful semantic children are
// retained in one value, so reject a larger subtree during pure decode rather
// than discovering the unsupported payload after scene mutation has begun.
export const FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES = 100_000;
const FIGMA_PLUGIN_ID = "1642456870947996392";
const FIGMA_PLUGIN_DATA_ENTRY_OVERHEAD_BYTES = 64;
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
  assets?: ForgeClipboardAsset[];
}

export type ForgeClipboardEnvelope =
  | FigmaToForgeClipboard
  | ForgeToFigmaClipboard;

export interface ForgePlanColor {
  r: number;
  g: number;
  b: number;
  a: number;
}

export interface ForgePlanGradientStop {
  position: number;
  color: ForgePlanColor;
}

export interface ForgePlanGradient {
  kind: "linear";
  angleDegrees: number;
  stops: ForgePlanGradientStop[];
  css: string;
}

export function gradientTransformForFigma(
  angleDegrees: number,
  width: number,
  height: number,
): [[number, number, number], [number, number, number]] {
  const radians = (angleDegrees - 90) * Math.PI / 180;
  const w = Math.max(width, 1);
  const h = Math.max(height, 1);
  const dx = Math.cos(radians);
  const dy = Math.sin(radians);
  const halfExtent = (Math.abs(dx) * w + Math.abs(dy) * h) / 2;
  // Construct the gradient-to-box transform from the CSS line endpoints and
  // then invert it: Figma stores box-to-gradient coordinates. Keeping the
  // projected line extent is what makes 0/100% meet opposite corners for
  // diagonal gradients, including non-square boxes.
  const vx = dx * 2 * halfExtent / w;
  const vy = dy * 2 * halfExtent / h;
  const wx = -dy * 2 * halfExtent / w;
  const wy = dx * 2 * halfExtent / h;
  const tx = 0.5 - 0.5 * vx - 0.5 * wx;
  const ty = 0.5 - 0.5 * vy - 0.5 * wy;
  const determinant = vx * wy - wx * vy;
  const a = wy / determinant;
  const b = -wx / determinant;
  const c = -vy / determinant;
  const d = vx / determinant;
  return [
    [a, b, -a * tx - b * ty],
    [c, d, -c * tx - d * ty],
  ];
}

export interface ForgePlanEffect {
  kind: "drop-shadow" | "inner-shadow" | "layer-blur" | "background-blur";
  color?: ForgePlanColor;
  offsetX?: number;
  offsetY?: number;
  radius: number;
  spread?: number;
}

export interface ForgePlanAsset {
  assetId: string;
  contentHash: string;
  mime: string;
  bytes: number[];
  originalUri?: string;
  originalUriAliases?: string[];
}

export interface ForgePlanTextRun {
  start: number;
  end: number;
  fontSize?: number;
  fontWeight?: number;
  fontStyle?: string;
  color?: ForgePlanColor;
  letterSpacing?: number;
  textDecoration?: "NONE" | "UNDERLINE" | "STRIKETHROUGH";
}

export interface ForgeFigmaPlanNode {
  kind: "frame" | "text" | "ellipse" | "rectangle" | "image" | "svg";
  svgVisualMode?: "faithful" | "inline" | "asset";
  sourceType: string;
  attributes: Record<string, string>;
  name: string;
  content?: string;
  x: number;
  y: number;
  width: number;
  height: number;
  backgroundColor?: ForgePlanColor;
  backgroundGradient?: ForgePlanGradient;
  asset?: ForgePlanAsset;
  assetRole?: "node" | "background";
  imageScaleMode?: "FILL" | "FIT" | "TILE";
  textColor?: ForgePlanColor;
  fontFamily?: string;
  fontStyle?: string;
  fontSize?: number;
  fontWeight?: number;
  textAlignHorizontal?: "LEFT" | "CENTER" | "RIGHT" | "JUSTIFIED";
  textAlignVertical?: "TOP" | "CENTER" | "BOTTOM";
  letterSpacing?: number;
  lineHeight?: number;
  textCase?: "UPPER" | "LOWER" | "TITLE";
  textDecoration?: "UNDERLINE" | "STRIKETHROUGH";
  textRuns: ForgePlanTextRun[];
  opacity?: number;
  blendMode?: string;
  effects: ForgePlanEffect[];
  clipsContent?: boolean;
  borderColor?: ForgePlanColor;
  borderWidth?: number;
  dashPattern?: number[];
  borderTopWidth?: number;
  borderRightWidth?: number;
  borderBottomWidth?: number;
  borderLeftWidth?: number;
  cornerRadius?: number;
  topLeftRadius?: number;
  topRightRadius?: number;
  bottomRightRadius?: number;
  bottomLeftRadius?: number;
  layoutMode?: "HORIZONTAL" | "VERTICAL";
  itemSpacing?: number;
  counterAxisSpacing?: number;
  paddingTop?: number;
  paddingRight?: number;
  paddingBottom?: number;
  paddingLeft?: number;
  primaryAxisAlignItems?: "MIN" | "MAX" | "CENTER" | "SPACE_BETWEEN";
  counterAxisAlignItems?: "MIN" | "MAX" | "CENTER";
  counterAxisStretch?: boolean;
  counterAxisAlignContent?: "AUTO" | "SPACE_BETWEEN";
  layoutWrap?: "NO_WRAP" | "WRAP";
  layoutSizingHorizontal?: "FIXED" | "HUG" | "FILL";
  layoutSizingVertical?: "FIXED" | "HUG" | "FILL";
  layoutGrow?: number;
  layoutAlign?: "INHERIT" | "STRETCH" | "MIN" | "MAX" | "CENTER";
  layoutPositioning?: "ABSOLUTE";
  minWidth?: number;
  minHeight?: number;
  maxWidth?: number;
  maxHeight?: number;
  aspectRatio?: number;
  constraints?: {
    horizontal: "MIN" | "CENTER" | "MAX" | "STRETCH" | "SCALE";
    vertical: "MIN" | "CENTER" | "MAX" | "STRETCH" | "SCALE";
  };
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
  interactiveElements?: InteractiveElement[];
  children: ForgeFigmaPlanNode[];
  preservedChildren?: unknown[];
  alternateFrames: ForgeFigmaPlanNode[];
}

export interface ForgeFigmaPlan {
  root: ForgeFigmaPlanNode;
  nodeCount: number;
  audioWidgetCount: number;
  assetCount: number;
  bundledFonts: Array<{
    family: string;
    style: string;
    bytes: number[];
    filename: string;
  }>;
}

interface ForgeFontCatalogEntry {
  family: string;
  style: string;
  weight?: number;
  italic?: boolean;
  assetId?: string;
}

const AUDIO_WIDGETS = new Set([
  "knob",
  "fader",
  "meter",
  "xy_pad",
  "waveform",
  "spectrum",
]);
const INTERACTIVE_KINDS = new Set([
  "knob", "fader", "toggle", "dropdown", "text_field", "tab_group",
  "stepper", "swap", "action", "xy_pad", "value_label", "custom",
]);

const FRAME_TYPES = new Set([
  "frame", "group", "container", "panel", "row", "col", "view",
  "button", "input", "slider",
  "knob", "fader", "meter", "xy_pad", "waveform", "spectrum",
]);

const SUPPORTED_STYLE_KEYS = new Set([
  "backgroundColor", "background_color", "backgroundGradient",
  "background_gradient", "backgroundImage", "background_image",
  "backgroundRepeat", "background_repeat", "backgroundSize",
  "background_size", "objectFit", "object_fit", "color", "opacity",
  "mixBlendMode", "mix_blend_mode", "borderRadius", "border_radius",
  "border", "borderColor", "border_color", "borderWidth", "border_width",
  "borderStyle", "border_style",
  "borderTopWidth", "border_top_width", "borderRightWidth",
  "border_right_width", "borderBottomWidth", "border_bottom_width",
  "borderLeftWidth", "border_left_width", "borderTopLeftRadius",
  "borderTopColor", "border_top_color", "borderRightColor",
  "border_right_color", "borderBottomColor", "border_bottom_color",
  "borderLeftColor", "border_left_color",
  "border_top_left_radius", "borderTopRightRadius",
  "border_top_right_radius", "borderBottomRightRadius",
  "border_bottom_right_radius", "borderBottomLeftRadius",
  "border_bottom_left_radius", "boxShadow", "box_shadow", "filter",
  "backdropFilter", "backdrop_filter", "fontFamily", "font_family",
  "fontSize", "font_size", "fontWeight", "font_weight", "fontStyle",
  "font_style", "textAlign", "text_align", "verticalAlign",
  "vertical_align", "letterSpacing", "letter_spacing", "lineHeight",
  "line_height", "textTransform", "text_transform", "textDecoration",
  "text_decoration", "overflow", "position", "top", "left", "width", "height",
  "renderBounds", "render_bounds",
  "minWidth", "min_width", "minHeight", "min_height", "maxWidth",
  "max_width", "maxHeight", "max_height",
]);

const SUPPORTED_LAYOUT_KEYS = new Set([
  "display", "direction", "gap", "rowGap", "row_gap", "columnGap",
  "column_gap", "paddingTop", "padding_top", "paddingRight",
  "padding_right", "paddingBottom", "padding_bottom", "paddingLeft",
  "padding_left", "padding", "justify", "align", "alignSelf", "align_self",
  "alignContent", "align_content", "wrap", "flexGrow", "flex_grow",
  "widthMode", "width_mode", "heightMode", "height_mode", "constraints",
  "aspectRatio",
]);

function bytesToBase64(bytes: number[]): string {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.slice(i, i + chunk));
  }
  return btoa(binary);
}

function base64ToBytes(text: string, path: string): number[] {
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(text) || text.length % 4 === 1) {
    throw new Error(`${path}: invalid base64 asset bytes.`);
  }
  try {
    const binary = atob(text);
    return Array.from(binary, (c) => c.charCodeAt(0));
  } catch {
    throw new Error(`${path}: invalid base64 asset bytes.`);
  }
}

function sha256Hex(bytes: number[]): string {
  const rotr = (value: number, shift: number) =>
    (value >>> shift) | (value << (32 - shift));
  const constants = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
    0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
    0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];
  const data = bytes.slice();
  const bitLength = data.length * 8;
  data.push(0x80);
  while (data.length % 64 !== 56) data.push(0);
  const high = Math.floor(bitLength / 0x100000000);
  const low = bitLength >>> 0;
  for (let shift = 24; shift >= 0; shift -= 8) data.push((high >>> shift) & 0xff);
  for (let shift = 24; shift >= 0; shift -= 8) data.push((low >>> shift) & 0xff);

  const state = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ];
  const words = new Array<number>(64);
  for (let offset = 0; offset < data.length; offset += 64) {
    for (let i = 0; i < 16; i++) {
      const at = offset + i * 4;
      words[i] = ((data[at] << 24) | (data[at + 1] << 16) |
        (data[at + 2] << 8) | data[at + 3]) >>> 0;
    }
    for (let i = 16; i < 64; i++) {
      const s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >>> 3);
      const s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >>> 10);
      words[i] = (words[i - 16] + s0 + words[i - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, h] = state;
    for (let i = 0; i < 64; i++) {
      const sigma1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const choose = (e & f) ^ (~e & g);
      const t1 = (h + sigma1 + choose + constants[i] + words[i]) >>> 0;
      const sigma0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (sigma0 + majority) >>> 0;
      h = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    state[0] = (state[0] + a) >>> 0;
    state[1] = (state[1] + b) >>> 0;
    state[2] = (state[2] + c) >>> 0;
    state[3] = (state[3] + d) >>> 0;
    state[4] = (state[4] + e) >>> 0;
    state[5] = (state[5] + f) >>> 0;
    state[6] = (state[6] + g) >>> 0;
    state[7] = (state[7] + h) >>> 0;
  }
  return state.map((word) => word.toString(16).padStart(8, "0")).join("");
}

function fnv1a64Hex(bytes: number[]): string {
  let low = 0xe7c8b3a1 | 0;
  let high = 0xcbf29ce4 | 0;
  for (const byte of bytes) {
    low ^= byte;
    const nextLow = Math.imul(low, 0x000001b3);
    const nextHigh =
      (Math.imul(high, 0x000001b3) + Math.imul(low, 0x00000001)) | 0;
    low = nextLow | 0;
    high = nextHigh;
  }
  const hex = (value: number) =>
    (value >>> 0).toString(16).padStart(8, "0");
  return hex(high) + hex(low);
}

function utf8ByteLength(text: string): number {
  let length = 0;
  for (let index = 0; index < text.length; index++) {
    const codePoint = text.codePointAt(index) ?? 0xfffd;
    if (codePoint > 0xffff) index++;
    length += codePoint <= 0x7f ? 1
      : codePoint <= 0x7ff ? 2
      : codePoint <= 0xffff ? 3 : 4;
  }
  return length;
}

function utf8Bytes(text: string): number[] {
  const bytes: number[] = [];
  for (let index = 0; index < text.length; index++) {
    let codePoint = text.codePointAt(index) ?? 0xfffd;
    if (codePoint > 0xffff) index++;
    // TextEncoder replaces isolated UTF-16 surrogates with U+FFFD.
    if (codePoint >= 0xd800 && codePoint <= 0xdfff) codePoint = 0xfffd;
    if (codePoint <= 0x7f) {
      bytes.push(codePoint);
    } else if (codePoint <= 0x7ff) {
      bytes.push(0xc0 | (codePoint >> 6), 0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0xffff) {
      bytes.push(
        0xe0 | (codePoint >> 12),
        0x80 | ((codePoint >> 6) & 0x3f),
        0x80 | (codePoint & 0x3f),
      );
    } else {
      bytes.push(
        0xf0 | (codePoint >> 18),
        0x80 | ((codePoint >> 12) & 0x3f),
        0x80 | ((codePoint >> 6) & 0x3f),
        0x80 | (codePoint & 0x3f),
      );
    }
  }
  return bytes;
}

function assertPluginDataEntryFits(
  key: string,
  value: string,
  path: string,
): void {
  const entryBytes =
    utf8ByteLength(FIGMA_PLUGIN_ID) +
    utf8ByteLength(key) +
    utf8ByteLength(value) +
    FIGMA_PLUGIN_DATA_ENTRY_OVERHEAD_BYTES;
  if (entryBytes > FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES) {
    throw new Error(
      `${path}: plugin metadata entry "${key}" exceeds Figma's ` +
      `${FIGMA_PLUGIN_DATA_VALUE_MAX_BYTES}-byte total entry limit; ` +
      "import refused before mutation.",
    );
  }
}

function preflightPlanPluginData(plan: ForgeFigmaPlanNode, path: string): void {
  const entries: Array<[string, string | undefined]> = [
    ["pulp.design_ir_type", plan.sourceType],
    ["pulp.svg_visual_mode", plan.svgVisualMode],
    [
      "pulp.design_ir_attributes",
      Object.keys(plan.attributes).length > 0
        ? JSON.stringify(plan.attributes) : undefined,
    ],
    [
      "pulp.background_image_asset",
      plan.assetRole === "background" &&
          plan.kind !== "image" && plan.kind !== "svg" ? "1" : undefined,
    ],
    [
      "pulp.node_image_asset",
      plan.assetRole === "node" &&
          plan.kind !== "image" && plan.kind !== "svg" ? "1" : undefined,
    ],
    ["pulp.audio_widget", plan.audioWidget],
    ["pulp.audio_label", plan.audioLabel],
    ["pulp.audio_min", plan.audioMin === undefined ? undefined : String(plan.audioMin)],
    ["pulp.audio_max", plan.audioMax === undefined ? undefined : String(plan.audioMax)],
    [
      "pulp.audio_default",
      plan.audioDefault === undefined ? undefined : String(plan.audioDefault),
    ],
    ["pulp.audio_units", plan.audioUnits],
    ["pulp.binding", plan.binding],
    ["pulp.binding_y", plan.bindingY],
    ["pulp.source_node_id", plan.sourceNodeId],
    ["pulp.stable_anchor_id", plan.stableAnchorId],
    ["pulp.background_gradient", plan.backgroundGradient?.css],
    [
      "pulp.interactive_elements",
      plan.interactiveElements === undefined
        ? undefined : JSON.stringify(plan.interactiveElements),
    ],
    [
      "pulp.faithful_semantic_children",
      plan.preservedChildren === undefined
        ? undefined : JSON.stringify(plan.preservedChildren),
    ],
  ];
  for (const [key, value] of entries) {
    if (value !== undefined) assertPluginDataEntryFits(key, value, path);
  }
}

function hasManifestAssetReference(value: unknown): boolean {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return false;
  }
  const node = value as Record<string, unknown>;
  if (typeof first(node, "asset_ref", "assetRef") === "string") return true;
  if (typeof first(node, "svg_asset_id", "svgAssetId") === "string") return true;
  const attributes = record(node.attributes);
  if (Object.entries(attributes).some(([key, field]) =>
    (key === "asset_ref" || /AssetId$/.test(key)) &&
    typeof field === "string" && field.length > 0
  )) return true;
  const style = record(node.style);
  if (typeof first(style, "backgroundImage", "background_image") === "string") {
    return true;
  }
  return ["children", "alternate_frames", "alternateFrames"].some((key) =>
    Array.isArray(node[key]) &&
    (node[key] as unknown[]).some(hasManifestAssetReference)
  );
}

function validatePreservedSemanticNode(
  input: unknown,
  counters: { nodes: number; widgets: number },
  path: string,
): void {
  if (counters.nodes >= FORGE_CLIPBOARD_MAX_NODES) {
    throw new Error(`Design exceeds the ${FORGE_CLIPBOARD_MAX_NODES}-node clipboard limit.`);
  }
  if (input === null || typeof input !== "object" || Array.isArray(input)) {
    throw new Error(`${path}: expected a DesignIR node object.`);
  }
  counters.nodes++;
  const node = input as Record<string, unknown>;
  const collections: Array<[string, unknown]> = [
    ["children", node.children],
    ["alternate_frames", node.alternate_frames ?? node.alternateFrames],
  ];
  for (const [key, value] of collections) {
    if (value !== undefined && !Array.isArray(value)) {
      throw new Error(`${path}.${key}: expected an array.`);
    }
    for (const [index, child] of (Array.isArray(value) ? value : []).entries()) {
      validatePreservedSemanticNode(child, counters, `${path}.${key}[${index}]`);
    }
  }
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

function optionalNumber(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === "string" && value.length > 0 ? value : undefined;
}

function record(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
}

function first(source: Record<string, unknown>, ...keys: string[]): unknown {
  for (const key of keys) {
    if (source[key] !== undefined && source[key] !== null) return source[key];
  }
  return undefined;
}

function nonDefault(value: unknown): boolean {
  if (value === undefined || value === null || value === "") return false;
  if (value === false || value === 0) return false;
  if (Array.isArray(value)) return value.length > 0;
  return true;
}

function assertSupportedKeys(
  value: Record<string, unknown>,
  supported: Set<string>,
  path: string,
): void {
  for (const [key, member] of Object.entries(value)) {
    const present = member !== undefined && member !== null && member !== "" &&
      member !== false && (!Array.isArray(member) || member.length > 0);
    if (!supported.has(key) && present) {
      throw new Error(
        `${path}.${key}: this canonical DesignIR property cannot be represented by the Figma plugin; import refused before mutation.`,
      );
    }
  }
}

function clamp01(value: number): number {
  return Math.min(1, Math.max(0, value));
}

function colorToCss(color: ForgePlanColor): string {
  const r = Math.round(color.r * 255);
  const g = Math.round(color.g * 255);
  const b = Math.round(color.b * 255);
  return `rgba(${r},${g},${b},${color.a})`;
}

export function parseForgeColor(value: unknown, path: string): ForgePlanColor | undefined {
  if (value === undefined || value === null || value === "") return undefined;
  if (typeof value !== "string") {
    throw new Error(`${path}: expected a CSS color string.`);
  }
  const text = value.trim().toLowerCase();
  if (text === "transparent") return { r: 0, g: 0, b: 0, a: 0 };
  const hex = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/.exec(text);
  if (hex) {
    const raw = hex[1].length <= 4
      ? hex[1].split("").map((c) => c + c).join("")
      : hex[1];
    return {
      r: parseInt(raw.slice(0, 2), 16) / 255,
      g: parseInt(raw.slice(2, 4), 16) / 255,
      b: parseInt(raw.slice(4, 6), 16) / 255,
      a: raw.length === 8 ? parseInt(raw.slice(6, 8), 16) / 255 : 1,
    };
  }
  const rgb = /^rgba?\(\s*([+-]?(?:\d+\.?\d*|\.\d+))\s*,\s*([+-]?(?:\d+\.?\d*|\.\d+))\s*,\s*([+-]?(?:\d+\.?\d*|\.\d+))(?:\s*,\s*([+-]?(?:\d+\.?\d*|\.\d+)))?\s*\)$/.exec(text);
  if (rgb) {
    return {
      r: clamp01(Number(rgb[1]) / 255),
      g: clamp01(Number(rgb[2]) / 255),
      b: clamp01(Number(rgb[3]) / 255),
      a: rgb[4] === undefined ? 1 : clamp01(Number(rgb[4])),
    };
  }
  throw new Error(
    `${path}: Figma import supports hex, rgb(), rgba(), and transparent colors; got "${value}".`,
  );
}

function splitTopLevel(text: string): string[] {
  const parts: string[] = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    if (text[i] === "(") depth++;
    else if (text[i] === ")") depth--;
    else if (text[i] === "," && depth === 0) {
      parts.push(text.slice(start, i).trim());
      start = i + 1;
    }
  }
  parts.push(text.slice(start).trim());
  return parts.filter(Boolean);
}

function parseGradient(value: unknown, path: string): ForgePlanGradient | undefined {
  if (value === undefined || value === null || value === "") return undefined;
  if (typeof value !== "string") throw new Error(`${path}: expected a CSS gradient string.`);
  const match = /^(linear|radial|conic)-gradient\((.*)\)$/i.exec(value.trim());
  if (!match) {
    throw new Error(`${path}: only linear-gradient(), radial-gradient(), and conic-gradient() can be materialized in Figma.`);
  }
  if (match[1].toLowerCase() !== "linear") {
    throw new Error(
      `${path}: radial/conic CSS geometry is not losslessly invertible to Figma gradientTransform; import refused before mutation.`,
    );
  }
  const parts = splitTopLevel(match[2]);
  let angleDegrees = match[1].toLowerCase() === "linear" ? 180 : 0;
  if (parts.length > 0) {
    const angle = /^from\s+(-?\d+(?:\.\d+)?)deg(?:\s+at\s+.*)?$/i.exec(parts[0]) ??
      /^(-?\d+(?:\.\d+)?)deg$/i.exec(parts[0]);
    const direction = /^to\s+(top|right|bottom|left)$/i.exec(parts[0]);
    if (angle) {
      angleDegrees = Number(angle[1]);
      parts.shift();
    } else if (direction) {
      angleDegrees = ({ top: 0, right: 90, bottom: 180, left: 270 } as const)[
        direction[1].toLowerCase() as "top" | "right" | "bottom" | "left"
      ];
      parts.shift();
    } else if (/^(circle|ellipse)(?:\s+at\s+.*)?$/i.test(parts[0]) ||
               /^at\s+.*$/i.test(parts[0])) {
      parts.shift();
    }
  }
  if (parts.length < 2) throw new Error(`${path}: a Figma gradient needs at least two color stops.`);
  const stops = parts.map((part, index) => {
    const stop = /^(.*?)(?:\s+(-?\d+(?:\.\d+)?)%)?$/.exec(part);
    if (!stop) throw new Error(`${path}: invalid gradient stop "${part}".`);
    const percentage = stop[2] === undefined ? undefined : Number(stop[2]);
    if (percentage !== undefined && (percentage < 0 || percentage > 100)) {
      throw new Error(
        `${path}.stops[${index}]: positions outside 0%-100% cannot be represented losslessly in Figma; import refused before mutation.`,
      );
    }
    return {
      color: parseForgeColor(stop[1].trim(), `${path}.stops[${index}]`)!,
      position: percentage === undefined ? undefined : percentage / 100,
    };
  });
  if (stops[0].position === undefined) stops[0].position = 0;
  if (stops[stops.length - 1].position === undefined) {
    stops[stops.length - 1].position = 1;
  }
  for (let start = 0; start < stops.length - 1;) {
    const left = stops[start].position!;
    let end = start + 1;
    while (stops[end].position === undefined) end++;
    const right = stops[end].position!;
    const span = end - start;
    for (let index = start + 1; index < end; index++) {
      stops[index].position = left + (right - left) * ((index - start) / span);
    }
    start = end;
  }
  for (let index = 1; index < stops.length; index++) {
    if (stops[index].position! < stops[index - 1].position!) {
      throw new Error(
        `${path}.stops[${index}]: decreasing CSS stop positions cannot be represented losslessly in Figma; import refused before mutation.`,
      );
    }
  }
  for (let i = 1; i < stops.length; i++) {
    if (stops[i].position! < stops[i - 1].position!) {
      throw new Error(`${path}: gradient stop positions must be nondecreasing.`);
    }
  }
  return {
    kind: "linear",
    angleDegrees,
    stops: stops as ForgePlanGradientStop[],
    css: value.trim(),
  };
}

function parseEffects(style: Record<string, unknown>, path: string): ForgePlanEffect[] {
  const effects: ForgePlanEffect[] = [];
  const shadowValue = first(style, "boxShadow", "box_shadow");
  if (shadowValue !== undefined && shadowValue !== "") {
    if (typeof shadowValue !== "string") throw new Error(`${path}.boxShadow: expected a CSS string.`);
    for (const [index, layer] of splitTopLevel(shadowValue).entries()) {
      const inset = /\binset\b/i.test(layer);
      const colorMatches: RegExpExecArray[] = [];
      const colorPattern = /#[0-9a-f]{3,8}(?![0-9a-f])|rgba?\([^)]*\)|\btransparent\b/gi;
      let nextColor: RegExpExecArray | null;
      while ((nextColor = colorPattern.exec(layer)) !== null) colorMatches.push(nextColor);
      if (colorMatches.length !== 1) {
        throw new Error(
          `${path}.boxShadow[${index}]: expected exactly one supported color.`,
        );
      }
      const colorMatch = colorMatches[0];
      const colorStart = colorMatch.index!;
      const numeric = (
        layer.slice(0, colorStart) + layer.slice(colorStart + colorMatch[0].length)
      ).replace(/\binset\b/gi, "").trim();
      const nums = numeric.split(/\s+/).map((token) => {
        const m = /^(-?\d+(?:\.\d+)?)px$/.exec(token);
        if (m) return Number(m[1]);
        return /^[+-]?0(?:\.0+)?$/.test(token) ? 0 : Number.NaN;
      });
      if (nums.length < 3 || nums.length > 4 || nums.some((n) => !Number.isFinite(n))) {
        throw new Error(`${path}.boxShadow[${index}]: expected "xpx ypx blurpx [spreadpx] color [inset]".`);
      }
      effects.push({
        kind: inset ? "inner-shadow" : "drop-shadow",
        color: parseForgeColor(colorMatch[0], `${path}.boxShadow[${index}].color`),
        offsetX: nums[0],
        offsetY: nums[1],
        radius: nums[2],
        spread: nums[3],
      });
    }
  }
  for (const [key, kind] of [
    ["filter", "layer-blur"],
    ["backdropFilter", "background-blur"],
    ["backdrop_filter", "background-blur"],
  ] as const) {
    const value = style[key];
    if (value === undefined || value === "") continue;
    if (typeof value !== "string") throw new Error(`${path}.${key}: expected a CSS filter string.`);
    const layers = value.trim().split(/\s+(?=blur\()/);
    if (layers.length === 0) {
      throw new Error(`${path}.${key}: expected one or more blur(Npx) functions.`);
    }
    for (const [index, layer] of layers.entries()) {
      const match = /^blur\(\s*(\d+(?:\.\d+)?)px\s*\)$/.exec(layer);
      if (!match) {
        throw new Error(
          `${path}.${key}[${index}]: only blur(Npx) has a Figma effect equivalent; got "${layer}".`,
        );
      }
      effects.push({ kind, radius: Number(match[1]) });
    }
  }
  return effects;
}

function parseBlendMode(value: unknown, path: string): string | undefined {
  if (value === undefined || value === "") return undefined;
  if (typeof value !== "string") throw new Error(`${path}: expected a CSS blend-mode string.`);
  if (value.toLowerCase() === "normal") return undefined;
  const mapped = ({
    darken: "DARKEN", multiply: "MULTIPLY",
    "color-burn": "COLOR_BURN", lighten: "LIGHTEN", screen: "SCREEN",
    "color-dodge": "COLOR_DODGE", overlay: "OVERLAY",
    "soft-light": "SOFT_LIGHT", "hard-light": "HARD_LIGHT",
    difference: "DIFFERENCE", exclusion: "EXCLUSION", hue: "HUE",
    saturation: "SATURATION", color: "COLOR", luminosity: "LUMINOSITY",
  } as Record<string, string>)[value.toLowerCase()];
  if (!mapped) throw new Error(`${path}: blend mode "${value}" has no Figma equivalent.`);
  return mapped;
}

function align(value: unknown, path: string, axis: "primary" | "counter"):
  "MIN" | "MAX" | "CENTER" | "SPACE_BETWEEN" | undefined {
  if (value === undefined || value === "") return undefined;
  const normalized = String(value).replace(/_/g, "-").toLowerCase();
  if (normalized === "stretch") {
    if (axis === "primary") {
      throw new Error(
        `${path}: alignment "stretch" cannot be represented on Figma's primary axis.`,
      );
    }
    return undefined;
  }
  const mapped = ({
    "flex-start": "MIN", start: "MIN", "flex-end": "MAX", end: "MAX",
    center: "CENTER", "space-between": "SPACE_BETWEEN",
  } as Record<string, "MIN" | "MAX" | "CENTER" | "SPACE_BETWEEN">)[normalized];
  if (!mapped || (axis === "counter" && mapped === "SPACE_BETWEEN")) {
    throw new Error(`${path}: alignment "${value}" cannot be represented on Figma's ${axis} axis.`);
  }
  return mapped;
}

function sizingMode(value: unknown, path: string): "FIXED" | "HUG" | "FILL" | undefined {
  if (value === undefined || value === "") return undefined;
  const normalized = String(value).toLowerCase();
  if (normalized === "fixed") return "FIXED";
  if (normalized === "hug") return "HUG";
  if (normalized === "fill") return "FILL";
  throw new Error(`${path}: sizing mode "${value}" is unsupported.`);
}

function sizeConstraint(
  value: unknown,
  path: string,
  kind: "min" | "max",
): number | undefined {
  if (value === undefined || value === null) return undefined;
  const parsed = optionalNumber(value);
  if (parsed === undefined || parsed < 0 || (kind === "max" && parsed === 0)) {
    throw new Error(
      `${path}: expected a ${kind === "max" ? "positive" : "non-negative"} finite size.`,
    );
  }
  // CSS min-size zero is the unconstrained default and needs no Figma setter.
  return parsed === 0 ? undefined : parsed;
}

function constraint(
  value: unknown,
  path: string,
  axis: "horizontal" | "vertical",
): "MIN" | "CENTER" | "MAX" | "STRETCH" | "SCALE" {
  const normalized = String(value ?? (axis === "horizontal" ? "left" : "top")).toLowerCase();
  const horizontal: Record<string, "MIN" | "CENTER" | "MAX" | "STRETCH" | "SCALE"> =
    { left: "MIN", min: "MIN", right: "MAX", max: "MAX", center: "CENTER", stretch: "STRETCH", scale: "SCALE" };
  const vertical: Record<string, "MIN" | "CENTER" | "MAX" | "STRETCH" | "SCALE"> =
    { top: "MIN", min: "MIN", bottom: "MAX", max: "MAX", center: "CENTER", stretch: "STRETCH", scale: "SCALE" };
  const mapped = (axis === "horizontal" ? horizontal : vertical)[normalized];
  if (!mapped) throw new Error(`${path}: ${axis} constraint "${value}" is unsupported.`);
  return mapped;
}

function parseAssets(
  design: Record<string, unknown>,
  envelopeAssets: unknown,
): Map<string, ForgePlanAsset> {
  const manifest = record(design.assetManifest ?? design.asset_manifest);
  const refs = Array.isArray(manifest.assets) ? manifest.assets : [];
  const supplied = new Map<string, { mime: string; bytes: number[] }>();
  if (envelopeAssets !== undefined && !Array.isArray(envelopeAssets)) {
    throw new Error("$.assets: expected an array.");
  }
  for (const [index, raw] of (Array.isArray(envelopeAssets) ? envelopeAssets : []).entries()) {
    const asset = record(raw);
    const hash = optionalString(asset.content_hash);
    const mime = optionalString(asset.mime);
    const base64 = optionalString(asset.base64);
    if (!hash || !/^(?:[0-9a-f]{16}|[0-9a-f]{64})$/.test(hash)) {
      throw new Error(
        `$.assets[${index}].content_hash: expected lowercase SHA-256 or FNV-1a fallback.`,
      );
    }
    if (!mime || !base64) throw new Error(`$.assets[${index}]: mime and base64 are required.`);
    if (supplied.has(hash)) throw new Error(`$.assets[${index}]: duplicate content hash ${hash}.`);
    const bytes = base64ToBytes(base64, `$.assets[${index}].base64`);
    const actualHash = hash.length === 64 ? sha256Hex(bytes) : fnv1a64Hex(bytes);
    if (actualHash !== hash) {
      throw new Error(`$.assets[${index}]: content_hash does not match the decoded bytes.`);
    }
    supplied.set(hash, { mime, bytes });
  }

  const resolved = new Map<string, ForgePlanAsset>();
  for (const [index, raw] of refs.entries()) {
    const ref = record(raw);
    const assetId = optionalString(ref.asset_id);
    const hash = optionalString(ref.content_hash);
    const mime = optionalString(ref.mime);
    if (!assetId) throw new Error(`$.design_ir.assetManifest.assets[${index}].asset_id: required.`);
    if (resolved.has(assetId)) {
      throw new Error(
        `$.design_ir.assetManifest.assets[${index}].asset_id: duplicate asset ID "${assetId}".`,
      );
    }
    if (!hash || !/^(?:[0-9a-f]{16}|[0-9a-f]{64})$/.test(hash)) {
      throw new Error(
        `$.design_ir.assetManifest.assets[${index}].content_hash: ` +
        "a lowercase SHA-256 or FNV-1a fallback is required for Figma copy-back.",
      );
    }
    let bytes: number[] | undefined;
    const suppliedAsset = supplied.get(hash);
    if (suppliedAsset) {
      if (mime && suppliedAsset.mime !== mime) {
        throw new Error(`$.design_ir.assetManifest.assets[${index}]: MIME disagrees with the clipboard asset.`);
      }
      bytes = suppliedAsset.bytes;
    } else {
      const uri = optionalString(ref.original_uri);
      const data = uri && /^data:([^;,]+);base64,(.*)$/i.exec(uri);
      if (data) {
        if (mime && data[1] !== mime) {
          throw new Error(`$.design_ir.assetManifest.assets[${index}]: MIME disagrees with its data URI.`);
        }
        bytes = base64ToBytes(data[2], `$.design_ir.assetManifest.assets[${index}].original_uri`);
        const actualHash = hash.length === 64 ? sha256Hex(bytes) : fnv1a64Hex(bytes);
        if (actualHash !== hash) {
          throw new Error(`$.design_ir.assetManifest.assets[${index}]: content_hash does not match its data URI bytes.`);
        }
      }
    }
    if (!bytes) {
      throw new Error(
        `$.design_ir.assetManifest.assets[${index}] (${assetId}): bytes are unavailable; Forge must embed the content-addressed asset before Figma can materialize it.`,
      );
    }
    resolved.set(assetId, {
      assetId,
      contentHash: hash,
      mime: mime ?? suppliedAsset?.mime ?? "",
      bytes,
      originalUri: optionalString(ref.original_uri),
      originalUriAliases: Array.isArray(ref.original_uri_aliases)
        ? ref.original_uri_aliases.map((alias, aliasIndex) => {
            const parsed = optionalString(alias);
            if (!parsed) {
              throw new Error(
                `$.design_ir.assetManifest.assets[${index}].original_uri_aliases[${aliasIndex}]: expected a non-empty string.`,
              );
            }
            return parsed;
          })
        : [],
    });
  }
  return resolved;
}

function assetFor(
  assetId: string | undefined,
  assets: Map<string, ForgePlanAsset>,
  path: string,
): ForgePlanAsset | undefined {
  if (!assetId) return undefined;
  const unwrapped = assetId.trim().replace(
    /^url\(\s*(['"]?)(.*?)\1\s*\)$/i,
    "$2",
  );
  const direct = assets.get(unwrapped);
  const uriMatches = direct ? [] : [...assets.values()].filter(
    (candidate) => candidate.originalUri === unwrapped ||
      candidate.originalUriAliases?.includes(unwrapped),
  );
  if (uriMatches.length > 1) {
    throw new Error(`${path}: asset URI "${unwrapped}" is ambiguous in assetManifest.`);
  }
  const asset = direct ?? uriMatches[0];
  if (!asset) throw new Error(`${path}: asset "${assetId}" is absent from assetManifest.`);
  return asset;
}

function textDecoration(value: unknown, path: string): "UNDERLINE" | "STRIKETHROUGH" | undefined {
  if (value === undefined || value === "" || value === "none") return undefined;
  const normalized = String(value).toLowerCase();
  if (normalized === "underline") return "UNDERLINE";
  if (normalized === "line-through") return "STRIKETHROUGH";
  throw new Error(`${path}: text decoration "${value}" is unsupported.`);
}

function textRunDecoration(
  value: unknown,
  path: string,
): "NONE" | "UNDERLINE" | "STRIKETHROUGH" | undefined {
  if (value === undefined || value === "") return undefined;
  if (value === "none") return "NONE";
  return textDecoration(value, path);
}

function figmaFontStyle(weight: number | undefined, explicit: string | undefined): string {
  const weightStyle = weight && weight >= 900 ? "Black"
    : weight && weight >= 800 ? "Extra Bold"
    : weight && weight >= 700 ? "Bold"
    : weight && weight >= 600 ? "Semi Bold"
    : weight && weight >= 500 ? "Medium"
    : weight && weight >= 400 ? "Regular"
    : weight && weight >= 300 ? "Light"
    : weight && weight >= 200 ? "Extra Light"
    : weight ? "Thin" : "Regular";
  return explicit?.toLowerCase() === "italic"
    ? (weightStyle === "Regular" ? "Italic" : `${weightStyle} Italic`)
    : explicit?.toLowerCase() === "normal" || !explicit ? weightStyle : explicit;
}

function resolveFontStyle(
  family: string | undefined,
  weight: number | undefined,
  explicit: string | undefined,
  catalog: ForgeFontCatalogEntry[],
): string {
  if (family) {
    const familyEntries = catalog.filter((entry) => entry.family === family);
    if (familyEntries.length > 0) {
      const explicitLower = explicit?.toLowerCase();
      const semanticStyle =
        explicitLower === "normal" || explicitLower === "italic";
      const wantsItalic = explicitLower === "italic";
      const targetWeight = weight ??
        (semanticStyle || explicit === undefined ? 400 : undefined);
      const exact = familyEntries.find((entry) => {
        const catalogWeight = entry.weight ??
          (/^(normal|regular|italic|oblique)$/i.test(entry.style) ? 400 : undefined);
        return (targetWeight === undefined || catalogWeight === targetWeight) &&
        (explicit === undefined
          ? !(entry.italic ?? /italic|oblique/i.test(entry.style))
          : semanticStyle
            ? (entry.italic ?? /italic|oblique/i.test(entry.style)) === wantsItalic
            : entry.style.toLowerCase() === explicitLower);
      });
      if (exact) {
        const semantic = exact.style.toLowerCase();
        return semantic === "normal" || semantic === "italic"
          ? figmaFontStyle(exact.weight ?? weight, semantic)
          : exact.style;
      }
      throw new Error(
        `Font catalog has no exact ${family} ${weight ?? ""} ${explicit ?? ""} face for Figma copy-back.`,
      );
    }
  }
  return figmaFontStyle(weight, explicit);
}

function parseTextRuns(
  node: Record<string, unknown>,
  path: string,
  fontFamily: string | undefined,
  baseFontWeight: number | undefined,
  baseFontStyle: string | undefined,
  fontCatalog: ForgeFontCatalogEntry[],
): ForgePlanTextRun[] {
  const rawRuns = node.runs ?? node.textRuns;
  if (rawRuns === undefined) return [];
  if (!Array.isArray(rawRuns)) throw new Error(`${path}.runs: expected an array.`);
  const content = typeof node.content === "string" ? node.content : "";
  const contentBytes = utf8ByteLength(content);
  return rawRuns.map((raw, index) => {
    const run = record(raw);
    const start = optionalNumber(run.start);
    const end = optionalNumber(run.end);
    if (start === undefined || end === undefined || start < 0 || end <= start) {
      throw new Error(`${path}.runs[${index}]: start/end must describe a non-empty range.`);
    }
    if (end > contentBytes) {
      throw new Error(`${path}.runs[${index}].end: exceeds the UTF-8 content length.`);
    }
    const fontWeight = optionalNumber(run.fontWeight);
    const explicitFontStyle = optionalString(run.fontStyle);
    return {
      start, end,
      fontSize: optionalNumber(run.fontSize),
      fontWeight,
      fontStyle: fontWeight !== undefined || explicitFontStyle !== undefined
        ? resolveFontStyle(
            fontFamily,
            fontWeight ?? baseFontWeight,
            explicitFontStyle ?? baseFontStyle,
            fontCatalog,
          )
        : undefined,
      color: parseForgeColor(run.color, `${path}.runs[${index}].color`),
      letterSpacing: optionalNumber(run.letterSpacing),
      textDecoration: textRunDecoration(
        run.textDecoration,
        `${path}.runs[${index}].textDecoration`,
      ),
    };
  });
}

function parseInteractiveElements(
  raw: unknown,
  path: string,
): InteractiveElement[] | undefined {
  if (raw === undefined) return undefined;
  if (!Array.isArray(raw)) {
    throw new Error(`${path}: expected an array of typed control objects.`);
  }
  const numberFields = new Set([
    "cx", "cy", "hit_radius", "default_value", "default_value_y",
    "confidence_score", "x", "y", "w", "h",
  ]);
  const integerFields = new Set([
    "target_frame", "resolution_rung", "selected_index",
  ]);
  const stringFields = new Set([
    "svg_patch_d", "action", "text", "factory_id", "custom_props",
    "placeholder", "bg_color", "label", "source_node_id", "param_key",
  ]);
  const booleanFields = new Set([
    "flash", "value_left_align", "verification_pass",
  ]);
  const stringArrayFields = new Set(["conflict_signals", "options"]);
  const allowed = new Set([
    "kind",
    ...numberFields,
    ...integerFields,
    ...stringFields,
    ...booleanFields,
    ...stringArrayFields,
  ]);
  return raw.map((value, index) => {
    if (value === null || typeof value !== "object" || Array.isArray(value)) {
      throw new Error(`${path}[${index}]: expected an object.`);
    }
    const entry = { ...(value as Record<string, unknown>) };
    const kind = entry.kind;
    if (typeof kind !== "string" || !INTERACTIVE_KINDS.has(kind)) {
      throw new Error(`${path}[${index}].kind: unsupported interactive control.`);
    }
    for (const [key, field] of Object.entries(entry)) {
      if (!allowed.has(key)) {
        throw new Error(`${path}[${index}].${key}: unsupported interactive field.`);
      }
      if (key === "kind") continue;
      if (numberFields.has(key) &&
          (typeof field !== "number" || !Number.isFinite(field))) {
        throw new Error(`${path}[${index}].${key}: expected a finite number.`);
      }
      if (integerFields.has(key) &&
          (typeof field !== "number" || !Number.isInteger(field))) {
        throw new Error(`${path}[${index}].${key}: expected an integer.`);
      }
      if (stringFields.has(key) && typeof field !== "string") {
        throw new Error(`${path}[${index}].${key}: expected a string.`);
      }
      if (booleanFields.has(key) && typeof field !== "boolean") {
        throw new Error(`${path}[${index}].${key}: expected a boolean.`);
      }
      if (stringArrayFields.has(key) &&
          (!Array.isArray(field) ||
           field.some((item) => typeof item !== "string"))) {
        throw new Error(`${path}[${index}].${key}: expected an array of strings.`);
      }
    }
    if (kind !== "knob") {
      // The canonical serializer elides zero-valued coordinates.
      if (entry.x === undefined) entry.x = 0;
      if (entry.y === undefined) entry.y = 0;
    }
    const required = kind === "knob"
      ? ["cx", "cy", "hit_radius"]
      : ["w", "h"];
    if (kind === "custom") required.push("factory_id");
    for (const key of required) {
      if (entry[key] === undefined) {
        throw new Error(`${path}[${index}].${key}: required for ${kind}.`);
      }
    }
    return entry as unknown as InteractiveElement;
  });
}

function toPlanNode(
  input: unknown,
  counters: { nodes: number; widgets: number },
  assets: Map<string, ForgePlanAsset>,
  path: string,
  fontCatalog: ForgeFontCatalogEntry[],
  parentIsAutoLayout = false,
): ForgeFigmaPlanNode {
  if (counters.nodes >= FORGE_CLIPBOARD_MAX_NODES) {
    throw new Error(`Design exceeds the ${FORGE_CLIPBOARD_MAX_NODES}-node clipboard limit.`);
  }
  if (input === null || typeof input !== "object" || Array.isArray(input)) {
    throw new Error(`${path}: expected a DesignIR node object.`);
  }
  counters.nodes++;

  const node = record(input);
  const style = record(node.style);
  const layout = record(node.layout);
  const requestedHorizontalSizing = sizingMode(
    first(layout, "widthMode", "width_mode"),
    `${path}.layout.widthMode`,
  );
  const requestedVerticalSizing = sizingMode(
    first(layout, "heightMode", "height_mode"),
    `${path}.layout.heightMode`,
  );
  if (!parentIsAutoLayout &&
      (requestedHorizontalSizing === "FILL" || requestedVerticalSizing === "FILL")) {
    throw new Error(
      `${path}.layout: FILL sizing requires an auto-layout parent; import refused before mutation.`,
    );
  }
  const paddingValue = layout.padding;
  const paddingScalar = typeof paddingValue === "number" &&
    Number.isFinite(paddingValue) ? paddingValue : undefined;
  if (paddingValue !== undefined && paddingScalar === undefined &&
      (paddingValue === null || typeof paddingValue !== "object" ||
       Array.isArray(paddingValue))) {
    throw new Error(`${path}.layout.padding: expected a finite number or edge object.`);
  }
  const padding = record(paddingValue);
  const attrs = record(node.attributes);
  const preservedAttributes: Record<string, string> = {};
  for (const [key, value] of Object.entries(attrs)) {
    if (typeof value !== "string") {
      throw new Error(`${path}.attributes.${key}: expected a string.`);
    }
    preservedAttributes[key] = value;
  }
  assertSupportedKeys(style, SUPPORTED_STYLE_KEYS, `${path}.style`);
  assertSupportedKeys(layout, SUPPORTED_LAYOUT_KEYS, `${path}.layout`);
  const renderBoundsValue = first(style, "renderBounds", "render_bounds");
  let parsedRenderBounds:
    { w: number; h: number; dx: number; dy: number } | undefined;
  if (renderBoundsValue !== undefined) {
    if (renderBoundsValue === null || typeof renderBoundsValue !== "object" ||
        Array.isArray(renderBoundsValue)) {
      throw new Error(`${path}.style.renderBounds: expected {w,h,dx?,dy?}.`);
    }
    const renderBounds = renderBoundsValue as Record<string, unknown>;
    const unknown = Object.keys(renderBounds).find(
      (key) => key !== "w" && key !== "h" && key !== "dx" && key !== "dy",
    );
    if (unknown) {
      throw new Error(`${path}.style.renderBounds.${unknown}: unsupported field.`);
    }
    const w = optionalNumber(renderBounds.w);
    const h = optionalNumber(renderBounds.h);
    const dx = renderBounds.dx === undefined ? 0 : optionalNumber(renderBounds.dx);
    const dy = renderBounds.dy === undefined ? 0 : optionalNumber(renderBounds.dy);
    if (w === undefined || h === undefined || w <= 0 || h <= 0 ||
        dx === undefined || dy === undefined) {
      throw new Error(
        `${path}.style.renderBounds: w/h must be positive finite numbers and dx/dy finite when present.`,
      );
    }
    parsedRenderBounds = { w, h, dx, dy };
    // Derived capture metadata. Figma recomputes it from the materialized
    // geometry, strokes, and effects; it is deliberately not assigned.
  }

  const type = (optionalString(node.type) ?? "frame").toLowerCase();
  const audioWidget =
    optionalString(node.audioWidget) ?? optionalString(node.audio_widget) ?? "none";
  if (audioWidget && audioWidget !== "none" && !AUDIO_WIDGETS.has(audioWidget)) {
    throw new Error(`${path}.audioWidget: Unsupported audio widget "${audioWidget}".`);
  }
  if (audioWidget && audioWidget !== "none") counters.widgets++;

  const renderMode = optionalString(node.render_mode) ?? optionalString(node.renderMode);
  if (node.children !== undefined && !Array.isArray(node.children)) {
    throw new Error(`${path}.children: expected an array.`);
  }
  const rawAlternates = node.alternate_frames ?? node.alternateFrames;
  if (rawAlternates !== undefined && !Array.isArray(rawAlternates)) {
    throw new Error(`${path}.alternate_frames: expected an array.`);
  }
  const childInputs = (node.children ?? []) as unknown[];
  const alternateInputs = (rawAlternates ?? []) as unknown[];
  const thisIsAutoLayout = optionalString(layout.display) === "flex";
  let children: ForgeFigmaPlanNode[];
  if (renderMode === "faithful_svg") {
    childInputs.forEach((child, index) => {
      validatePreservedSemanticNode(
        child,
        counters,
        `${path}.children[${index}]`,
      );
    });
    children = [];
  } else {
    children = childInputs.map((child, index) =>
      toPlanNode(
        child,
        counters,
        assets,
        `${path}.children[${index}]`,
        fontCatalog,
        thisIsAutoLayout,
      ));
  }
  const alternateFrames = alternateInputs.map((child, index) =>
    toPlanNode(
      child,
      counters,
      assets,
      `${path}.alternate_frames[${index}]`,
      fontCatalog,
      false,
    ));
  if (alternateFrames.length > 0 && path !== "$.design_ir.root") {
    throw new Error(
      `${path}.alternate_frames: nested alternate-frame sets have no unambiguous Figma component-set representation; import refused before mutation.`,
    );
  }
  const parsedInteractiveElements = parseInteractiveElements(
    node.interactive_elements ?? node.interactiveElements,
    `${path}.interactive_elements`,
  );
  // Forge's canonical serializer omits empty vectors. For faithful SVG, an
  // absent collection therefore means the authoritative set is empty, not
  // "please re-run heuristic detection."
  const interactiveElements = parsedInteractiveElements ??
    (renderMode === "faithful_svg" ? [] : undefined);

  const svgAssetId = optionalString(node.svg_asset_id) ?? optionalString(node.svgAssetId);
  const rawStyleBackgroundImage =
    optionalString(first(style, "backgroundImage", "background_image"));
  const styleBackgroundImage =
    rawStyleBackgroundImage?.toLowerCase() === "none" ? undefined : rawStyleBackgroundImage;
  const fallbackAssetIds = Object.keys(attrs)
    .filter((key) => /AssetId$/.test(key) && optionalString(attrs[key]));
  const genericFallbackAssetIds = fallbackAssetIds.filter(
    (key) => key !== "srcAssetId" &&
      key !== "backgroundImageAssetId" &&
      key !== "hrefAssetId",
  );
  if (type === "image" && fallbackAssetIds.length > 1 &&
      !optionalString(attrs.srcAssetId) &&
      !optionalString(attrs.backgroundImageAssetId) &&
      !optionalString(attrs.hrefAssetId)) {
    throw new Error(
      `${path}.attributes: multiple fallback AssetId fields are ambiguous for an image node.`,
    );
  }
  const topLevelAssetId = optionalString(node.asset_ref);
  const nodeAssetId = topLevelAssetId ??
    optionalString(attrs.srcAssetId) ??
    optionalString(attrs.hrefAssetId) ??
    optionalString(attrs.asset_ref);
  const backgroundAssetId =
    optionalString(attrs.backgroundImageAssetId) ?? styleBackgroundImage;
  if ((nodeAssetId || genericFallbackAssetIds.length > 0) && backgroundAssetId) {
    throw new Error(
      `${path}: independent node and background image assets cannot be round-tripped through one Figma image paint; import refused before mutation.`,
    );
  }
  const selectedAttributeAssetKey = topLevelAssetId ? undefined
    : optionalString(attrs.srcAssetId) ? "srcAssetId"
    : optionalString(attrs.backgroundImageAssetId) ? "backgroundImageAssetId"
    : optionalString(attrs.hrefAssetId) ? "hrefAssetId"
    : optionalString(attrs.asset_ref) ? "asset_ref"
    : type === "image" && fallbackAssetIds.length === 1
      ? fallbackAssetIds[0]
      : undefined;
  const directAssetId = topLevelAssetId ??
    (selectedAttributeAssetKey
      ? optionalString(attrs[selectedAttributeAssetKey])
      : styleBackgroundImage);
  let asset = assetFor(renderMode === "faithful_svg" ? svgAssetId : directAssetId, assets,
    `${path}.${renderMode === "faithful_svg" ? "svg_asset_id" : "asset_ref"}`);
  if (asset) {
    // The plugin captures a fresh content-addressed asset after Figma edits.
    // Keeping source IDs here would make downstream first_asset_id() select a
    // stale attribute instead of the new top-level asset_ref/svg_asset_id.
    for (const key of Object.keys(preservedAttributes)) {
      if (key !== "asset_ref" && !/AssetId$/.test(key)) continue;
      if (key !== selectedAttributeAssetKey) {
        throw new Error(
          `${path}.attributes.${key}: multiple independently meaningful asset references cannot be round-tripped through one Figma image paint; import refused before mutation.`,
        );
      }
      delete preservedAttributes[key];
    }
  }

  let kind: ForgeFigmaPlanNode["kind"];
  let inlineVector = false;
  if (renderMode === "faithful_svg") {
    if (!asset || asset.mime !== "image/svg+xml") {
      throw new Error(`${path}.svg_asset_id: faithful_svg requires an embedded image/svg+xml asset.`);
    }
    kind = "svg";
  } else if (renderMode && renderMode !== "normal") {
    throw new Error(`${path}.render_mode: unsupported value "${renderMode}".`);
  } else if (type === "text" || type === "label") kind = "text";
  else if (type === "ellipse" || type === "circle") kind = "ellipse";
  else if (type === "rectangle" || type === "rect") kind = "rectangle";
  else if (type === "image") {
    if (!asset) throw new Error(`${path}.asset_ref: image nodes require an embedded content-addressed asset.`);
    if (asset.mime === "image/svg+xml") {
      const sizingClaim = first(
        style,
        "objectFit",
        "object_fit",
        "backgroundSize",
        "background_size",
        "backgroundRepeat",
        "background_repeat",
      );
      if (nonDefault(sizingClaim)) {
        throw new Error(
          `${path}.style: SVG image sizing needs a scaled wrapper and cannot be losslessly materialized; import refused before mutation.`,
        );
      }
      kind = "svg";
    } else {
      kind = "image";
    }
  } else if (type === "vector" || type === "path" || type === "svg_path") {
    if (!asset) {
      const pathData = optionalString(attrs.path_data) ?? optionalString(attrs.d);
      if (!pathData) {
        throw new Error(`${path}: vector/path nodes need an SVG asset or attributes.path_data.`);
      }
      const width = Math.max(1, finiteNumber(first(style, "width"), 100));
      const height = Math.max(1, finiteNumber(first(style, "height"), 100));
      if (first(style, "backgroundGradient", "background_gradient") !== undefined) {
        throw new Error(
          `${path}.style.backgroundGradient: inline path gradients require an SVG asset; import refused before mutation.`,
        );
      }
      const fill = parseForgeColor(
        first(style, "backgroundColor", "background_color", "color"),
        `${path}.style.backgroundColor`,
      );
      const stroke = parseForgeColor(
        first(style, "borderColor", "border_color"),
        `${path}.style.borderColor`,
      );
      const strokeWidth = optionalNumber(first(style, "borderWidth", "border_width")) ?? 0;
      const preservedDash = optionalString(attrs["figma:dash_pattern"]);
      const dashValues = preservedDash
        ? preservedDash.split(",").map((part) => Number(part.trim()))
        : [4, 4];
      if (dashValues.length === 0 ||
          dashValues.some((part) => !Number.isFinite(part) || part <= 0)) {
        throw new Error(
          `${path}.attributes.figma:dash_pattern: expected positive comma-separated numbers.`,
        );
      }
      const dash = optionalString(first(style, "borderStyle", "border_style")) === "dashed"
        ? ` stroke-dasharray="${dashValues.join(" ")}"` : "";
      const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}"><path d="${pathData.replace(/&/g, "&amp;").replace(/"/g, "&quot;")}" fill="${fill ? colorToCss(fill) : "none"}" stroke="${stroke && strokeWidth > 0 ? colorToCss(stroke) : "none"}" stroke-width="${strokeWidth}"${dash}/></svg>`;
      asset = {
        assetId: `${path}:inline-svg`,
        contentHash: "",
        mime: "image/svg+xml",
        bytes: utf8Bytes(svg),
      };
      inlineVector = true;
    }
    if (asset.mime !== "image/svg+xml") {
      throw new Error(`${path}.asset_ref: vector/path materialization requires image/svg+xml bytes.`);
    }
    kind = "svg";
  } else if (FRAME_TYPES.has(type)) kind = "frame";
  else {
    throw new Error(`${path}.type: node type "${type}" has no truthful Figma materialization.`);
  }
  if (renderMode === "faithful_svg" && parsedRenderBounds) {
    const layoutWidth = Math.max(1, finiteNumber(first(style, "width"), 100));
    const layoutHeight = Math.max(1, finiteNumber(first(style, "height"), 100));
    const { w, h, dx, dy } = parsedRenderBounds;
    if (Math.abs(w - layoutWidth) > 1e-6 ||
        Math.abs(h - layoutHeight) > 1e-6 ||
        Math.abs(dx) > 1e-6 || Math.abs(dy) > 1e-6) {
      throw new Error(
        `${path}.style.renderBounds: faithful SVG capture bounds differ from ` +
        "the layout box and require a wrapper representation; import refused before mutation.",
      );
    }
  }
  if (asset && asset.mime !== "image/svg+xml" &&
      asset.mime !== "image/png" && asset.mime !== "image/jpeg" &&
      asset.mime !== "image/gif") {
    throw new Error(
      `${path}.asset_ref: Figma createImage supports PNG, JPEG, and GIF; got "${asset.mime}".`,
    );
  }
  if (asset?.mime === "image/svg+xml" && kind !== "svg") {
    throw new Error(
      `${path}.asset_ref: SVG backgrounds require a faithful wrapper and cannot be passed to Figma createImage; import refused before mutation.`,
    );
  }
  if (kind !== "text" && !inlineVector) {
    const inheritedTypography = [
      "color", "fontFamily", "font_family", "fontSize", "font_size",
      "fontWeight", "font_weight", "fontStyle", "font_style", "textAlign",
      "text_align", "verticalAlign", "vertical_align", "letterSpacing",
      "letter_spacing", "lineHeight", "line_height", "textTransform",
      "text_transform", "textDecoration", "text_decoration",
    ].find((key) => nonDefault(style[key]));
    if (inheritedTypography) {
      throw new Error(
        `${path}.style.${inheritedTypography}: inheritable typography on non-text nodes cannot be represented truthfully in Figma; import refused before mutation.`,
      );
    }
  }

  if (children.length > 0 && kind !== "frame" && renderMode !== "faithful_svg") {
    throw new Error(`${path}.children: ${kind} is a Figma leaf; refusing to flatten ${children.length} child node(s).`);
  }

  const direction = optionalString(layout.direction);
  if (direction && direction !== "row" && direction !== "column") {
    throw new Error(`${path}.layout.direction: "${direction}" is unsupported.`);
  }
  const display = optionalString(layout.display);
  if (display && display !== "flex") {
    throw new Error(`${path}.layout.display: Figma copy-back supports flex/auto-layout only; got "${display}".`);
  }
  if (display !== "flex") {
    const nonFlexSpacing = [
      ["gap", layout.gap],
      ["rowGap", first(layout, "rowGap", "row_gap")],
      ["columnGap", first(layout, "columnGap", "column_gap")],
      ["padding", paddingScalar ?? (
        Object.values(padding).some((value) => (optionalNumber(value) ?? 0) !== 0)
          ? 1 : 0
      )],
      ["paddingTop", first(layout, "paddingTop", "padding_top")],
      ["paddingRight", first(layout, "paddingRight", "padding_right")],
      ["paddingBottom", first(layout, "paddingBottom", "padding_bottom")],
      ["paddingLeft", first(layout, "paddingLeft", "padding_left")],
      ["wrap", layout.wrap],
      ["alignContent", first(layout, "alignContent", "align_content")],
      ["widthMode", optionalString(first(layout, "widthMode", "width_mode"))
        ?.toLowerCase() === "fixed" ? undefined
        : first(layout, "widthMode", "width_mode")],
      ["heightMode", optionalString(first(layout, "heightMode", "height_mode"))
        ?.toLowerCase() === "fixed" ? undefined
        : first(layout, "heightMode", "height_mode")],
    ].find(([, value]) => nonDefault(value));
    if (nonFlexSpacing) {
      throw new Error(
        `${path}.layout.${nonFlexSpacing[0]}: layout spacing/sizing requires display:flex for truthful Figma materialization; import refused before mutation.`,
      );
    }
  }

  const constraintsObj = record(layout.constraints ?? node.constraints);
  const position = optionalString(style.position);
  if (position && position !== "absolute" && position !== "relative") {
    throw new Error(`${path}.style.position: "${position}" is unsupported.`);
  }
  if (position === "relative" &&
      ((optionalNumber(style.left) ?? 0) !== 0 ||
       (optionalNumber(style.top) ?? 0) !== 0)) {
    throw new Error(
      `${path}.style.position: relative offsets have no truthful Figma auto-layout equivalent; import refused before mutation.`,
    );
  }
  const backgroundImage = styleBackgroundImage;
  if (backgroundImage && !asset) {
    throw new Error(
      `${path}.style.backgroundImage: referenced bytes are unavailable in assetManifest.`,
    );
  }
  const objectFit = optionalString(first(style, "objectFit", "object_fit"));
  const backgroundSize = optionalString(first(style, "backgroundSize", "background_size"));
  const backgroundRepeat = optionalString(first(style, "backgroundRepeat", "background_repeat"));
  for (const [value, allowed, member] of [
    [objectFit, new Set(["cover", "contain"]), "objectFit"],
    [backgroundSize, new Set(["cover", "contain", "auto"]), "backgroundSize"],
    [backgroundRepeat, new Set(["repeat", "no-repeat"]), "backgroundRepeat"],
  ] as const) {
    if (value && !allowed.has(value)) {
      throw new Error(
        `${path}.style.${member}: "${value}" has no exact Figma image scaleMode equivalent; import refused before mutation.`,
      );
    }
  }
  if (objectFit && backgroundSize && backgroundSize !== "auto" &&
      objectFit !== backgroundSize) {
    throw new Error(`${path}.style: objectFit and backgroundSize request conflicting image sizing.`);
  }
  if (backgroundSize === "auto" && !objectFit && backgroundRepeat !== "repeat") {
    throw new Error(
      `${path}.style.backgroundSize: "auto" with a non-tiled image has no exact Figma scaleMode; import refused before mutation.`,
    );
  }
  if (backgroundRepeat === "repeat" &&
      (objectFit !== undefined || (backgroundSize !== undefined && backgroundSize !== "auto"))) {
    throw new Error(
      `${path}.style: tiled images cannot also request cover/contain sizing in Figma; import refused before mutation.`,
    );
  }
  if (asset && kind !== "svg" &&
      !objectFit && !backgroundSize && backgroundRepeat !== "repeat") {
    throw new Error(
      `${path}.style: omitted image/background sizing has no exact Figma image scaleMode; import refused before mutation.`,
    );
  }
  const imageScaleMode = backgroundRepeat === "repeat" ? "TILE"
    : objectFit === "contain" || backgroundSize === "contain" ? "FIT" : "FILL";
  const backgroundColor = parseForgeColor(
    first(style, "backgroundColor", "background_color"),
    `${path}.style.backgroundColor`,
  );
  const backgroundGradient = parseGradient(
    first(style, "backgroundGradient", "background_gradient"),
    `${path}.style.backgroundGradient`,
  );
  if (kind === "text" &&
      (backgroundColor || backgroundGradient || asset || backgroundImage)) {
    throw new Error(
      `${path}.style: text backgrounds require a wrapper frame and cannot be losslessly materialized; import refused before mutation.`,
    );
  }
  if (kind === "svg" && renderMode !== "faithful_svg" && !inlineVector &&
      (backgroundColor || backgroundGradient)) {
    throw new Error(
      `${path}.style: backgrounds behind SVG assets require a wrapper frame and cannot be losslessly materialized; import refused before mutation.`,
    );
  }

  const fontWeight = optionalNumber(first(style, "fontWeight", "font_weight"));
  const explicitFontStyle = optionalString(first(style, "fontStyle", "font_style"));
  const fontFamily = optionalString(first(style, "fontFamily", "font_family"));
  const fontStyle = resolveFontStyle(
    fontFamily,
    fontWeight,
    explicitFontStyle,
    fontCatalog,
  );
  const textTransform = optionalString(first(style, "textTransform", "text_transform"));
  const textCase = textTransform === "uppercase" ? "UPPER"
    : textTransform === "lowercase" ? "LOWER"
    : textTransform === "capitalize" ? "TITLE"
    : textTransform === undefined || textTransform === "none" ? undefined
    : (() => { throw new Error(`${path}.style.textTransform: "${textTransform}" is unsupported.`); })();
  const textAlign = optionalString(first(style, "textAlign", "text_align"));
  const textAlignHorizontal = textAlign === "left" || textAlign === undefined ? "LEFT"
    : textAlign === "center" ? "CENTER"
    : textAlign === "right" ? "RIGHT"
    : textAlign === "justify" ? "JUSTIFIED"
    : (() => { throw new Error(`${path}.style.textAlign: "${textAlign}" is unsupported.`); })();
  const verticalAlign = optionalString(first(style, "verticalAlign", "vertical_align"));
  const textAlignVertical = verticalAlign === "top" || verticalAlign === undefined ? "TOP"
    : verticalAlign === "middle" || verticalAlign === "center" ? "CENTER"
    : verticalAlign === "bottom" ? "BOTTOM"
    : (() => { throw new Error(`${path}.style.verticalAlign: "${verticalAlign}" is unsupported.`); })();

  const justify = align(layout.justify, `${path}.layout.justify`, "primary");
  const counter = align(layout.align, `${path}.layout.align`, "counter");
  const counterAxisStretch =
    display === "flex" &&
    optionalString(layout.align)?.toLowerCase() === "stretch";
  const alignSelf = optionalString(first(layout, "alignSelf", "align_self"));
  const normalizedAlignSelf = alignSelf?.replace(/_/g, "-").toLowerCase();
  const layoutAlign = ({
    auto: "INHERIT",
    inherit: "INHERIT",
    stretch: "STRETCH",
    start: "MIN",
    "flex-start": "MIN",
    center: "CENTER",
    end: "MAX",
    "flex-end": "MAX",
  } as Record<string, ForgeFigmaPlanNode["layoutAlign"]>)[normalizedAlignSelf ?? ""];
  if (alignSelf && !layoutAlign) {
    throw new Error(`${path}.layout.alignSelf: "${alignSelf}" has no Figma layoutAlign equivalent.`);
  }
  const alignContent = optionalString(first(layout, "alignContent", "align_content"));
  if (alignContent && alignContent !== "stretch" && alignContent !== "space-between") {
    throw new Error(`${path}.layout.alignContent: only stretch/space-between map to Figma wrapped auto-layout.`);
  }
  if (layout.wrap === true && direction !== "row") {
    throw new Error(`${path}.layout.wrap: Figma only supports wrapping horizontal auto-layout frames.`);
  }
  const mainGap = optionalNumber(layout.gap) ?? optionalNumber(direction === "row"
    ? first(layout, "columnGap", "column_gap")
    : first(layout, "rowGap", "row_gap"));
  const crossGap = optionalNumber(direction === "row"
    ? first(layout, "rowGap", "row_gap")
    : first(layout, "columnGap", "column_gap"));
  if ((crossGap ?? 0) !== 0 && layout.wrap !== true) {
    throw new Error(`${path}.layout: rowGap/columnGap requires wrap=true in Figma auto-layout.`);
  }
  const opacity = optionalNumber(style.opacity);
  if (opacity !== undefined && (opacity < 0 || opacity > 1)) {
    throw new Error(`${path}.style.opacity: expected a value between 0 and 1.`);
  }
  const overflow = optionalString(style.overflow);
  if (overflow && overflow !== "clip" && overflow !== "hidden" && overflow !== "visible") {
    throw new Error(`${path}.style.overflow: "${overflow}" has no Figma frame equivalent.`);
  }
  const layoutGrow = optionalNumber(first(layout, "flexGrow", "flex_grow"));
  if (layoutGrow !== undefined && layoutGrow !== 0 && layoutGrow !== 1) {
    throw new Error(
      `${path}.layout.flexGrow: Figma supports only 0 or 1; import refused before mutation.`,
    );
  }
  const borderWidths = [
    first(style, "borderTopWidth", "border_top_width"),
    first(style, "borderRightWidth", "border_right_width"),
    first(style, "borderBottomWidth", "border_bottom_width"),
    first(style, "borderLeftWidth", "border_left_width"),
  ];
  if ((kind === "ellipse" || kind === "svg") &&
      borderWidths.some((value) => value !== undefined)) {
    throw new Error(
      `${path}.style: per-side strokes cannot be represented on ${kind} nodes; import refused before mutation.`,
    );
  }
  const borderShorthand = optionalString(style.border);
  if (borderShorthand &&
      (first(style, "borderWidth", "border_width") === undefined ||
       first(style, "borderColor", "border_color") === undefined ||
       first(style, "borderStyle", "border_style") === undefined)) {
    throw new Error(
      `${path}.style.border: canonical copy-back requires the discrete borderWidth, borderColor, and borderStyle fields beside the shorthand.`,
    );
  }
  const sideColorValues = [
    first(style, "borderTopColor", "border_top_color"),
    first(style, "borderRightColor", "border_right_color"),
    first(style, "borderBottomColor", "border_bottom_color"),
    first(style, "borderLeftColor", "border_left_color"),
  ];
  let borderColor = parseForgeColor(
    first(style, "borderColor", "border_color"),
    `${path}.style.borderColor`,
  );
  const uniformBorderWidth = optionalNumber(
    first(style, "borderWidth", "border_width"),
  ) ?? 0;
  for (let i = 0; i < sideColorValues.length; i++) {
    const effectiveWidth = optionalNumber(borderWidths[i]) ?? uniformBorderWidth;
    if (effectiveWidth <= 0 || sideColorValues[i] === undefined) continue;
    const sideColor = parseForgeColor(sideColorValues[i], `${path}.style.borderSideColor[${i}]`)!;
    if (!borderColor) borderColor = sideColor;
    else if (JSON.stringify(sideColor) !== JSON.stringify(borderColor)) {
      throw new Error(
        `${path}.style: Figma box strokes cannot represent different colors per side; import refused before mutation.`,
      );
    }
  }
  const hasStroke = [
    first(style, "borderWidth", "border_width"),
    ...borderWidths,
  ].some((value) => (optionalNumber(value) ?? 0) > 0);
  if (kind === "text" && hasStroke) {
    throw new Error(
      `${path}.style.borderWidth: text box borders require a wrapper frame and cannot be losslessly materialized; import refused before mutation.`,
    );
  }
  if (hasStroke && !borderColor) {
    throw new Error(`${path}.style.borderColor: a visible stroke width requires a color.`);
  }
  const effects = parseEffects(style, `${path}.style`);
  const hasSpread = effects.some((effect) =>
    (effect.kind === "drop-shadow" || effect.kind === "inner-shadow") &&
    (effect.spread ?? 0) !== 0);
  const spreadTargetSupported =
    kind === "rectangle" || kind === "ellipse" ||
    (kind === "frame" && (overflow === "clip" || overflow === "hidden") &&
     !!(backgroundColor || backgroundGradient || asset));
  if (hasSpread && !spreadTargetSupported) {
    throw new Error(
      `${path}.style.boxShadow: nonzero spread is unsupported on this Figma target; import refused before mutation.`,
    );
  }
  const borderStyle = optionalString(first(style, "borderStyle", "border_style"));
  if (borderStyle && borderStyle !== "solid" && borderStyle !== "dashed") {
    throw new Error(
      `${path}.style.borderStyle: "${borderStyle}" has no lossless Figma box-stroke representation; import refused before mutation.`,
    );
  }
  let dashPattern: number[] | undefined;
  if (borderStyle === "dashed") {
    const preserved = optionalString(attrs["figma:dash_pattern"]);
    dashPattern = preserved ? preserved.split(",").map((part) => Number(part.trim())) : [4, 4];
    if (dashPattern.length === 0 ||
        dashPattern.some((part) => !Number.isFinite(part) || part <= 0)) {
      throw new Error(`${path}.attributes.figma:dash_pattern: expected positive comma-separated numbers.`);
    }
  }

  const preservedChildren =
    renderMode === "faithful_svg" && childInputs.length > 0
      ? (() => {
        if (childInputs.some(hasManifestAssetReference)) {
          throw new Error(
            `${path}.children: faithful semantic subtrees with asset references ` +
            "cannot retain their manifest dependencies in Figma; import refused before mutation.",
          );
        }
        return childInputs;
      })()
      : undefined;
  const plan: ForgeFigmaPlanNode = {
    kind,
    svgVisualMode: kind === "svg"
      ? renderMode === "faithful_svg" ? "faithful"
        : inlineVector ? "inline" : "asset"
      : undefined,
    sourceType: type,
    attributes: preservedAttributes,
    name: optionalString(node.name) ?? optionalString(node.label) ??
      (audioWidget && audioWidget !== "none" ? `Pulp / ${audioWidget}` : type),
    content: typeof node.content === "string" ? node.content : undefined,
    x: finiteNumber(first(style, "left"), 0),
    y: finiteNumber(first(style, "top"), 0),
    width: Math.max(1, finiteNumber(first(style, "width"), kind === "text" ? 120 : 100)),
    height: Math.max(1, finiteNumber(first(style, "height"), kind === "text" ? 24 : 100)),
    backgroundColor,
    backgroundGradient,
    asset,
    assetRole: styleBackgroundImage ||
        selectedAttributeAssetKey === "backgroundImageAssetId"
      ? "background"
      : asset ? "node" : undefined,
    imageScaleMode,
    textColor: parseForgeColor(style.color, `${path}.style.color`),
    fontFamily,
    fontStyle,
    fontSize: optionalNumber(first(style, "fontSize", "font_size")),
    fontWeight,
    textAlignHorizontal,
    textAlignVertical,
    letterSpacing: optionalNumber(first(style, "letterSpacing", "letter_spacing")),
    lineHeight: optionalNumber(first(style, "lineHeight", "line_height")),
    textCase,
    textDecoration: textDecoration(first(style, "textDecoration", "text_decoration"), `${path}.style.textDecoration`),
    textRuns: parseTextRuns(
      node,
      path,
      fontFamily,
      fontWeight,
      explicitFontStyle,
      fontCatalog,
    ),
    opacity,
    blendMode: parseBlendMode(first(style, "mixBlendMode", "mix_blend_mode"), `${path}.style.mixBlendMode`),
    effects,
    clipsContent: overflow === "clip" || overflow === "hidden",
    borderColor: inlineVector ? undefined : borderColor,
    borderWidth: inlineVector ? undefined : optionalNumber(first(style, "borderWidth", "border_width")),
    dashPattern: inlineVector ? undefined : dashPattern,
    borderTopWidth: inlineVector ? undefined : optionalNumber(first(style, "borderTopWidth", "border_top_width")),
    borderRightWidth: inlineVector ? undefined : optionalNumber(first(style, "borderRightWidth", "border_right_width")),
    borderBottomWidth: inlineVector ? undefined : optionalNumber(first(style, "borderBottomWidth", "border_bottom_width")),
    borderLeftWidth: inlineVector ? undefined : optionalNumber(first(style, "borderLeftWidth", "border_left_width")),
    cornerRadius: optionalNumber(first(style, "borderRadius", "border_radius")),
    topLeftRadius: optionalNumber(first(style, "borderTopLeftRadius", "border_top_left_radius")),
    topRightRadius: optionalNumber(first(style, "borderTopRightRadius", "border_top_right_radius")),
    bottomRightRadius: optionalNumber(first(style, "borderBottomRightRadius", "border_bottom_right_radius")),
    bottomLeftRadius: optionalNumber(first(style, "borderBottomLeftRadius", "border_bottom_left_radius")),
    // Canonical DesignIR serializes a direction default on ordinary frames,
    // so only the explicit display:flex claim authorizes Figma auto-layout.
    layoutMode: display === "flex"
      ? direction === "column" ? "VERTICAL" : "HORIZONTAL"
      : undefined,
    itemSpacing: mainGap,
    counterAxisSpacing: crossGap,
    paddingTop: optionalNumber(
      first(layout, "paddingTop", "padding_top") ?? padding.top ?? paddingScalar,
    ),
    paddingRight: optionalNumber(
      first(layout, "paddingRight", "padding_right") ?? padding.right ?? paddingScalar,
    ),
    paddingBottom: optionalNumber(
      first(layout, "paddingBottom", "padding_bottom") ?? padding.bottom ?? paddingScalar,
    ),
    paddingLeft: optionalNumber(
      first(layout, "paddingLeft", "padding_left") ?? padding.left ?? paddingScalar,
    ),
    primaryAxisAlignItems: justify,
    counterAxisAlignItems: counter as "MIN" | "MAX" | "CENTER" | undefined,
    counterAxisStretch,
    counterAxisAlignContent: layout.wrap === true
      ? alignContent === "space-between" ? "SPACE_BETWEEN"
        : alignContent === "stretch" ? "AUTO" : undefined
      : undefined,
    layoutWrap: layout.wrap === true ? "WRAP" : "NO_WRAP",
    layoutSizingHorizontal: requestedHorizontalSizing,
    layoutSizingVertical: requestedVerticalSizing,
    layoutGrow,
    layoutAlign,
    layoutPositioning: position === "absolute" ? "ABSOLUTE" : undefined,
    minWidth: sizeConstraint(
      first(style, "minWidth", "min_width"),
      `${path}.style.minWidth`,
      "min",
    ),
    minHeight: sizeConstraint(
      first(style, "minHeight", "min_height"),
      `${path}.style.minHeight`,
      "min",
    ),
    maxWidth: sizeConstraint(
      first(style, "maxWidth", "max_width"),
      `${path}.style.maxWidth`,
      "max",
    ),
    maxHeight: sizeConstraint(
      first(style, "maxHeight", "max_height"),
      `${path}.style.maxHeight`,
      "max",
    ),
    aspectRatio: sizeConstraint(
      layout.aspectRatio,
      `${path}.layout.aspectRatio`,
      "max",
    ),
    constraints: Object.keys(constraintsObj).length > 0 ? {
      horizontal: constraint(constraintsObj.horizontal, `${path}.layout.constraints.horizontal`, "horizontal"),
      vertical: constraint(constraintsObj.vertical, `${path}.layout.constraints.vertical`, "vertical"),
    } : undefined,
    audioWidget,
    audioLabel: optionalString(node.label),
    audioMin: optionalNumber(node.min),
    audioMax: optionalNumber(node.max),
    audioDefault: optionalNumber(node.default),
    audioUnits: optionalString(attrs.units),
    binding: optionalString(attrs.binding) ?? optionalString(attrs.pulpParamKey),
    bindingY: optionalString(attrs.binding_y) ?? optionalString(attrs.pulpParamKeyY),
    sourceNodeId: optionalString(node.source_node_id) ?? optionalString(node.sourceNodeId) ??
      optionalString(attrs["pulp:roundtrip_source_node_id"]),
    stableAnchorId: optionalString(node.stable_anchor_id) ?? optionalString(node.stableAnchorId) ??
      optionalString(attrs["pulp:roundtrip_stable_anchor_id"]),
    interactiveElements,
    // A faithful SVG already contains the retained semantic subtree's pixels.
    // Keep the SVG authoritative instead of painting those layers a second time.
    children,
    preservedChildren,
    alternateFrames,
  };
  preflightPlanPluginData(plan, path);
  return plan;
}

export function decodeForgeDesignForFigma(text: string): ForgeFigmaPlan {
  if (utf8ByteLength(text) > FORGE_CLIPBOARD_MAX_BYTES) {
    throw new Error(`Clipboard payload exceeds the ${FORGE_CLIPBOARD_MAX_BYTES}-byte limit.`);
  }
  let parsed: Partial<ForgeToFigmaClipboard>;
  try {
    parsed = JSON.parse(text) as Partial<ForgeToFigmaClipboard>;
  } catch {
    throw new Error("Clipboard is not valid JSON.");
  }
  if (parsed.format !== FORGE_CLIPBOARD_FORMAT || parsed.kind !== "design-ir") {
    throw new Error(`Clipboard does not contain a ${FORGE_CLIPBOARD_FORMAT} DesignIR payload.`);
  }
  if (typeof parsed.design_ir_json !== "string") {
    throw new Error("Clipboard DesignIR payload is missing design_ir_json.");
  }
  let design: Record<string, unknown>;
  try {
    design = JSON.parse(parsed.design_ir_json) as Record<string, unknown>;
  } catch {
    throw new Error("Clipboard design_ir_json is not valid JSON.");
  }
  if (!design.root) throw new Error("Clipboard DesignIR document has no root.");
  const assets = parseAssets(design, parsed.assets);
  const rawFontCatalog = design.fontFamilyAssets ?? design.font_family_assets;
  if (rawFontCatalog !== undefined && !Array.isArray(rawFontCatalog)) {
    throw new Error("$.design_ir.fontFamilyAssets: expected an array.");
  }
  const fontCatalog = (Array.isArray(rawFontCatalog) ? rawFontCatalog : []).map(
    (raw, index): ForgeFontCatalogEntry => {
      const entry = record(raw);
      const family = optionalString(entry.family);
      const style = optionalString(entry.style) ?? "normal";
      if (!family) {
        throw new Error(`$.design_ir.fontFamilyAssets[${index}]: family is required.`);
      }
      return {
        family,
        style,
        weight: optionalNumber(entry.weight),
        italic: typeof entry.italic === "boolean" ? entry.italic : undefined,
        assetId: optionalString(entry.asset_id) ?? optionalString(entry.assetId),
      };
    },
  );
  const catalogFacesByTuple = new Map<string, Set<string>>();
  for (const entry of fontCatalog) {
    const weight = entry.weight ??
      (/^(normal|regular|italic|oblique)$/i.test(entry.style) ? 400 : undefined);
    const italic = entry.italic ?? /italic|oblique/i.test(entry.style);
    const tuple = `${entry.family}|${weight ?? ""}|${italic}`;
    const faces = catalogFacesByTuple.get(tuple) ?? new Set<string>();
    faces.add(entry.style);
    catalogFacesByTuple.set(tuple, faces);
  }
  const assertUnambiguousSemanticFace = (
    family: string | undefined,
    weight: number | undefined,
    explicit: string | undefined,
    path: string,
  ): void => {
    if (!family) return;
    const normalized = explicit?.toLowerCase();
    if (normalized && normalized !== "normal" && normalized !== "italic") return;
    const tuple = `${family}|${weight ?? 400}|${normalized === "italic"}`;
    const faces = catalogFacesByTuple.get(tuple);
    if (faces && faces.size > 1) {
      throw new Error(
        `${path}: font catalog faces ${Array.from(faces).map((face) => `"${face}"`).join(" and ")} ` +
        `collapse to the same ${family} weight/style tuple; import refused before mutation.`,
      );
    }
  };
  const validateFontClaims = (raw: unknown, path: string): void => {
    const node = record(raw);
    const style = record(node.style);
    const family = optionalString(first(style, "fontFamily", "font_family"));
    const weight = optionalNumber(first(style, "fontWeight", "font_weight"));
    const explicit = optionalString(first(style, "fontStyle", "font_style"));
    const type = (optionalString(node.type) ?? "").toLowerCase();
    if (type === "text" || type === "label") {
      assertUnambiguousSemanticFace(family, weight, explicit, `${path}.style`);
      const runs = Array.isArray(node.runs) ? node.runs
        : Array.isArray(node.textRuns) ? node.textRuns : [];
      for (const [index, rawRun] of runs.entries()) {
        const run = record(rawRun);
        const runWeight = optionalNumber(run.fontWeight);
        const runStyle = optionalString(run.fontStyle);
        if (runWeight !== undefined || runStyle !== undefined) {
          assertUnambiguousSemanticFace(
            family,
            runWeight ?? weight,
            runStyle ?? explicit,
            `${path}.runs[${index}]`,
          );
        }
      }
    }
    for (const [field, children] of [
      ["children", node.children],
      ["alternate_frames", node.alternate_frames ?? node.alternateFrames],
    ] as const) {
      if (!Array.isArray(children)) continue;
      children.forEach((child, index) =>
        validateFontClaims(child, `${path}.${field}[${index}]`));
    }
  };
  validateFontClaims(design.root, "$.design_ir.root");
  fontCatalog.forEach((entry, index) => {
    if (!entry.assetId) return [];
    const asset = assetFor(
      entry.assetId,
      assets,
      `$.design_ir.fontFamilyAssets[${index}].asset_id`,
    );
    if (!asset || !asset.mime.startsWith("font/")) {
      throw new Error(
        `$.design_ir.fontFamilyAssets[${index}].asset_id: bundled font requires a font/* asset.`,
      );
    }
    const bytes = asset.bytes;
    const magic = bytes.length >= 4
      ? String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3])
      : "";
    const detectedMime = bytes[0] === 0x00 && bytes[1] === 0x01 &&
        bytes[2] === 0x00 && bytes[3] === 0x00 || magic === "true"
      ? "font/ttf"
      : magic === "OTTO" ? "font/otf"
      : magic === "wOFF" ? "font/woff"
      : magic === "wOF2" ? "font/woff2"
      : undefined;
    if (!detectedMime || detectedMime !== asset.mime) {
      throw new Error(
        `$.design_ir.fontFamilyAssets[${index}].asset_id: declared MIME ` +
        `"${asset.mime}" does not match a supported SFNT/WOFF font signature; ` +
        "import refused before mutation.",
      );
    }
    throw new Error(
      `$.design_ir.fontFamilyAssets[${index}].asset_id: bundled font bytes ` +
      "cannot be persisted durably by the Figma plugin across sessions; " +
      "install the face in Figma and omit the bundled asset, or import is refused before mutation.",
    );
  });
  const bundledFonts: ForgeFigmaPlan["bundledFonts"] = [];
  const counters = { nodes: 0, widgets: 0 };
  const root = toPlanNode(
    design.root,
    counters,
    assets,
    "$.design_ir.root",
    fontCatalog,
  );
  if (root.alternateFrames.length > 0 &&
      (root.svgVisualMode !== "faithful" ||
       root.alternateFrames.some((frame) => frame.svgVisualMode !== "faithful"))) {
    throw new Error(
      "$.design_ir.root.alternate_frames: every state, including the primary, must use render_mode faithful_svg.",
    );
  }
  return {
    root,
    nodeCount: counters.nodes,
    audioWidgetCount: counters.widgets,
    assetCount: assets.size,
    bundledFonts,
  };
}
