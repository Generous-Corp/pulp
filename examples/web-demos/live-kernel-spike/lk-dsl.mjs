// Pulp Live — M1 — the Pulp DSL (a patch sheet you can play).
//
// It reads like a patch sheet, not a program: one line per module, signal flows
// left-to-right through named wires, units are written the way musicians say
// them (`1.2khz`, `180ms`, `-6db`, `7ct`). No functions, no loops, no types.
//
//   patch WarmKeys
//   osc1 = saw(freq: note.hz, amp: 0.3)
//   osc2 = saw(freq: note.hz * 1.007, amp: 0.3)
//   mix  = mixer(osc1, osc2, mix: 0.5)
//   env  = adsr(mix, a: 5ms, d: 120ms, s: 0.6, r: 300ms, gate: note.gate)
//   filt = ladder(env, cutoff: 1.2khz, res: 0.4)
//   out  = filt * -2db
//
// Rules that carry the feel:
//   * `out` is the patch  — exactly one binding named `out` is the graph output.
//   * audio first, knobs after — positional args are audio inputs (in port
//     order); `name: value` args are params.
//   * `~name` is the one-block feedback cable (lowers to an LKB0 edge fb=1).
//   * `x * -2db` is a gain stage — the only arithmetic on audio.
//   * units convert at parse time; `note.hz`/`note.gate` are the mono voice and
//     never enter the blob — they become a main-thread binding table the
//     keyboard evaluates through the same ~3 ms set_param path.
//
// Everything parses to the SAME bounded graph the S0 kernel already decodes,
// then encodes to the LKB0 blob (codec.hpp). DOM-free: runs in the browser
// editor AND in the headless test.

export const T = {
  OSC: 0, GAIN: 1, BIQUAD: 2, LADDER: 3, ADSR: 4, DELAY: 5, MIXER: 6,
  // iteration 2 — node-breadth expansion (6 → 14 wire types)
  SVF: 7, SHAPER: 8, DCBLOCK: 9, NOISE: 10, CHORUS: 11, REVERB: 12, COMP: 13,
};

// verb → kernel node (+ any param the verb bakes in: waveform / filter / curve).
export const VERBS = {
  sine: { type: T.OSC, bake: [1, 0] }, saw: { type: T.OSC, bake: [1, 1] },
  square: { type: T.OSC, bake: [1, 2] }, tri: { type: T.OSC, bake: [1, 3] },
  gain: { type: T.GAIN },
  lowpass: { type: T.BIQUAD, bake: [0, 0] }, highpass: { type: T.BIQUAD, bake: [0, 1] },
  bandpass: { type: T.BIQUAD, bake: [0, 2] }, notch: { type: T.BIQUAD, bake: [0, 3] },
  allpass: { type: T.BIQUAD, bake: [0, 4] }, peak: { type: T.BIQUAD, bake: [0, 5] },
  lowshelf: { type: T.BIQUAD, bake: [0, 6] }, highshelf: { type: T.BIQUAD, bake: [0, 7] },
  ladder: { type: T.LADDER }, adsr: { type: T.ADSR }, delay: { type: T.DELAY },
  mixer: { type: T.MIXER },
  // ── iteration 2 verbs ──
  svf: { type: T.SVF, bake: [0, 0] },              // modulation-stable state-variable LP
  shape: { type: T.SHAPER, bake: [0, 2] },         // tanh soft-clip
  fold: { type: T.SHAPER, bake: [0, 3] },          // wavefolder
  clip: { type: T.SHAPER, bake: [0, 1] },          // hard clip
  dcblock: { type: T.DCBLOCK },
  noise: { type: T.NOISE },
  chorus: { type: T.CHORUS }, reverb: { type: T.REVERB }, comp: { type: T.COMP },
};
const VERB_NAMES = Object.keys(VERBS);

// audio input ports per node type.
function audioIns(type) {
  if (type === T.OSC || type === T.NOISE) return 0;
  if (type === T.MIXER) return 2;
  return 1;
}

// Per-node-type param metadata: pid, natural unit (display), range + scrub scale.
// The editor reads this to scale/clamp scrubbing and to format the number.
export const PARAM_META = {
  [T.OSC]:    { freq: { pid: 0, unit: "hz", min: 20, max: 8000, scale: "log", def: 220 },
                amp:  { pid: 2, unit: "",   min: 0,  max: 1,    scale: "lin", step: 0.005, def: 0.3 } },
  [T.GAIN]:   { gain: { pid: 0, unit: "db", min: -60, max: 24, scale: "lin", step: 0.1, def: 0 } },
  [T.BIQUAD]: { at:   { pid: 1, unit: "hz", min: 20, max: 18000, scale: "log", def: 1000 },
                q:    { pid: 2, unit: "",   min: 0.1, max: 20,   scale: "log", def: 0.707 },
                gain: { pid: 3, unit: "db", min: -24, max: 24,  scale: "lin", step: 0.1, def: 0 } },
  [T.LADDER]: { cutoff: { pid: 0, unit: "hz", min: 20, max: 18000, scale: "log", def: 1000 },
                res:    { pid: 1, unit: "",   min: 0, max: 0.98, scale: "lin", step: 0.004, def: 0.3 } },
  [T.ADSR]:   { a: { pid: 0, unit: "ms", min: 0, max: 4, scale: "log", def: 0.01 },
                d: { pid: 1, unit: "ms", min: 0, max: 4, scale: "log", def: 0.1 },
                s: { pid: 2, unit: "",  min: 0, max: 1, scale: "lin", step: 0.005, def: 0.7 },
                r: { pid: 3, unit: "ms", min: 0, max: 8, scale: "log", def: 0.3 },
                gate: { pid: 4, unit: "", min: 0, max: 1, scale: "lin", step: 1, def: 0 } },
  [T.DELAY]:  { time: { pid: 0, unit: "ms", min: 0.001, max: 2, scale: "log", def: 0.25 },
                feedback: { pid: 1, unit: "", min: 0, max: 0.98, scale: "lin", step: 0.004, def: 0.3 },
                mix:  { pid: 2, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.35 } },
  [T.MIXER]:  { mix:  { pid: 0, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.5 } },
  // ── iteration 2 node params ──
  [T.SVF]:    { cutoff: { pid: 1, unit: "hz", min: 20, max: 18000, scale: "log", def: 1000 },
                res:    { pid: 2, unit: "",   min: 0.1, max: 0.98, scale: "lin", step: 0.004, def: 0.707 } },
  [T.SHAPER]: { drive: { pid: 1, unit: "", min: 0.1, max: 40, scale: "log", def: 1 } },
  [T.DCBLOCK]:{ pole:  { pid: 0, unit: "", min: 0.9, max: 0.9999, scale: "lin", step: 0.0004, def: 0.995 } },
  [T.NOISE]:  { amp:   { pid: 0, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.3 },
                color: { pid: 1, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0 } },
  [T.CHORUS]: { rate:  { pid: 0, unit: "hz", min: 0.05, max: 8, scale: "log", def: 1 },
                depth: { pid: 1, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.5 },
                mix:   { pid: 2, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.4 },
                delay: { pid: 3, unit: "ms", min: 0.001, max: 0.05, scale: "log", def: 0.015 } },
  [T.REVERB]: { decay: { pid: 0, unit: "s", min: 0.1, max: 12, scale: "log", def: 2 },
                damp:  { pid: 1, unit: "", min: 0, max: 0.99, scale: "lin", step: 0.004, def: 0.3 },
                mix:   { pid: 2, unit: "", min: 0, max: 1, scale: "lin", step: 0.005, def: 0.3 } },
  [T.COMP]:   { thresh:  { pid: 0, unit: "db", min: -60, max: 0, scale: "lin", step: 0.2, def: -20 },
                ratio:   { pid: 1, unit: "", min: 1, max: 20, scale: "log", def: 4 },
                attack:  { pid: 2, unit: "ms", min: 0.0001, max: 0.2, scale: "log", def: 0.005 },
                release: { pid: 3, unit: "ms", min: 0.005, max: 2, scale: "log", def: 0.1 } },
};

// ── unit conversion ──────────────────────────────────────────────────────────
const UNIT_FACTORS = { hz: 1, khz: 1000, ms: 0.001, s: 1, db: 1, ct: null };
export function convertUnit(num, unit) {
  if (!unit) return num;
  if (unit === "ct") return Math.pow(2, num / 1200); // cents → frequency ratio
  const f = UNIT_FACTORS[unit];
  return f == null ? num : num * f;
}

const NUM_RE = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?/i;
const UNIT_RE = /^(khz|hz|ms|s|db|ct)/i;

// Split a comma list at paren depth 0.
function splitArgs(s) {
  const out = []; let depth = 0, cur = "";
  for (const ch of s) {
    if (ch === "(") depth++;
    else if (ch === ")") depth--;
    if (ch === "," && depth === 0) { out.push(cur); cur = ""; }
    else cur += ch;
  }
  if (cur.trim() || out.length) out.push(cur);
  return out.map((x) => x.trim()).filter((x) => x !== "");
}

const editDistance = (a, b) => {
  const m = a.length, n = b.length, d = Array.from({ length: m + 1 }, (_, i) => [i, ...Array(n).fill(0)]);
  for (let j = 0; j <= n; j++) d[0][j] = j;
  for (let i = 1; i <= m; i++) for (let j = 1; j <= n; j++)
    d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
  return d[m][n];
};
const suggest = (word, pool) => {
  let best = null, bd = 3;
  for (const c of pool) { const dd = editDistance(word.toLowerCase(), c); if (dd < bd) { bd = dd; best = c; } }
  return best;
};

// Parse a param value: a number(+unit), or a note expression.
function parseValue(raw, err, line) {
  const s = raw.trim();
  if (s.startsWith("note.")) {
    const rest = s.slice(5);
    if (rest === "gate") return { note: { kind: "gate" }, value: 0 };
    if (rest.startsWith("hz")) {
      let mul = 1, add = 0;
      const tail = rest.slice(2).trim();
      const m = tail.match(/^([*+])\s*(.+)$/);
      if (m) {
        const nm = m[2].match(NUM_RE);
        if (!nm) { err(line, `bad note expression "${s}"`); return null; }
        let n = parseFloat(nm[0]);
        const um = m[2].slice(nm[0].length).match(UNIT_RE);
        if (um) n = convertUnit(n, um[1].toLowerCase());
        if (m[1] === "*") mul = n; else add = n;
      } else if (tail) { err(line, `bad note expression "${s}"`); return null; }
      return { note: { kind: "hz", mul, add }, value: 220 };
    }
    if (rest === "vel" || rest === "pressure") { err(line, `note.${rest} is reserved — lands with the M1 voice update`, "info"); return null; }
    err(line, `unknown note field "${s}" (use note.hz or note.gate)`); return null;
  }
  const nm = s.match(NUM_RE);
  if (!nm) { err(line, `expected a number, got "${s}"`); return null; }
  const num = parseFloat(nm[0]);
  const um = s.slice(nm[0].length).match(UNIT_RE);
  const unit = um ? um[1].toLowerCase() : "";
  return { value: convertUnit(num, unit), unit };
}

// Join physical lines into logical bindings (a param list may wrap across lines
// as long as parentheses stay open). Returns [{text, line}].
function logicalLines(src) {
  const phys = src.split(/\r?\n/);
  const out = []; let buf = "", startLine = 0, depth = 0, open = false;
  for (let i = 0; i < phys.length; i++) {
    let line = phys[i].replace(/#.*$/, "");
    for (const ch of line) { if (ch === "(") depth++; else if (ch === ")") depth = Math.max(0, depth - 1); }
    if (!open) { if (!line.trim()) continue; startLine = i + 1; open = true; buf = line; }
    else buf += " " + line;
    if (depth === 0) { out.push({ text: buf.trim(), line: startLine }); open = false; buf = ""; }
  }
  if (open && buf.trim()) out.push({ text: buf.trim(), line: startLine });
  return out;
}

// Parse a whole patch → { ok, errors, graph }.
export function parsePatch(src) {
  const errors = [];
  const err = (line, msg, level) => errors.push({ line, msg, level: level || "error" });

  const nodes = [];          // { type, params:[[pid,val]], verb, name, line }
  const internalEdges = [];  // { src, dst, dport, fb } created during node build
  const audioRefs = [];      // deferred: { dst, dport, name, fb, line }
  const nameOut = new Map();  // binding name → { alias?:name, out?:idx }
  const bindings = [];       // note.* voice bindings: { node, paramId, kind, mul, add }
  const noteBound = new Set();

  for (const { text, line } of logicalLines(src)) {
    if (/^patch\b/.test(text)) continue; // `patch NAME` header — cosmetic
    const eq = text.indexOf("=");
    if (eq < 0) { err(line, `expected \`name = ...\` (a binding), got "${trunc(text)}"`); continue; }
    const name = text.slice(0, eq).trim();
    const rhs = text.slice(eq + 1).trim();
    if (!/^[a-zA-Z_]\w*$/.test(name)) { err(line, `"${name}" is not a valid wire name`); continue; }

    // split rhs into a primary and any `* NdB` gain stages (top-level '*' only).
    const segs = splitTop(rhs, "*");
    const primary = segs[0].trim();
    const gainDbs = [];
    for (let i = 1; i < segs.length; i++) {
      const pv = parseValue(segs[i].trim(), err, line);
      if (pv && pv.unit === "db") gainDbs.push(pv.value);
      else if (pv) { err(line, `\`* value\` must be a dB gain (e.g. \`* -6db\`)`); }
    }

    let curOut; // node index this binding's output resolves to
    const callm = primary.match(/^([a-zA-Z]\w*)\s*\((.*)\)\s*$/s);
    if (callm) {
      const verb = callm[1].toLowerCase();
      const spec = VERBS[verb];
      if (!spec) { const s = suggest(verb, VERB_NAMES); err(line, `no node \`${callm[1]}\`${s ? ` — did you mean \`${s}\`?` : ""}`); continue; }
      const nodeIdx = nodes.length;
      const meta = PARAM_META[spec.type];
      // start every param at its default, then apply the verb bake + named args.
      const pmap = new Map();
      for (const k in meta) pmap.set(meta[k].pid, meta[k].def);
      if (spec.bake) pmap.set(spec.bake[0], spec.bake[1]);
      // parse args: positional (audio) vs `key: value` (params)
      const args = splitArgs(callm[2]);
      let portIdx = 0;
      for (const a of args) {
        const ci = a.indexOf(":");
        if (ci >= 0 && /^[a-zA-Z]\w*\s*:/.test(a)) { // param
          const key = a.slice(0, ci).trim();
          const pm = meta[key];
          if (!pm) { err(line, `\`${verb}\` has no \`${key}:\` — its knobs are ${Object.keys(meta).filter((k) => k !== "gate" || spec.type === T.ADSR).join(", ")}`); continue; }
          const pv = parseValue(a.slice(ci + 1), err, line);
          if (!pv) continue;
          if (pv.unit && !unitOk(pm.unit, pv.unit)) { err(line, `\`${key}:\` is ${pm.unit || "unitless"}, not ${pv.unit}`); continue; }
          pmap.set(pm.pid, pv.value);
          if (pv.note) { bindings.push({ node: nodeIdx, paramId: pm.pid, ...pv.note }); noteBound.add(nodeIdx + ":" + pm.pid); }
        } else { // audio ref (positional)
          const fb = a.startsWith("~");
          const refName = (fb ? a.slice(1) : a).trim();
          if (!/^[a-zA-Z_]\w*$/.test(refName)) { err(line, `bad input "${a}"`); portIdx++; continue; }
          if (portIdx >= audioIns(spec.type)) { err(line, `\`${verb}\` takes ${audioIns(spec.type)} audio input(s)`); }
          else audioRefs.push({ dst: nodeIdx, dport: portIdx, name: refName, fb: fb ? 1 : 0, line });
          portIdx++;
        }
      }
      const params = [...pmap.entries()].sort((a, b) => a[0] - b[0]);
      nodes.push({ type: spec.type, params, verb, name, line });
      curOut = nodeIdx;
    } else if (/^[a-zA-Z_]\w*$/.test(primary)) {
      // alias: `out = echo` (+ optional gain sugar handled below)
      curOut = { alias: primary };
    } else {
      err(line, `can't read "${trunc(primary)}" — expected \`verb(...)\` or a wire name`);
      continue;
    }

    // apply gain-sugar stages: append Gain nodes fed by curOut
    for (const db of gainDbs) {
      const gIdx = nodes.length;
      nodes.push({ type: T.GAIN, params: [[0, db]], verb: "gain", name, line });
      if (typeof curOut === "number") internalEdges.push({ src: curOut, dst: gIdx, dport: 0, fb: 0 });
      else audioRefs.push({ dst: gIdx, dport: 0, name: curOut.alias, fb: 0, line });
      curOut = gIdx;
    }
    nameOut.set(name, typeof curOut === "number" ? { out: curOut } : { alias: curOut.alias });
  }

  if (nodes.length === 0) {
    if (!errors.length) err(1, "patch has no nodes yet — try `out = saw(freq: 220)`");
    return finish(false);
  }

  // resolve a wire name to a real node index (following alias chains)
  const seen = new Set();
  function resolve(name, line) {
    let n = name;
    while (true) {
      if (seen.has(n)) { return null; }
      const rec = nameOut.get(n);
      if (!rec) { err(line, `unknown wire \`${name}\``); return null; }
      if (rec.out != null) return rec.out;
      seen.add(n); n = rec.alias;
    }
  }

  const edges = [...internalEdges];
  for (const r of audioRefs) {
    seen.clear();
    const src = resolve(r.name, r.line);
    if (src == null) continue;
    edges.push({ src, dst: r.dst, dport: r.dport, fb: r.fb });
  }

  // `out` is the patch
  let output = null;
  if (!nameOut.has("out")) err(1, "patch has no `out` — nothing to hear yet");
  else { seen.clear(); output = resolve("out", 1); }
  if (output == null && !errors.some((e) => e.level === "error")) err(1, "`out` doesn't resolve to a node");

  // capacity + cycle checks
  if (nodes.length > 64) err(1, `64-node limit reached (kernel LK_MAX_NODES) — this patch has ${nodes.length}`);
  const cyc = findCycle(nodes.length, edges);
  if (cyc) err(1, `loop ${cyc.map((i) => nodes[i]?.name || i).join(" → ")} needs a \`~\` on the cable that closes it`);

  if (errors.some((e) => e.level === "error")) return finish(false);
  const signature = nodes.map(shapeKey).join("|") + "#" +
    edges.map((e) => `${e.src}>${e.dst}.${e.dport}${e.fb ? "~" : ""}`).sort().join(",");
  const graph = { nodes, edges, output, bindings, noteBound, signature };
  return finish(true, graph);

  function finish(ok, graph) {
    return { ok: ok && !errors.some((e) => e.level === "error"), errors, graph: graph || null };
  }
}

function shapeKey(n) {
  // Topology identity for the shape-hash: node type + any verb-baked variant
  // (oscillator waveform, biquad/svf mode, shaper curve) that changes the DSP,
  // so e.g. saw→sine or shape→fold reads as a structural (crossfade) edit while
  // a knob turn stays a param edit.
  if (n.type === T.OSC) return "O" + (n.params.find((p) => p[0] === 1)?.[1] ?? 0);
  if (n.type === T.BIQUAD) return "B" + (n.params.find((p) => p[0] === 0)?.[1] ?? 0);
  if (n.type === T.SVF) return "V" + (n.params.find((p) => p[0] === 0)?.[1] ?? 0);
  if (n.type === T.SHAPER) return "S" + (n.params.find((p) => p[0] === 0)?.[1] ?? 0);
  return "T" + n.type;
}
const unitOk = (want, got) => {
  if (want === "hz") return got === "hz" || got === "khz";
  if (want === "ms" || want === "s") return got === "ms" || got === "s"; // time: ms/s interchangeable
  if (want === "db") return got === "db";
  if (want === "") return got === "ct" || got === ""; // unitless params accept bare numbers
  return false;
};
const trunc = (s) => (s.length > 24 ? s.slice(0, 24) + "…" : s);
function splitTop(s, sep) {
  const out = []; let depth = 0, cur = "";
  for (const ch of s) { if (ch === "(") depth++; else if (ch === ")") depth--; if (ch === sep && depth === 0) { out.push(cur); cur = ""; } else cur += ch; }
  out.push(cur); return out;
}
// Detect a cycle over NON-feedback edges (feedback edges break cycles legally).
function findCycle(n, edges) {
  const adj = Array.from({ length: n }, () => []);
  for (const e of edges) if (!e.fb) adj[e.src].push(e.dst);
  const color = new Array(n).fill(0), stack = [];
  function dfs(u) {
    color[u] = 1; stack.push(u);
    for (const v of adj[u]) {
      if (color[v] === 1) { const i = stack.indexOf(v); return stack.slice(i); }
      if (color[v] === 0) { const r = dfs(v); if (r) return r; }
    }
    color[u] = 2; stack.pop(); return null;
  }
  for (let i = 0; i < n; i++) if (color[i] === 0) { const r = dfs(i); if (r) return r; }
  return null;
}

// ── LKB0 encoder (mirrors experimental/live_kernel/codec.hpp) ─────────────────
export function encodeLKB0(graph) {
  const { nodes, edges, output } = graph;
  const params = [];
  nodes.forEach((n, i) => n.params.forEach(([id, v]) => params.push([i, id, v])));
  const len = 12 + nodes.length * 4 + edges.length * 8 + params.length * 8;
  const buf = new ArrayBuffer(len), dv = new DataView(buf), u8 = new Uint8Array(buf);
  u8[0] = 0x4c; u8[1] = 0x4b; u8[2] = 0x42; u8[3] = 0x30; u8[4] = 0;
  u8[5] = nodes.length; u8[6] = edges.length; u8[7] = params.length;
  dv.setUint16(8, output, true); dv.setUint16(10, 0, true);
  let off = 12;
  for (const n of nodes) { const p = audioIns(n.type); u8[off] = n.type; u8[off + 1] = p; u8[off + 2] = 1; u8[off + 3] = 0; off += 4; }
  for (const e of edges) { dv.setUint16(off, e.src, true); u8[off + 2] = e.sport || 0; u8[off + 3] = 0; dv.setUint16(off + 4, e.dst, true); u8[off + 6] = e.dport || 0; u8[off + 7] = e.fb ? 1 : 0; off += 8; }
  for (const [node, id, v] of params) { dv.setUint16(off, node, true); dv.setUint16(off + 2, id, true); dv.setFloat32(off + 4, v, true); off += 8; }
  return u8;
}

// Diff two graphs. Same SHAPE (node verbs + edges) → a value edit routed to
// set_param (instant); different shape → a structural crossfade. Excludes
// note-bound params (the keyboard owns them).
export function diffGraphs(prev, next) {
  if (!prev || prev.signature !== next.signature) return { kind: "structural", paramEdits: [] };
  const paramEdits = [];
  for (let i = 0; i < next.nodes.length; i++) {
    const b = next.nodes[i].params, a = prev.nodes[i].params;
    for (const [pid, v] of b) {
      if (next.noteBound.has(i + ":" + pid)) continue;
      const pa = a.find((x) => x[0] === pid);
      if (!pa || pa[1] !== v) paramEdits.push({ node: i, paramId: pid, value: v });
    }
  }
  return { kind: paramEdits.length ? "param" : "none", paramEdits };
}

// ── shipped presets ──────────────────────────────────────────────────────────
export const EXAMPLES = {
  WarmKeys: `patch WarmKeys
# Two detuned saws → filter → delay. The measured S0 patch, in DSL.
# Hit LATCH (or hold a key), then DRAG any number and hear it move.
osc1 = saw(freq: note.hz, amp: 0.3)
osc2 = saw(freq: note.hz * 1.007, amp: 0.3)
mix  = mixer(osc1, osc2, mix: 0.5)
env  = adsr(mix, a: 5ms, d: 120ms, s: 0.6, r: 300ms, gate: note.gate)
filt = ladder(env, cutoff: 1.2khz, res: 0.4)
tone = peak(filt, at: 2.5khz, q: 1, gain: +6db)
echo = delay(tone, time: 180ms, feedback: 0.35, mix: 0.35)
out  = echo * -2db`,
  AcidLine: `patch AcidLine
# The scrub showcase. Turn RIFF on, then drag cutoff: and res:.
o   = saw(freq: note.hz, amp: 0.35)
env = adsr(o, a: 2ms, d: 90ms, s: 0.1, r: 60ms, gate: note.gate)
f   = ladder(env, cutoff: 700hz, res: 0.82)
sl  = delay(f, time: 375ms, feedback: 0.3, mix: 0.22)
out = sl * -3db`,
  DubEcho: `patch DubEcho
# ~ret is a FEEDBACK cable: it arrives one block (2.7ms) late.
drone = saw(freq: 55hz, amp: 0.25)
sum   = mixer(drone, ~ret, mix: 0.5)
tape  = delay(sum, time: 150ms, feedback: 0.0, mix: 0.5)
ret   = tape * -6db
out   = ret`,
  GlassBell: `patch GlassBell
# Inharmonic partials + long release. Play it high.
a    = sine(freq: note.hz * 2.0, amp: 0.2)
b    = sine(freq: note.hz * 5.04, amp: 0.08)
m    = mixer(a, b, mix: 0.35)
env  = adsr(m, a: 1ms, d: 900ms, s: 0.0, r: 900ms, gate: note.gate)
ring = peak(env, at: 6khz, q: 4, gain: +8db)
sh   = delay(ring, time: 90ms, feedback: 0.45, mix: 0.3)
out  = sh * -4db`,
  LushPad: `patch LushPad
# 14-node flex: 3 detuned saws -> svf -> chorus -> reverb. Same grammar, new words.
a   = saw(freq: note.hz, amp: 0.16)
b   = saw(freq: note.hz * 1.006, amp: 0.16)
c   = saw(freq: note.hz * 0.994, amp: 0.16)
m1  = mixer(a, b, mix: 0.5)
m2  = mixer(m1, c, mix: 0.34)
env = adsr(m2, a: 280ms, d: 500ms, s: 0.85, r: 1400ms, gate: note.gate)
flt = svf(env, cutoff: 1.4khz, res: 0.45)
ch  = chorus(flt, rate: 0.35hz, depth: 0.6, mix: 0.5, delay: 14ms)
rv  = reverb(ch, decay: 3.5s, damp: 0.4, mix: 0.42)
out = rv * -5db`,
  FuzzBass: `patch FuzzBass
# Distortion chain: saw -> soft-clip shaper -> dc block -> ladder. Drag drive:.
o   = saw(freq: note.hz, amp: 0.5)
env = adsr(o, a: 3ms, d: 140ms, s: 0.7, r: 120ms, gate: note.gate)
dist = shape(env, drive: 6)
dc  = dcblock(dist, pole: 0.995)
flt = ladder(dc, cutoff: 1.1khz, res: 0.3)
out = flt * -8db`,
  NoiseSweep: `patch NoiseSweep
# Filtered noise. Drag cutoff: for a sweep, res: for a whistle.
n   = noise(amp: 0.4, color: 0.3)
env = adsr(n, a: 1ms, d: 220ms, s: 0.0, r: 200ms, gate: note.gate)
flt = svf(env, cutoff: 2khz, res: 0.7)
out = flt * -6db`,
};
