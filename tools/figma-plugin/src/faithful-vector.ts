// Faithful-vector import (Plan B / B4b) — geometry auto-detect of knobs in an
// exported frame SVG, ported from the vector-knob PoC and the C++ DesignFrameView
// convention (kept in lockstep with tools/import-design/figma_rest_export.py's
// parse_frame_knobs). A knob DOME is a gradient-filled <circle> (fill="url(",
// r>=8); its NEEDLE is a thin LIGHT-stroked (white or #ABABAB — dark ticks are
// #506274) short vertical <path d="Mx1 y1Lx2 y2"> just above the dome center.
// Pair each needle to its nearest dome and emit the EXACT `d` so DesignFrameView
// can rotate only that needle while the chrome stays pixel-exact.
//
// Written ES5-conservative (exec loops, indexOf, indexed loops, char-by-char
// decode) — the Figma plugin sandbox tsconfig targets an older lib without
// String.matchAll / includes / spread-of-typed-array.

export interface InteractiveElement {
  kind: string;
  cx: number;
  cy: number;
  hit_radius: number;
  svg_patch_d: string;
  default_value: number;
  source_node_id?: string;
}

const CIRCLE_RE = /<circle\b[^>]*>/g;
const CXR_RE = /cx="([-\d.]+)"\s+cy="([-\d.]+)"\s+r="([-\d.]+)"/;
const PATH_RE = /<path\b[^>]*>/g;
const PATHD_RE = /\bd="(M[^"]*)"/;
const NEEDLE_RE = /^M([-\d.]+) ([-\d.]+)L([-\d.]+) ([-\d.]+)/;

export function parseFrameKnobs(svg: string): InteractiveElement[] {
  const domes: Array<[number, number, number]> = [];
  let m: RegExpExecArray | null;
  CIRCLE_RE.lastIndex = 0;
  while ((m = CIRCLE_RE.exec(svg)) !== null) {
    const tag = m[0];
    const cm = CXR_RE.exec(tag);
    if (!cm) continue;
    const cx = parseFloat(cm[1]);
    const cy = parseFloat(cm[2]);
    const r = parseFloat(cm[3]);
    if (r >= 8 && tag.indexOf('fill="url') !== -1) domes.push([cx, cy, r]);
  }
  const knobs: InteractiveElement[] = [];
  PATH_RE.lastIndex = 0;
  while ((m = PATH_RE.exec(svg)) !== null) {
    const tag = m[0];
    if (tag.indexOf('stroke="white"') === -1 && tag.indexOf('stroke="#ABABAB"') === -1) continue;
    const dm = PATHD_RE.exec(tag);
    if (!dm) continue;
    const d = dm[1];
    const pm = NEEDLE_RE.exec(d);
    if (!pm) continue;
    const x1 = parseFloat(pm[1]);
    const y1 = parseFloat(pm[2]);
    const x2 = parseFloat(pm[3]);
    const y2 = parseFloat(pm[4]);
    if (Math.abs(x1 - x2) > 0.6 || Math.abs(y1 - y2) > 14) continue; // short vertical needle
    const ny = Math.max(y1, y2);
    let best: [number, number, number] | null = null;
    let bd = 1e9;
    for (let i = 0; i < domes.length; i++) {
      const dome = domes[i];
      if (Math.abs(dome[0] - x1) < 1.5 && dome[1] > ny - 2) {
        const dd = Math.abs(dome[1] - ny);
        if (dd < bd) {
          bd = dd;
          best = dome;
        }
      }
    }
    if (best) {
      knobs.push({
        kind: "knob",
        cx: best[0],
        cy: best[1],
        hit_radius: best[2],
        svg_patch_d: d,
        default_value: 0.5,
      });
    }
  }
  return knobs;
}

// The Figma plugin sandbox has no TextDecoder (see assets.ts svgSize). Decode the
// exported SVG bytes manually. SVG markup (paths, colors, numbers) is ASCII, so a
// byte->char map is sufficient for knob detection; chunked to bound string growth.
export function decodeSvgBytes(bytes: Uint8Array): string {
  const CHUNK = 8192;
  const parts: string[] = [];
  for (let i = 0; i < bytes.length; i += CHUNK) {
    const end = Math.min(i + CHUNK, bytes.length);
    let s = "";
    for (let j = i; j < end; j++) s += String.fromCharCode(bytes[j]);
    parts.push(s);
  }
  return parts.join("");
}
