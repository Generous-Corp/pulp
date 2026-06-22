// Faithful-vector import: geometry auto-detect of knobs in an
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

// The resolution self-check records each control's provenance and catches
// name/geometry conflicts. Type-only the other direction, so no cycle.
import { assessResolution } from "./resolve-control";

// The interactive-overlay kinds the schema (figma-plugin-export-v1.json
// interactive_element.kind) accepts and the C++ materializer
// (to_frame_elements) maps to a DesignFrameElement::Kind. Kept a literal union
// — not `string` — so the producer can never emit a kind the schema forbids.
export type InteractiveElementKind =
  | "knob"
  | "fader"
  | "toggle"
  | "dropdown"
  | "text_field"
  | "tab_group"
  | "stepper"
  | "swap"
  | "action"
  | "xy_pad"
  | "value_label"
  | "custom";

export interface InteractiveElement {
  kind: InteractiveElementKind;
  // knob / fader (SVG-patch) fields
  cx?: number;
  cy?: number;
  hit_radius?: number;
  svg_patch_d?: string;
  default_value?: number;
  flash?: boolean;  // toggle: press-flash command button vs sticky on/off flip
  source_node_id?: string;
  // overlay-control (text_field / dropdown / stepper / tab_group / toggle, and
  // the fader track) fields, in SVG coords — kept in lockstep with the REST
  // lane's detect_overlay_controls and the C++ parser (design_ir_json.cpp).
  x?: number;
  y?: number;
  w?: number;
  h?: number;
  options?: string[];
  selected_index?: number;
  placeholder?: string;
  bg_color?: string;  // text_field: the design's own field bg ("#RRGGBB")
  // swap / action / xy_pad / value_label fields, in lockstep with the C++ parser.
  target_frame?: number;   // swap: frame index activated on click
  action?: string;         // action: command id fired on click
  text?: string;           // value_label: initial readout string
  value_left_align?: boolean;  // value_label: left-align the readout
  default_value_y?: number;    // xy_pad: initial normalized Y (0=top)
  // Resolution provenance — carried so a low-confidence or conflicted control is
  // visible at the host materialize boundary, not just in the TS importer.
  resolution_rung?: number;        // which ladder rung resolved this (0=unset..5=inert)
  confidence_score?: number;       // 0..1; 1.0 = unset/legacy
  conflict_signals?: string[];     // cross-signal conflicts (empty = none)
  verification_pass?: boolean;     // render verification passed (default true)
  // Registered custom control.
  factory_id?: string;             // the registered native-overlay factory id
  custom_props?: string;           // opaque props handed to the factory (e.g. JSON)
}

// Minimal structural node shape detectOverlayControls needs. ExtractedFigmaNode
// satisfies it (defined here to avoid a circular import with extract-model).
export interface OverlayNode {
  name: string;
  figma_type: string;          // raw SceneNode.type: "FRAME" | "INSTANCE" | ...
  figma_node_id: string;
  absolute_bounds: { x: number; y: number; w: number; h: number };
  content?: string;            // the node's own text characters, if any
  opacity?: number;            // node opacity (1 when absent) — for the occlusion guard
  style?: { background_color?: string };
  children: OverlayNode[];
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

// ── Native-overlay detection (lockstep with the REST lane) ────────────────────
//
// Ported from tools/import-design/figma_rest_export.py's detect_overlay_controls
// + parse_panel_bounds. Detects search (text_field), dropdown, < > stepper, and
// tab_group controls from the source NODE TREE (names/structure/bounds) — more
// reliable than SVG glyphs (Codex review). Node coords map into the exported
// SVG's space: svg = (node_abs - root_abs) + panel_origin, because the node tree
// is frame-local while the SVG export adds the drop-shadow margin. The output
// shape matches the REST lane + the C++ parser (design_ir_json.cpp) exactly, so
// both import lanes feed DesignFrameView identical overlays.

const PANEL_RECT_RE =
  /<rect x="([-\d.]+)" y="([-\d.]+)" width="([-\d.]+)" height="([-\d.]+)"/g;
const SVG_W_RE = /width="([-\d.]+)"/;
const SVG_H_RE = /height="([-\d.]+)"/;

// The design PANEL = the largest <rect> that is a big fraction of the frame
// (0.15..0.97); its (x,y) is where the frame content sits in SVG space.
export function parsePanelBounds(svg: string): [number, number, number, number] {
  let fw = 0;
  let fh = 0;
  const mw = SVG_W_RE.exec(svg);
  const mh = SVG_H_RE.exec(svg);
  if (mw) fw = parseFloat(mw[1]);
  if (mh) fh = parseFloat(mh[1]);
  const frameArea = fw * fh;
  let best = 0;
  let out: [number, number, number, number] = [0, 0, 0, 0];
  let m: RegExpExecArray | null;
  PANEL_RECT_RE.lastIndex = 0;
  while ((m = PANEL_RECT_RE.exec(svg)) !== null) {
    const x = parseFloat(m[1]);
    const y = parseFloat(m[2]);
    const w = parseFloat(m[3]);
    const h = parseFloat(m[4]);
    const area = w * h;
    if (frameArea > 0) {
      const frac = area / frameArea;
      if (frac < 0.15 || frac > 0.97) continue;
    }
    if (area > best) {
      best = area;
      out = [x, y, w, h];
    }
  }
  return out;
}

// First TEXT descendant's characters, or "" (a field's placeholder).
function firstText(n: OverlayNode): string {
  if (n.figma_type === "TEXT" && n.content) return n.content;
  const kids = n.children || [];
  for (let i = 0; i < kids.length; i++) {
    const t = firstText(kids[i]);
    if (t) return t;
  }
  return "";
}

// The field's own box background ("#RRGGBB"): its own resolved SOLID fill, else
// the first child rect carrying one (e.g. ELYSIUM's box inside the search group).
// "" when none. Mirrors the REST lane's _field_bg_hex (style.background_color is
// the TS lane's already-resolved SOLID fill).
function fieldBgHex(field: OverlayNode): string {
  if (field.style && field.style.background_color) return field.style.background_color;
  const kids = field.children || [];
  for (let i = 0; i < kids.length; i++) {
    const k = kids[i];
    if (k.style && k.style.background_color) return k.style.background_color;
  }
  return "";
}

const OVERLAY_CONTAINER_TYPES = ["FRAME", "INSTANCE", "COMPONENT", "GROUP"];

export function detectOverlayControls(
  root: OverlayNode,
  rootAbs: [number, number],
  panelOrigin: [number, number],
): InteractiveElement[] {
  const rax = rootAbs[0];
  const ray = rootAbs[1];
  const pox = panelOrigin[0];
  const poy = panelOrigin[1];

  function toSvg(bb: { x: number; y: number; w: number; h: number }):
    [number, number, number, number] {
    return [bb.x - rax + pox, bb.y - ray + poy, bb.w, bb.h];
  }

  const out: InteractiveElement[] = [];

  // ── Occlusion guard (lockstep with figma_rest_export.py) ──────────────────
  // A control fully painted over by a later (higher-z) OPAQUE node is not
  // visible and must NOT become an overlay (else a buried layer — e.g. a
  // leftover radio strip under an envelope graph — gets resurfaced). Paint order
  // = document preorder; only a node painted AFTER the candidate's ENTIRE
  // subtree can occlude it (so a node's own background rect / descendants never
  // count). Opaque proxy: the node has a resolved background_color (the
  // extractor sets this for SOLID and the flat fallback of GRADIENT fills) and
  // is not itself faded. This mirrors the REST detector's _opaque_cover.
  function opaqueCover(n: OverlayNode): boolean {
    if (n.opacity != null && n.opacity < 0.99) return false;
    return !!(n.style && n.style.background_color);
  }
  const paintIndex = new Map<OverlayNode, number>();
  const subtreeEnd = new Map<OverlayNode, number>();
  const occluders: Array<[number, number, number, number, number]> = [];
  let _counter = 0;
  // node id -> name, so the post-pass can read the name signal for each emitted
  // control (which only carries its source_node_id).
  const nodeNameById: { [id: string]: string } = {};
  function scan(n: OverlayNode): number {
    const idx = _counter++;
    if (n.figma_node_id) nodeNameById[n.figma_node_id] = n.name || "";
    paintIndex.set(n, idx);
    const b = n.absolute_bounds;
    if (b && opaqueCover(n)) occluders.push([idx, b.x, b.y, b.x + b.w, b.y + b.h]);
    let last = idx;
    const kids = n.children || [];
    for (let i = 0; i < kids.length; i++) last = scan(kids[i]);
    subtreeEnd.set(n, last);
    return last;
  }
  scan(root);
  function occluded(n: OverlayNode): boolean {
    const b = n.absolute_bounds;
    if (!b) return false;
    const after = subtreeEnd.has(n) ? subtreeEnd.get(n)!
                                    : (paintIndex.has(n) ? paintIndex.get(n)! : -1);
    const cx0 = b.x, cy0 = b.y, cx1 = b.x + b.w, cy1 = b.y + b.h, eps = 0.5;
    for (let i = 0; i < occluders.length; i++) {
      const o = occluders[i];
      if (o[0] <= after) continue;
      if (o[1] - eps <= cx0 && o[2] - eps <= cy0 && o[3] + eps >= cx1 && o[4] + eps >= cy1) {
        return true;
      }
    }
    return false;
  }

  // A tab/segmented control = a horizontal row of >=3 container children, each
  // with a short text label, of similar width; the selected tab carries a
  // visible SOLID fill (here: a resolved background_color).
  function detectTabGroup(n: OverlayNode): InteractiveElement | null {
    const tabs: Array<{ node: OverlayNode; label: string;
                        bb: { x: number; y: number; w: number; h: number } }> = [];
    const kids = n.children || [];
    for (let i = 0; i < kids.length; i++) {
      const c = kids[i];
      const cb = c.absolute_bounds;
      if (!cb || OVERLAY_CONTAINER_TYPES.indexOf(c.figma_type) === -1) continue;
      const label = firstText(c);
      if (label && label.length <= 3) tabs.push({ node: c, label: label, bb: cb });
    }
    if (tabs.length < 3) return null;
    let minY = Infinity;
    let maxY = -Infinity;
    let minW = Infinity;
    let maxW = -Infinity;
    for (let i = 0; i < tabs.length; i++) {
      const cb = tabs[i].bb;
      if (cb.y < minY) minY = cb.y;
      if (cb.y > maxY) maxY = cb.y;
      if (cb.w < minW) minW = cb.w;
      if (cb.w > maxW) maxW = cb.w;
    }
    if (maxY - minY > 6) return null;                 // one horizontal row
    if (minW <= 0 || maxW / minW > 1.6) return null;  // similar-width slots
    tabs.sort((a, b) => a.bb.x - b.bb.x);             // left→right
    let selected = 0;
    for (let i = 0; i < tabs.length; i++) {
      const bg = tabs[i].node.style && tabs[i].node.style!.background_color;
      if (bg) selected = i;                            // last filled wins (mirrors REST)
    }
    let x0 = Infinity;
    let y0 = Infinity;
    let x1 = -Infinity;
    let y1 = -Infinity;
    const options: string[] = [];
    for (let i = 0; i < tabs.length; i++) {
      const cb = tabs[i].bb;
      if (cb.x < x0) x0 = cb.x;
      if (cb.y < y0) y0 = cb.y;
      if (cb.x + cb.w > x1) x1 = cb.x + cb.w;
      if (cb.y + cb.h > y1) y1 = cb.y + cb.h;
      options.push(tabs[i].label);
    }
    const r = toSvg({ x: x0, y: y0, w: x1 - x0, h: y1 - y0 });
    return {
      kind: "tab_group",
      x: r[0], y: r[1], w: r[2], h: r[3],
      options: options,
      selected_index: selected,
      source_node_id: n.figma_node_id || "",
    };
  }

  function visit(n: OverlayNode, parent: OverlayNode | null): void {
    // Painted-over (occluded) subtrees hold no visible controls — skip the whole
    // subtree so a buried layer never becomes an interactive overlay.
    if (occluded(n)) return;
    const name = (n.name || "").toLowerCase();
    const ntype = n.figma_type || "";
    const bb = n.absolute_bounds;

    // ── tab group ──────────────────────────────────────────────────────────
    const tg = detectTabGroup(n);
    if (tg) {
      out.push(tg);
      return;  // owned by the overlay — don't recurse
    }

    // ── search (text_field) ────────────────────────────────────────────────
    // Named ~"search" (not the "ic*"/"icon*" magnifier). When the match is the
    // placeholder TEXT, the field is its parent group; the overlay is INSET past
    // the leading icon (start at the text's x) so the baked magnifier stays
    // visible, and carries the field's own bg color so the inset edge is seamless.
    const isSearch =
      name.indexOf("search") !== -1 &&
      name.indexOf("ic") !== 0 && name.indexOf("icon") !== 0;
    if (bb && isSearch) {
      const useParent = ntype === "TEXT" && !!parent && !!parent.absolute_bounds;
      const field = useParent ? (parent as OverlayNode) : n;
      const fbb = field.absolute_bounds;
      let ox = fbb.x;
      let ow = fbb.w;
      if (useParent && bb.x > fbb.x) {  // inset left to the text's x (past the icon)
        ox = bb.x;
        ow = fbb.x + fbb.w - bb.x;
      }
      const f = toSvg({ x: ox, y: fbb.y, w: ow, h: fbb.h });
      const el: InteractiveElement = {
        kind: "text_field",
        x: f[0], y: f[1], w: f[2], h: f[3],
        placeholder: firstText(n) || n.name || "Search",
        source_node_id: field.figma_node_id || "",
      };
      const bgc = fieldBgHex(field);
      if (bgc) el.bg_color = bgc;
      out.push(el);
      return;  // leaf overlay
    }

    // ── dropdown ───────────────────────────────────────────────────────────
    const current = firstText(n) || "Select";
    const kids = n.children || [];
    let hasDownChevron = false;
    for (let i = 0; i < kids.length; i++) {
      if ((kids[i].name || "").toLowerCase().indexOf("expand_more") === 0) {
        hasDownChevron = true;
        break;
      }
    }
    const sized = !!bb && bb.w >= 40 && bb.h >= 14 && bb.h <= 44;
    const isDropdown =
      name.indexOf("dropdown") !== -1 && ntype === "FRAME" && sized &&
      hasDownChevron && current !== "Dropdown";
    if (isDropdown && bb) {
      const d = toSvg(bb);
      out.push({
        kind: "dropdown",
        x: d[0], y: d[1], w: d[2], h: d[3],
        options: [current],  // only the real shown value (no fabricated stubs)
        selected_index: 0,
        source_node_id: n.figma_node_id || "",
      });
      return;  // leaf overlay
    }

    // ── stepper (< > header preset selector) ──────────────────────────────
    // Same "Dropdown"-named FRAME family, but its chevron child is a < > PAIR
    // ("Frame 41", or an explicit left+right chevron pair), not a down-chevron.
    let hasStepperPair = false;
    for (let i = 0; i < kids.length; i++) {
      if ((kids[i].name || "").toLowerCase().indexOf("frame 41") === 0) {
        hasStepperPair = true;
        break;
      }
    }
    if (!hasStepperPair) {
      const lefts = ["chevron_left", "navigate_before", "keyboard_arrow_left",
                     "arrow_left", "arrow_back"];
      const rights = ["chevron_right", "navigate_next", "keyboard_arrow_right",
                      "arrow_right", "arrow_forward"];
      let hasLeft = false;
      let hasRight = false;
      for (let i = 0; i < kids.length; i++) {
        const cn = (kids[i].name || "").toLowerCase();
        for (let j = 0; j < lefts.length; j++) if (cn.indexOf(lefts[j]) !== -1) hasLeft = true;
        for (let j = 0; j < rights.length; j++) if (cn.indexOf(rights[j]) !== -1) hasRight = true;
      }
      hasStepperPair = hasLeft && hasRight;
    }
    const isStepper =
      name.indexOf("dropdown") !== -1 && ntype === "FRAME" && sized &&
      !hasDownChevron && hasStepperPair && current !== "Dropdown";
    if (isStepper && bb) {
      const s = toSvg(bb);
      out.push({
        kind: "stepper",
        x: s[0], y: s[1], w: s[2], h: s[3],
        options: [current],  // real shown value only (see REST lane)
        selected_index: 0,
        source_node_id: n.figma_node_id || "",
      });
      return;  // leaf overlay
    }

    // ── swap / action / xy_pad / value_label ────────────────────────────────
    // Whole-word name-gated, and run AFTER the tuned dropdown/stepper/tab_group/
    // text_field detectors in this visit() so they always win — these only claim
    // a node the others left unclaimed, and only on an explicit name token, so a
    // generic design never sprouts spurious overlays. (Knob detection is a
    // separate geometry pass, parseFrameKnobs, not part of this precedence.) The
    // Full node-tree signals (prototype reactions for swap, value patterns for
    // value_label, a command vocabulary for action) belong in the unified
    // resolver; this is the name-driven floor that proves the producer can emit
    // each kind.
    if (bb) {
      const named = detectNamedControl(n, name, ntype, toSvg(bb));
      if (named) { out.push(named); return; }
    }

    for (let i = 0; i < kids.length; i++) visit(kids[i], n);
  }

  visit(root, null);

  // ── Resolution self-check ────────────────────────────────────────────────
  // Stamp each emitted control with its resolution provenance (rung / confidence
  // / conflicts / verification). assessResolution cross-checks the name signal
  // against the node's geometry, so a control whose name and shape disagree
  // (the original silent-knob stumble) is FLAGGED — still materialized with the
  // best candidate AND recorded for review — instead of shipped silently.
  for (let i = 0; i < out.length; i++) {
    const el = out[i];
    const name = (el.source_node_id && nodeNameById[el.source_node_id]) || "";
    // detectOverlayControls resolves by the design's own naming + structure.
    const report = assessResolution(el.kind, name, { w: el.w || 0, h: el.h || 0 }, "name");
    el.resolution_rung = report.resolution_rung;
    el.confidence_score = report.confidence_score;
    if (report.conflict_signals.length > 0) el.conflict_signals = report.conflict_signals;
    if (!report.verification_pass) el.verification_pass = false;
  }
  return out;
}

// Whole-word test (ES5-conservative: no \b reliance on unicode, explicit
// non-alphanumeric boundaries so "xy" matches "xy pad" but not "deoxy").
function hasWord(s: string, w: string): boolean {
  return new RegExp("(^|[^a-z0-9])" + w + "([^a-z0-9]|$)").test(s);
}

// Name-driven detection for overlay kinds. `rect` is the node's SVG-space box.
function detectNamedControl(
  n: OverlayNode, name: string, ntype: string,
  rect: [number, number, number, number],
): InteractiveElement | null {
  const sid = n.figma_node_id || "";
  // xy_pad: a node explicitly named an XY pad.
  if (hasWord(name, "xy") || name.indexOf("xypad") !== -1 ||
      name.indexOf("xy pad") !== -1 || name.indexOf("xy-pad") !== -1) {
    return { kind: "xy_pad", x: rect[0], y: rect[1], w: rect[2], h: rect[3], source_node_id: sid };
  }
  // swap-link: a node named a swap / mode switch. A trailing number ("swap 2")
  // sets the target frame index.
  if (hasWord(name, "swap")) {
    const el: InteractiveElement = { kind: "swap", x: rect[0], y: rect[1], w: rect[2], h: rect[3], source_node_id: sid };
    const m = /(\d+)\s*$/.exec(name);
    if (m) el.target_frame = parseInt(m[1], 10);
    return el;
  }
  // action command button: "action:octave_up" or a whole-word "action" name. The
  // id is the command the consumer routes on, so take it from the ORIGINAL-case
  // node name / text (never the lowercased detection copy). A bare "action" with
  // no text yields an empty id so a consumer can detect an unconfigured button.
  if (name.indexOf("action:") === 0 || hasWord(name, "action")) {
    const orig = n.name || "";
    const id = name.indexOf("action:") === 0
      ? orig.substring("action:".length).replace(/^\s+|\s+$/g, "")
      : (firstText(n) || "").replace(/^\s+|\s+$/g, "");
    return { kind: "action", x: rect[0], y: rect[1], w: rect[2], h: rect[3], action: id, source_node_id: sid };
  }
  // value_label: a TEXT node DELIBERATELY named a readout. Gated on explicit
  // markers ("readout" / "value_label"), NOT the bare word "value"/"display" —
  // those are far too common on STATIC text layers to flip into a live overlay.
  // Richer content-pattern detection belongs in the unified resolver.
  if (ntype === "TEXT" &&
      (hasWord(name, "readout") || name.indexOf("value_label") !== -1 ||
       name.indexOf("value-label") !== -1 || name.indexOf("valuelabel") !== -1)) {
    return { kind: "value_label", x: rect[0], y: rect[1], w: rect[2], h: rect[3],
             text: firstText(n) || n.content || "", source_node_id: sid };
  }
  return null;
}
