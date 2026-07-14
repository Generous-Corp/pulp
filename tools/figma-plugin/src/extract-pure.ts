// Pure, host-neutral helpers shared by the UI plugin and headless extractor.
//
// Every function here operates on types only (SceneNode shape, RGBA,
// Paint, GradientPaint, FrameNode axis enums) — no `await
// figma.X.Async()`, no `getBytesAsync`, no `exportAsync`. Anything
// pulling bytes through the Figma Plugin API stays in `extract.ts`.
//
// The extractor has two real consumers — the UI plugin (`code.ts`) and the
// headless bundle (`headless.ts`) — plus a structural neighbor, the Python REST
// port at `tools/import-design/figma_rest_export.py`, which mirrors these
// helpers field-for-field. Co-locating the pure logic makes drift between the
// language ports visible in one file and reduces the surface area future
// provider abstractions (`NodeProvider`, `AssetProvider`, etc.) have to thread
// through.

import type { ExtractedFigmaNode, ExtractedLayout } from "./extract-model";
import type { AudioWidgetKind } from "./extract-model";
import type { FontFamilyAsset } from "./extract";

// ──────────────────────────────────────────────────────────────────────────
// Color helpers — convert Figma Paint / RGBA shapes into CSS strings.

export function paintToColor(p: SolidPaint): string {
  const c = p.color;
  const a = p.opacity !== undefined ? p.opacity : 1;
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  if (a >= 1) return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  return `rgba(${r}, ${g}, ${b}, ${a.toFixed(3)})`;
}

export function rgbaToCss(c: RGBA): string {
  const r = Math.round(c.r * 255);
  const g = Math.round(c.g * 255);
  const b = Math.round(c.b * 255);
  if (c.a === undefined || c.a >= 1) {
    return `#${hex2(r)}${hex2(g)}${hex2(b)}`;
  }
  return `rgba(${r}, ${g}, ${b}, ${c.a.toFixed(3)})`;
}

export function hex2(n: number): string {
  return n.toString(16).padStart(2, "0");
}

export function gradientToCss(p: GradientPaint): string {
  if (!p.gradientStops || p.gradientStops.length === 0) return "linear-gradient(transparent, transparent)";
  // Pulp's setBackgroundGradient bridge takes color stop positions implicitly by
  // index, and its parseColor doesn't strip trailing `Npc%` from a token.
  // Emit colors only (no inline percentages).
  const stops = p.gradientStops.map((s) => rgbaToCss(s.color)).join(", ");
  return `linear-gradient(to bottom, ${stops})`;
}

export function gradientFallbackFlat(p: GradientPaint): string {
  const first = p.gradientStops?.[0]?.color;
  if (!first) return "transparent";
  return rgbaToCss(first);
}

// ──────────────────────────────────────────────────────────────────────────
// Type and layout mapping — Plugin API enums → envelope strings.

export function mapNodeType(n: SceneNode): string {
  switch (n.type) {
    case "FRAME":
    case "GROUP":
    case "SECTION":
      return "frame";
    case "COMPONENT":
    case "COMPONENT_SET":
      return "frame"; // recognized instances are promoted to widget kinds later
    case "INSTANCE":
      return "frame"; // ditto
    case "TEXT":
      return "text";
    case "RECTANGLE":
    case "ELLIPSE":
    case "POLYGON":
    case "STAR":
    case "LINE":
      return "frame";
    case "VECTOR":
    case "BOOLEAN_OPERATION":
      return "vector";
    case "SLICE":
      return "frame";
    default:
      return "frame";
  }
}

export function mapPrimaryAxisAlign(v: FrameNode["primaryAxisAlignItems"]): ExtractedLayout["justify"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "SPACE_BETWEEN": return "space_between";
    default: return "flex_start";
  }
}

export function mapCounterAxisAlign(v: FrameNode["counterAxisAlignItems"]): ExtractedLayout["align"] {
  switch (v) {
    case "MIN": return "flex_start";
    case "MAX": return "flex_end";
    case "CENTER": return "center";
    case "BASELINE": return "flex_start"; // Pulp Yoga doesn't model baseline; closest fallback
    default: return "stretch";
  }
}

export function mapAxisSize(v: FrameNode["layoutSizingHorizontal"]): ExtractedLayout["width_mode"] {
  switch (v) {
    case "HUG": return "hug";
    case "FILL": return "fill";
    case "FIXED":
    default: return "fixed";
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Library recognition fallback — name-based heuristic when the
// component_set_key match doesn't hit (e.g. unpublished local
// components, or designs that use the audio widget visual without
// installing the published library).

// Whole-word tokenizer mirroring the C++ tokenize_name (design_import.cpp): split
// on non-alphanumerics AND camelCase / acronym / digit boundaries, lowercased.
// "VUMeter" -> [vu, meter]; "Knob_1" -> [knob, 1]; "Dialog" -> [dialog].
export function tokenizeName(name: string): string[] {
  const tokens: string[] = [];
  let cur = "";
  const flush = () => { if (cur) { tokens.push(cur); cur = ""; } };
  for (let i = 0; i < name.length; i++) {
    const c = name.charAt(i);
    if (!/[a-z0-9]/i.test(c)) { flush(); continue; }
    if (cur) {
      const p = name.charAt(i - 1);
      const next = i + 1 < name.length ? name.charAt(i + 1) : "";
      let boundary = false;
      if (/[a-z]/.test(p) && /[A-Z]/.test(c)) boundary = true;                            // aB -> a|B
      else if (/[A-Z]/.test(p) && /[A-Z]/.test(c) && /[a-z]/.test(next)) boundary = true; // ABc -> A|Bc
      else if (/[0-9]/.test(p) !== /[0-9]/.test(c)) boundary = true;                      // a1 / 1a
      if (boundary) flush();
    }
    cur += c.toLowerCase();
  }
  flush();
  return tokens;
}

// Recognize an audio widget by WHOLE-WORD name tokens (not substrings), mirroring
// the C++ detect_audio_widget. The old substring match promoted any name
// *containing* "dial"/"meter"/… — so "Dialog"/"Radial" became knobs and
// "Parameter"/"Diameter" became meters. Token matching (tolerant of a simple
// English plural, as the C++ `has` is) fixes those while keeping "xy_pad"/"XYPad"
// and acronym names like "VUMeter".
export function audioWidgetKindFromName(name: string): AudioWidgetKind | undefined {
  const toks: { [t: string]: true } = {};
  const list = tokenizeName(name);
  for (let i = 0; i < list.length; i++) toks[list[i]] = true;
  const has = (w: string) => toks[w] === true || toks[w + "s"] === true;
  if (has("knob") || has("dial")) return "knob";
  if (has("fader") || has("slider")) return "fader";
  if (has("meter") || has("level") || has("vu")) return "meter";
  if (has("xypad") || (has("xy") && has("pad"))) return "xy_pad";
  if (has("waveform") || has("oscilloscope")) return "waveform";
  if (has("spectrum") || has("analyzer") || has("analyser")) return "spectrum";
  return undefined;
}

// ──────────────────────────────────────────────────────────────────────────
// Vector-illustration detection — heuristic, used to flatten an entire
// shape illustration frame to a single SVG export instead of walking
// each primitive. Recursive but bounded by tree depth.

/// Returns true if the node and all its recursive descendants are
/// vector-like primitives (or empty frames wrapping them).
export function isPureVectorIllustration(node: SceneNode): boolean {
  if (!("children" in node)) return false;
  const children = (node as ChildrenMixin).children;
  if (children.length === 0) return false;
  for (const child of children) {
    const t = child.type;
    if (
      t === "VECTOR" ||
      t === "BOOLEAN_OPERATION" ||
      t === "LINE" ||
      t === "STAR" ||
      t === "POLYGON" ||
      t === "ELLIPSE" ||
      t === "RECTANGLE"
    ) {
      continue;
    }
    if (t === "FRAME" || t === "GROUP") {
      if (!isPureVectorIllustration(child)) return false;
      continue;
    }
    // text, instance, image, anything else → not a pure illustration
    return false;
  }
  return true;
}

// ──────────────────────────────────────────────────────────────────────────
// Font catalog — walks the post-extraction IR (not the Figma scene),
// so it's already host-neutral and operates only on ExtractedFigmaNode
// trees. Emitted as the envelope's top-level `font_family_assets`.

export function collectFontFamilyAssets(roots: ExtractedFigmaNode[]): FontFamilyAsset[] {
  const seen = new Map<string, FontFamilyAsset>();
  function visit(n: ExtractedFigmaNode): void {
    const family = n.style.font_family;
    if (family) {
      // Figma's `fontName.style` (already captured by extractTextStyle)
      // lives on `style.font_style` as either "normal" or "italic" today;
      // the verbose style string ("Semi Bold", "Bold Italic") is not yet
      // surfaced. Emit what we have and let the runtime resolve.
      const styleField = (n.style.font_style as string | undefined) ?? "Regular";
      const weight = n.style.font_weight;
      const italic = styleField === "italic" || /italic/i.test(styleField);
      const key = `${family}|${styleField}|${weight ?? ""}|${italic ? "i" : ""}`;
      if (!seen.has(key)) {
        const row: FontFamilyAsset = { family, style: styleField };
        if (typeof weight === "number") row.weight = weight;
        if (italic) row.italic = true;
        seen.set(key, row);
      }
    }
    for (const c of n.children) visit(c);
  }
  for (const r of roots) visit(r);
  return Array.from(seen.values());
}
