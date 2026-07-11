// Pulp Live Kernel — F2-S1 spike — graph → wasm emitter (classic script).
//
// Compiles an LKB0 graph blob into a purpose-built WebAssembly MODULE containing
// ONE fused, straight-line per-sample loop — the "stable" tier that erases the
// interpreter's per-node buffer traffic and dispatch overhead once a graph stops
// changing. Design doc: planning/2026-07-09-f2-graph-wasm-emitter-design.md.
//
// HARD CONSTRAINTS this file honors:
//   * Runs inside AudioWorkletGlobalScope: classic script (no ES import), no
//     fetch/atob/TextEncoder, no dependencies (no binaryen/LLVM/libfaust — the
//     wasm binary is hand-encoded below). Also loads in Node for the offline
//     harness (module.exports guard at the bottom).
//   * BIT-EXACT to the interpreter and the AOT twins, by construction:
//       - every rational f32 op is emitted in the same order as the C++ source
//         of the core/signal classes (wasm f32 ops are IEEE-754-exact, no FMA,
//         no reassociation), and
//       - every transcendental — per-sample (ladder tanh, sine osc, square/tri
//         fmod) AND emit-time constant folding (gain dB→lin powf, ladder expf,
//         biquad cosf/sinf/powf, SVF tanf) — routes through the resident
//         kernel's f2_* libm bridge, i.e. through the exact same emcc-linked
//         musl bits the interpreter itself executes.
//     Constant folding in JS uses Math.fround after each individual f32 op,
//     which is exact: for f32 operands, computing +,-,*,/,sqrt in f64 and
//     rounding once to f32 equals the direct f32 operation (f64 has >= 2p+2
//     significand bits).
//   * Zero-alloc by construction: the emitted module has NO allocator — no
//     malloc, no growth (memory min == max), no data-dependent control flow
//     that could allocate. State lives at fixed offsets; process() hoists it
//     into wasm locals for the loop and spills it back once per block.
//
// Node coverage (v0): Oscillator, Gain, Biquad, Ladder, Adsr, Delay, Mixer,
// Svf, Shaper, DcBlock, Noise — 11 of the 14 wire types. Chorus / Reverb / Comp
// (stereo, pool-backed classes) are NOT emitted: a graph containing them stays
// on the interpreter (emit() throws, the caller keeps the dev tier — correct
// audio, just interpreted). Feedback edges ARE supported (one-block
// feedback_prev, mirroring the executor).

/* eslint-disable no-undef */
(function () {
  "use strict";

  const fr = Math.fround;

  // ── node type ids (mirror experimental/live_kernel/codec.hpp) ───────────────
  const NT = {
    OSC: 0, GAIN: 1, BIQUAD: 2, LADDER: 3, ADSR: 4, DELAY: 5, MIXER: 6,
    SVF: 7, SHAPER: 8, DCBLOCK: 9, NOISE: 10, CHORUS: 11, REVERB: 12, COMP: 13,
  };
  const SUPPORTED = new Set([
    NT.OSC, NT.GAIN, NT.BIQUAD, NT.LADDER, NT.ADSR, NT.DELAY, NT.MIXER,
    NT.SVF, NT.SHAPER, NT.DCBLOCK, NT.NOISE,
  ]);
  const MAX_BLOCK = 128;              // LK_MAX_BLOCK
  const RING_SIZE = 48001;            // LK_MAX_DELAY_SAMPLES + 1 (DelayLineT pool)
  const PI_F = fr(3.14159265358979323846); // == fr(3.14159265): same f32

  function portsFor(type) {
    if (type === NT.OSC || type === NT.NOISE) return 0;
    if (type === NT.MIXER) return 2;
    return 1;
  }

  // ── LKB0 decode (mirror codec.hpp decode_plan; throws on malformed) ─────────
  function decodeLKB0(u8) {
    if (u8.length < 12) throw new Error("LKB0: too small");
    if (!(u8[0] === 0x4c && u8[1] === 0x4b && u8[2] === 0x42 && u8[3] === 0x30))
      throw new Error("LKB0: bad magic");
    if (u8[4] !== 0) throw new Error("LKB0: bad version");
    const numNodes = u8[5], numEdges = u8[6], numParams = u8[7];
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const output = dv.getUint16(8, true);
    const need = 12 + numNodes * 4 + numEdges * 8 + numParams * 8;
    if (u8.length < need) throw new Error("LKB0: truncated");
    if (numNodes === 0 || output >= numNodes) throw new Error("LKB0: bad output node");
    let off = 12;
    const nodes = [];
    for (let i = 0; i < numNodes; i++, off += 4) {
      if (u8[off] >= 14) throw new Error("LKB0: bad node type " + u8[off]);
      nodes.push(u8[off]);
    }
    const edges = [];
    for (let i = 0; i < numEdges; i++, off += 8) {
      const e = {
        src: dv.getUint16(off, true), sport: u8[off + 2],
        dst: dv.getUint16(off + 4, true), dport: u8[off + 6], fb: u8[off + 7] !== 0,
      };
      if (e.src >= numNodes || e.dst >= numNodes) throw new Error("LKB0: bad edge index");
      edges.push(e);
    }
    const params = [];
    for (let i = 0; i < numParams; i++, off += 8) {
      const node = dv.getUint16(off, true);
      if (node >= numNodes) throw new Error("LKB0: bad param node");
      params.push([node, dv.getUint16(off + 2, true), dv.getFloat32(off + 4, true)]);
    }
    return { nodes, edges, params, output };
  }

  // ── Kahn topo sort, mirroring executor.hpp topo_sort exactly ────────────────
  function topoSort(plan) {
    const n = plan.nodes.length;
    const indeg = new Array(n).fill(0);
    for (const e of plan.edges) if (!e.fb) indeg[e.dst]++;
    const queue = [];
    for (let i = 0; i < n; i++) if (indeg[i] === 0) queue.push(i);
    const order = [];
    let head = 0;
    while (head < queue.length) {
      const k = queue[head++];
      order.push(k);
      for (const e of plan.edges) {
        if (e.fb || e.src !== k) continue;
        if (--indeg[e.dst] === 0) queue.push(e.dst);
      }
    }
    if (order.length !== n) throw new Error("F2: graph has a non-feedback cycle");
    return order;
  }

  // ── param derivation (mirror registry.hpp init_node + set_node_param) ───────
  const clampf = (v, lo, hi) => (v < lo ? lo : (hi < v ? hi : v)); // std::clamp
  function rateFor(t, sr) { return t <= 0 ? 1 : fr(1 / fr(t * sr)); }

  function initCfg(type, sr) {
    switch (type) {
      case NT.OSC:     return { type, freq: 220, wave: 1, amp: fr(0.3) };
      case NT.GAIN:    return { type, db: 0 };
      case NT.BIQUAD:  return { type, bqType: 0, cutoff: 1000, q: fr(0.707), gainDb: 0 };
      case NT.LADDER:  return { type, cutoff: 1000, res: fr(0.3) };
      case NT.ADSR:    return { type, a: fr(0.005), d: fr(0.12), s: fr(0.6), r: fr(0.3), stage0: 0, gateOn: false };
      case NT.DELAY:   return { type, time: fr(0.25), fb: 0, mix: fr(0.5) };
      case NT.MIXER:   return { type, mix: fr(0.5) };
      case NT.SVF:     return { type, mode: 0, freq: 1000, q: fr(0.707) };
      case NT.SHAPER:  return { type, curve: 2, drive: 1 };
      case NT.DCBLOCK: return { type, pole: fr(0.995) };
      case NT.NOISE:   return { type, amp: fr(0.3), color: 0 };
      default: throw new Error("F2: unsupported node type " + type + " (falls back to interpreter)");
    }
  }

  function applyParam(c, pid, v) {
    switch (c.type) {
      case NT.OSC:
        if (pid === 0) c.freq = v; else if (pid === 1) c.wave = Math.trunc(v);
        else if (pid === 2) c.amp = v;
        break;
      case NT.GAIN: if (pid === 0) c.db = v; break;
      case NT.BIQUAD:
        if (pid === 0) c.bqType = Math.trunc(v); else if (pid === 1) c.cutoff = v;
        else if (pid === 2) c.q = v; else if (pid === 3) c.gainDb = v;
        break;
      case NT.LADDER:
        if (pid === 0) c.cutoff = v; else if (pid === 1) c.res = clampf(v, 0, 1);
        break;
      case NT.ADSR:
        if (pid === 0) c.a = v; else if (pid === 1) c.d = v;
        else if (pid === 2) c.s = v; else if (pid === 3) c.r = v;
        else if (pid === 4) {
          const want = v >= 0.5;
          if (want && !c.gateOn) c.stage0 = 1;                    // note_on -> attack
          else if (!want && c.gateOn && c.stage0 !== 0) c.stage0 = 4; // note_off -> release
          c.gateOn = want;
        }
        break;
      case NT.DELAY:
        if (pid === 0) c.time = v; else if (pid === 1) c.fb = clampf(v, 0, fr(0.99));
        else if (pid === 2) c.mix = clampf(v, 0, 1);
        break;
      case NT.MIXER: if (pid === 0) c.mix = clampf(v, 0, 1); break;
      case NT.SVF:
        if (pid === 0) c.mode = Math.trunc(v); else if (pid === 1) c.freq = v;
        else if (pid === 2) c.q = v;
        break;
      case NT.SHAPER:
        if (pid === 0) c.curve = Math.trunc(v); else if (pid === 1) c.drive = v;
        break;
      case NT.DCBLOCK: if (pid === 0) c.pole = v; break;
      case NT.NOISE:
        if (pid === 0) c.amp = v; else if (pid === 1) c.color = clampf(v, 0, 1);
        break;
    }
  }

  // biquad set_coefficients transcription (all 8 types, incl. the input guards).
  function biquadCoefs(bqType, f, q, sr, gdb, L) {
    if (!(Number.isFinite(sr) && sr > 0)) sr = 44100;
    if (!(Number.isFinite(q) && q > 0)) q = fr(1e-4);
    if (!Number.isFinite(gdb)) gdb = 0;
    if (!(Number.isFinite(f) && f > 0)) f = 1000;
    const w0 = fr(fr(fr(2 * PI_F) * f) / sr);
    const cw = L.cosf(w0), sw = L.sinf(w0);
    const alpha = fr(sw / fr(2 * q));
    let b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    const stdA = () => { a0 = fr(1 + alpha); a1 = fr(-2 * cw); a2 = fr(1 - alpha); };
    switch (bqType) {
      case 0: // lowpass
        b0 = fr(fr(1 - cw) / 2); b1 = fr(1 - cw); b2 = fr(fr(1 - cw) / 2); stdA(); break;
      case 1: // highpass
        b0 = fr(fr(1 + cw) / 2); b1 = -fr(1 + cw); b2 = fr(fr(1 + cw) / 2); stdA(); break;
      case 2: // bandpass
        b0 = alpha; b1 = 0; b2 = -alpha; stdA(); break;
      case 3: // notch
        b0 = 1; b1 = fr(-2 * cw); b2 = 1; stdA(); break;
      case 4: // allpass
        b0 = fr(1 - alpha); b1 = fr(-2 * cw); b2 = fr(1 + alpha); stdA(); break;
      case 5: { // peaking
        const A = L.powf(10, fr(gdb / 40));
        b0 = fr(1 + fr(alpha * A)); b1 = fr(-2 * cw); b2 = fr(1 - fr(alpha * A));
        a0 = fr(1 + fr(alpha / A)); a1 = fr(-2 * cw); a2 = fr(1 - fr(alpha / A));
        break;
      }
      case 6: { // low_shelf
        const A = L.powf(10, fr(gdb / 40));
        const tsa = fr(fr(2 * fr(Math.sqrt(A))) * alpha);
        const Ap1 = fr(A + 1), Am1 = fr(A - 1);
        b0 = fr(A * fr(fr(Ap1 - fr(Am1 * cw)) + tsa));
        b1 = fr(fr(2 * A) * fr(Am1 - fr(Ap1 * cw)));
        b2 = fr(A * fr(fr(Ap1 - fr(Am1 * cw)) - tsa));
        a0 = fr(fr(Ap1 + fr(Am1 * cw)) + tsa);
        a1 = fr(-2 * fr(Am1 + fr(Ap1 * cw)));
        a2 = fr(fr(Ap1 + fr(Am1 * cw)) - tsa);
        break;
      }
      case 7: { // high_shelf
        const A = L.powf(10, fr(gdb / 40));
        const tsa = fr(fr(2 * fr(Math.sqrt(A))) * alpha);
        const Ap1 = fr(A + 1), Am1 = fr(A - 1);
        b0 = fr(A * fr(fr(Ap1 + fr(Am1 * cw)) + tsa));
        b1 = fr(fr(-2 * A) * fr(Am1 + fr(Ap1 * cw)));
        b2 = fr(A * fr(fr(Ap1 + fr(Am1 * cw)) - tsa));
        a0 = fr(fr(Ap1 - fr(Am1 * cw)) + tsa);
        a1 = fr(2 * fr(Am1 - fr(Ap1 * cw)));
        a2 = fr(fr(Ap1 - fr(Am1 * cw)) - tsa);
        break;
      }
    }
    return { b0: fr(b0 / a0), b1: fr(b1 / a0), b2: fr(b2 / a0), a1: fr(a1 / a0), a2: fr(a2 / a0) };
  }

  function finalizeCfg(c, sr, L) {
    switch (c.type) {
      case NT.OSC:
        c.dt = fr(c.freq / sr); c.c1mdt = fr(1 - c.dt); c.c2pi = fr(2 * PI_F);
        break;
      case NT.GAIN: c.g = L.powf(10, fr(c.db / 20)); break;
      case NT.BIQUAD: c.co = biquadCoefs(c.bqType, c.cutoff, c.q, sr, c.gainDb, L); break;
      case NT.LADDER:
        c.res4 = fr(c.res * 4);
        c.g = fr(1 - L.expf(fr(fr(fr(-2 * PI_F) * c.cutoff) / sr)));
        break;
      case NT.ADSR:
        c.rA = rateFor(c.a, sr); c.rD = rateFor(c.d, sr); c.rR = rateFor(c.r, sr);
        break;
      case NT.DELAY: c.ds = clampf(fr(c.time * sr), 1, RING_SIZE - 1); c.cm = fr(1 - c.mix); break;
      case NT.MIXER: c.d = fr(1 - c.mix); break;
      case NT.SVF: {
        const g = L.tanf(fr(fr(PI_F * c.freq) / sr));
        c.k = fr(1 / c.q);
        c.a1 = fr(1 / fr(1 + fr(g * fr(g + c.k))));
        c.a2 = fr(g * c.a1); c.a3 = fr(g * c.a2);
        break;
      }
      case NT.SHAPER: c.hp = fr(1.5707963); break;
      case NT.NOISE: c.c231 = 2 ** -31; break;
    }
  }

  // ── wasm binary encoding helpers ─────────────────────────────────────────────
  const OP = {
    BLOCK: 0x02, LOOP: 0x03, IF: 0x04, ELSE: 0x05, END: 0x0b, BR: 0x0c, BR_IF: 0x0d,
    CALL: 0x10, SELECT: 0x1b,
    LGET: 0x20, LSET: 0x21, LTEE: 0x22,
    I32LOAD: 0x28, F32LOAD: 0x2a, I32STORE: 0x36, F32STORE: 0x38,
    I32CONST: 0x41, F32CONST: 0x43,
    I32EQZ: 0x45, I32EQ: 0x46,
    F32LT: 0x5d, F32GT: 0x5e, F32LE: 0x5f, F32GE: 0x60,
    I32ADD: 0x6a, I32SUB: 0x6b, I32REMS: 0x6f, I32OR: 0x72, I32XOR: 0x73,
    I32SHL: 0x74, I32SHRU: 0x76, I32GES: 0x4e,
    F32ABS: 0x8b, F32FLOOR: 0x8e,
    F32ADD: 0x92, F32SUB: 0x93, F32MUL: 0x94, F32DIV: 0x95,
    I32TRUNCSATF32S: [0xfc, 0x00], F32CONVERTI32S: 0xb2,
    MEMCOPY: [0xfc, 0x0a, 0x00, 0x00],
  };
  const T_F32 = 0x7d, T_I32 = 0x7f;

  class Bytes {
    constructor() { this.a = []; }
    byte(...bs) { for (const b of bs) this.a.push(b & 0xff); return this; }
    push(arr) { for (const b of (arr.a || arr)) this.a.push(b); return this; }
    u(n) { // unsigned LEB128
      n = n >>> 0;
      do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; this.a.push(b); } while (n);
      return this;
    }
    s(n) { // signed LEB128 (i32)
      n = n | 0;
      for (;;) {
        let b = n & 0x7f; n >>= 7;
        const done = (n === 0 && (b & 0x40) === 0) || (n === -1 && (b & 0x40) !== 0);
        if (!done) b |= 0x80;
        this.a.push(b);
        if (done) return this;
      }
    }
    f32(v) {
      if (fr(v) !== v || !Number.isFinite(v)) throw new Error("F2: non-f32 constant " + v);
      const dv = new DataView(new ArrayBuffer(4));
      dv.setFloat32(0, v, true);
      for (let i = 0; i < 4; i++) this.a.push(dv.getUint8(i));
      return this;
    }
    str(s) { this.u(s.length); for (let i = 0; i < s.length; i++) this.a.push(s.charCodeAt(i)); return this; }
    section(id, body) { this.byte(id); this.u(body.a.length); return this.push(body); }
  }

  // Code writer with local allocation + tiny op sugar.
  class Code extends Bytes {
    constructor(numParams) { super(); this.locals = []; this.numParams = numParams; }
    local(type) { this.locals.push(type); return this.numParams + this.locals.length - 1; }
    ld(i) { return this.byte(OP.LGET).u(i); }
    st(i) { return this.byte(OP.LSET).u(i); }
    te(i) { return this.byte(OP.LTEE).u(i); }
    f(v) { return this.byte(OP.F32CONST).f32(v); }
    i(k) { return this.byte(OP.I32CONST).s(k); }
    op(o) { return Array.isArray(o) ? this.byte(...o) : this.byte(o); }
    call(fi) { return this.byte(OP.CALL).u(fi); }
    if0() { return this.byte(OP.IF, 0x40); }
    else0() { return this.byte(OP.ELSE); }
    end() { return this.byte(OP.END); }
    blk() { return this.byte(OP.BLOCK, 0x40); }
    loop() { return this.byte(OP.LOOP, 0x40); }
    br(d) { return this.byte(OP.BR).u(d); }
    brif(d) { return this.byte(OP.BR_IF).u(d); }
    f32load(off) { return this.byte(OP.F32LOAD).u(2).u(off); }
    f32store(off) { return this.byte(OP.F32STORE).u(2).u(off); }
    i32load(off) { return this.byte(OP.I32LOAD).u(2).u(off); }
    i32store(off) { return this.byte(OP.I32STORE).u(2).u(off); }
  }

  // ── the emitter ──────────────────────────────────────────────────────────────
  // emit(blob, sampleRate, libm) -> { bytes, imports, stats }
  //   blob:       Uint8Array (LKB0)
  //   sampleRate: number (the AudioContext rate; f32-converted like lk_init)
  //   libm:       { tanhf, ladder_tanhf, sinf, cosf, expf, tanf, powf, fmodf } — the resident
  //               kernel's f2_* exports (bit-source for all transcendentals).
  // The emitted module imports the per-sample subset it needs from env.*;
  // instantiate with F2.imports(libm). Exports: memory, process(dst,n), dst.
  function emit(blob, sampleRate, libm) {
    const plan = decodeLKB0(blob instanceof Uint8Array ? blob : new Uint8Array(blob));
    for (const t of plan.nodes)
      if (!SUPPORTED.has(t)) throw new Error("F2: unsupported node type " + t + " (falls back to interpreter)");
    const order = topoSort(plan);
    const sr = fr(sampleRate);
    const cfgs = plan.nodes.map((t) => initCfg(t, sr));
    for (const [node, pid, v] of plan.params) applyParam(cfgs[node], pid, v);
    for (const c of cfgs) finalizeCfg(c, sr, libm);

    // Per-sample imports actually needed by this graph.
    const need = new Set();
    for (const c of cfgs) {
      // LadderFilterT<float> saturates with FastMath::tanh (Padé), not libm.
      if (c.type === NT.LADDER) need.add("ladder_tanhf");
      if (c.type === NT.OSC && c.wave === 0) need.add("sinf");
      if (c.type === NT.OSC && (c.wave === 2 || c.wave === 3)) need.add("fmodf");
      if (c.type === NT.SHAPER && c.curve === 2) need.add("tanhf");
      if (c.type === NT.SHAPER && c.curve === 4) need.add("sinf");
    }
    const importNames = ["tanhf", "ladder_tanhf", "sinf", "fmodf"].filter((n) => need.has(n));
    const fidx = {}; importNames.forEach((n, i) => (fidx[n] = i));
    const PROCESS_IDX = importNames.length;

    // ── memory layout ──────────────────────────────────────────────────────────
    let off = 0;
    const alloc = (bytes, align = 4) => { off = (off + align - 1) & ~(align - 1); const o = off; off += bytes; return o; };
    // state words per node (assigned below), fb buffers, rings, dst.
    const stateOff = [];      // per node: base offset of its state words
    const stateWords = [];    // per node: [{k:'f32'|'i32', init}]
    for (let k = 0; k < cfgs.length; k++) {
      const c = cfgs[k];
      let words = [];
      switch (c.type) {
        case NT.OSC:     words = [{ k: "f32", init: 0 }, { k: "f32", init: 0 }]; break; // phase, tri
        case NT.BIQUAD:  words = [{ k: "f32", init: 0 }, { k: "f32", init: 0 }]; break; // s1, s2
        case NT.LADDER:  words = [{ k: "f32", init: 0 }, { k: "f32", init: 0 }, { k: "f32", init: 0 }, { k: "f32", init: 0 }]; break;
        case NT.ADSR:    words = [{ k: "f32", init: 0 }, { k: "i32", init: c.stage0 }]; break; // level, stage
        case NT.DELAY:   words = [{ k: "i32", init: 0 }]; break; // write_pos
        case NT.SVF:     words = [{ k: "f32", init: 0 }, { k: "f32", init: 0 }]; break; // ic1, ic2
        case NT.DCBLOCK: words = [{ k: "f32", init: 0 }, { k: "f32", init: 0 }]; break; // last_in, last_out
        case NT.NOISE:   words = [{ k: "i32", init: 0x1234567 }, { k: "f32", init: 0 }]; break; // s, pink
        default: words = [];
      }
      stateWords.push(words);
      stateOff.push(words.length ? alloc(words.length * 4) : 0);
    }
    const stateSize = off;
    // feedback buffers (one-block feedback_prev semantics)
    const fbSources = [...new Set(plan.edges.filter((e) => e.fb).map((e) => e.src))];
    const fbPrev = {}, fbCur = {};
    for (const s of fbSources) { fbPrev[s] = alloc(MAX_BLOCK * 4, 16); fbCur[s] = alloc(MAX_BLOCK * 4, 16); }
    // delay rings
    const ringOff = {};
    for (let k = 0; k < cfgs.length; k++)
      if (cfgs[k].type === NT.DELAY) ringOff[k] = alloc(RING_SIZE * 4, 16);
    const dstOff = alloc(MAX_BLOCK * 4, 16);
    const pages = Math.max(1, Math.ceil(off / 65536));

    // ── code ───────────────────────────────────────────────────────────────────
    const C = new Code(2); // params: dst(0), n(1)
    const DST = 0, N = 1;
    const I = C.local(T_I32);
    // shared scratch
    const t0 = C.local(T_F32), t1 = C.local(T_F32), t2 = C.local(T_F32), t3 = C.local(T_F32);
    const in0 = C.local(T_F32), in1 = C.local(T_F32);
    const IN = [in0, in1];
    // per-node output locals + state locals
    const oL = cfgs.map(() => C.local(T_F32));
    const sL = cfgs.map((c, k) => stateWords[k].map((w) => C.local(w.k === "f32" ? T_F32 : T_I32)));

    // prologue: hoist persistent state into locals
    for (let k = 0; k < cfgs.length; k++)
      stateWords[k].forEach((w, wi) => {
        C.i(0);
        if (w.k === "f32") C.f32load(stateOff[k] + wi * 4); else C.i32load(stateOff[k] + wi * 4);
        C.st(sL[k][wi]);
      });

    // i = 0
    C.i(0).st(I);
    C.blk().loop();
    //   if (i >= n) break
    C.ld(I).ld(N).op(OP.I32GES).brif(1);

    // polyBLEP residual -> t1, given phase already in t0 (t0 is clobbered).
    const emitPolyBlep = (dt, c1mdt) => {
      C.ld(t0).f(dt).op(OP.F32LT).if0();
      C.ld(t0).f(dt).op(OP.F32DIV).st(t2);
      C.ld(t2).ld(t2).op(OP.F32ADD).ld(t2).ld(t2).op(OP.F32MUL).op(OP.F32SUB).f(1).op(OP.F32SUB).st(t1);
      C.else0();
      C.ld(t0).f(c1mdt).op(OP.F32GT).if0();
      C.ld(t0).f(1).op(OP.F32SUB).f(dt).op(OP.F32DIV).st(t2);
      C.ld(t2).ld(t2).op(OP.F32MUL).ld(t2).op(OP.F32ADD).ld(t2).op(OP.F32ADD).f(1).op(OP.F32ADD).st(t1);
      C.else0();
      C.f(0).st(t1);
      C.end().end();
    };

    for (const k of order) {
      const c = cfgs[k];
      const nin = portsFor(c.type);
      // gather: sum inbound edges per port, executor edge order, fb reads prev[]
      for (let p = 0; p < nin; p++) {
        let terms = 0;
        for (const e of plan.edges) {
          if (e.dst !== k) continue;
          let port = e.dport; if (port >= nin) port = nin - 1; if (port < 0) continue;
          if (port !== p) continue;
          if (e.fb) C.ld(I).i(2).op(OP.I32SHL).f32load(fbPrev[e.src]);
          else C.ld(oL[e.src]);
          if (terms++) C.op(OP.F32ADD);
        }
        if (!terms) C.f(0);
        C.st(IN[p]);
      }
      const out = oL[k];
      switch (c.type) {
        case NT.OSC: {
          const [ph, tri] = sL[k];
          if (c.wave === 0) { // sine: out = sin(2*pi*phase)
            C.f(c.c2pi).ld(ph).op(OP.F32MUL).call(fidx.sinf).st(out);
          } else if (c.wave === 1) { // saw: 2*phase - 1 - polyblep(phase, dt)
            C.f(2).ld(ph).op(OP.F32MUL).f(1).op(OP.F32SUB).st(out);
            C.ld(ph).st(t0); emitPolyBlep(c.dt, c.c1mdt);
            C.ld(out).ld(t1).op(OP.F32SUB).st(out);
          } else { // square (2) / triangle (3)
            C.f(1).f(-1).ld(ph).f(fr(0.5)).op(OP.F32LT).op(OP.SELECT).st(out);
            C.ld(ph).st(t0); emitPolyBlep(c.dt, c.c1mdt);
            C.ld(out).ld(t1).op(OP.F32ADD).st(out);
            C.ld(ph).f(fr(0.5)).op(OP.F32ADD).f(1).call(fidx.fmodf).st(t0);
            emitPolyBlep(c.dt, c.c1mdt);
            C.ld(out).ld(t1).op(OP.F32SUB).st(out);
            if (c.wave === 3) { // tri_state = dt*out + (1-dt)*tri; out = tri*4
              C.f(c.dt).ld(out).op(OP.F32MUL).f(c.c1mdt).ld(tri).op(OP.F32MUL).op(OP.F32ADD).te(tri);
              C.f(4).op(OP.F32MUL).st(out);
            }
          }
          // phase += dt; if (phase >= 1) phase -= 1
          C.ld(ph).f(c.dt).op(OP.F32ADD).te(ph).f(1).op(OP.F32GE).if0();
          C.ld(ph).f(1).op(OP.F32SUB).st(ph);
          C.end();
          C.ld(out).f(c.amp).op(OP.F32MUL).st(out); // registry: osc.next() * amp
          break;
        }
        case NT.GAIN:
          C.ld(in0).f(c.g).op(OP.F32MUL).st(out);
          break;
        case NT.MIXER: // dry*(1-mix) + wet*mix
          C.ld(in0).f(c.d).op(OP.F32MUL).ld(in1).f(c.mix).op(OP.F32MUL).op(OP.F32ADD).st(out);
          break;
        case NT.ADSR: { // env state machine (AdsrT::next), then out = in * env
          const [lvl, stg] = sL[k];
          C.ld(stg).i(1).op(OP.I32EQ).if0(); // attack
          C.ld(lvl).f(c.rA).op(OP.F32ADD).te(lvl).f(1).op(OP.F32GE).if0();
          C.f(1).st(lvl).i(2).st(stg);
          C.end();
          C.ld(lvl).st(t0);
          C.else0();
          C.ld(stg).i(2).op(OP.I32EQ).if0(); // decay
          C.ld(lvl).f(c.rD).op(OP.F32SUB).te(lvl).f(c.s).op(OP.F32LE).if0();
          C.f(c.s).st(lvl).i(3).st(stg);
          C.end();
          C.ld(lvl).st(t0);
          C.else0();
          C.ld(stg).i(3).op(OP.I32EQ).if0(); // sustain
          C.ld(lvl).st(t0);
          C.else0();
          C.ld(stg).i(4).op(OP.I32EQ).if0(); // release
          C.ld(lvl).f(c.rR).op(OP.F32SUB).te(lvl).f(0).op(OP.F32LE).if0();
          C.f(0).st(lvl).i(0).st(stg);
          C.end();
          C.ld(lvl).st(t0);
          C.else0(); // idle
          C.f(0).st(t0);
          C.end().end().end().end();
          C.ld(in0).ld(t0).op(OP.F32MUL).st(out);
          break;
        }
        case NT.LADDER: { // LadderFilterT::process (8 tanh via bridge import)
          const st = sL[k];
          // t0 = x = in - res4*(s3 - in*0.5)
          C.ld(in0).f(c.res4).ld(st[3]).ld(in0).f(fr(0.5)).op(OP.F32MUL).op(OP.F32SUB)
            .op(OP.F32MUL).op(OP.F32SUB).st(t0);
          for (let s = 0; s < 4; s++) {
            // st[s] += g * (tanh(prev) - tanh(st[s]))
            C.ld(st[s]).f(c.g);
            if (s === 0) C.ld(t0); else C.ld(st[s - 1]);
            C.call(fidx.ladder_tanhf).ld(st[s]).call(fidx.ladder_tanhf).op(OP.F32SUB)
              .op(OP.F32MUL).op(OP.F32ADD).st(st[s]);
          }
          C.ld(st[3]).st(out);
          break;
        }
        case NT.BIQUAD: { // Direct Form II Transposed
          const [s1, s2] = sL[k];
          const co = c.co;
          C.f(co.b0).ld(in0).op(OP.F32MUL).ld(s1).op(OP.F32ADD).st(out);
          C.f(co.b1).ld(in0).op(OP.F32MUL).f(co.a1).ld(out).op(OP.F32MUL).op(OP.F32SUB)
            .ld(s2).op(OP.F32ADD).st(s1);
          C.f(co.b2).ld(in0).op(OP.F32MUL).f(co.a2).ld(out).op(OP.F32MUL).op(OP.F32SUB).st(s2);
          break;
        }
        case NT.DELAY: { // registry: wet=read(ds); push(in + fb*wet); out=in*(1-mix)+wet*mix
          const [wp] = sL[k];
          const ring = ringOff[k];
          const rp = t0, frac = t2, wet = t3;
          const j0 = C.local(T_I32), j1 = C.local(T_I32); // per-delay index scratch
          // rp = (f32)wp - ds - 1
          C.ld(wp).op(OP.F32CONVERTI32S).f(c.ds).op(OP.F32SUB).f(1).op(OP.F32SUB).st(rp);
          // while (rp < 0) rp += RING_SIZE
          C.blk().loop();
          C.ld(rp).f(0).op(OP.F32LT).op(OP.I32EQZ).brif(1);
          C.ld(rp).f(RING_SIZE).op(OP.F32ADD).st(rp);
          C.br(0).end().end();
          // idx0 = (int)rp % size; idx1 = (idx0+1) % size; frac = rp - floor(rp)
          C.ld(rp).op(OP.I32TRUNCSATF32S).i(RING_SIZE).op(OP.I32REMS).st(j0);
          C.ld(j0).i(1).op(OP.I32ADD).i(RING_SIZE).op(OP.I32REMS).st(j1);
          C.ld(rp).ld(rp).op(OP.F32FLOOR).op(OP.F32SUB).st(frac);
          // wet = ring[idx0]*(1-frac) + ring[idx1]*frac
          C.ld(j0).i(2).op(OP.I32SHL).f32load(ring)
            .f(1).ld(frac).op(OP.F32SUB).op(OP.F32MUL);
          C.ld(j1).i(2).op(OP.I32SHL).f32load(ring).ld(frac).op(OP.F32MUL)
            .op(OP.F32ADD).st(wet);
          // ring[wp] = in + fb*wet; wp = (wp+1) % size
          C.ld(wp).i(2).op(OP.I32SHL)
            .ld(in0).f(c.fb).ld(wet).op(OP.F32MUL).op(OP.F32ADD)
            .f32store(ring);
          C.ld(wp).i(1).op(OP.I32ADD).i(RING_SIZE).op(OP.I32REMS).st(wp);
          // out = in*(1-mix) + wet*mix
          C.ld(in0).f(c.cm).op(OP.F32MUL).ld(wet).f(c.mix).op(OP.F32MUL).op(OP.F32ADD).st(out);
          break;
        }
        case NT.SVF: { // SvfT::process (TPT)
          const [ic1, ic2] = sL[k];
          const v1 = t0, v2 = t1, v3 = t2;
          C.ld(in0).ld(ic2).op(OP.F32SUB).st(v3);
          C.f(c.a1).ld(ic1).op(OP.F32MUL).f(c.a2).ld(v3).op(OP.F32MUL).op(OP.F32ADD).st(v1);
          C.ld(ic2).f(c.a2).ld(ic1).op(OP.F32MUL).op(OP.F32ADD)
            .f(c.a3).ld(v3).op(OP.F32MUL).op(OP.F32ADD).st(v2);
          C.f(2).ld(v1).op(OP.F32MUL).ld(ic1).op(OP.F32SUB).st(ic1);
          C.f(2).ld(v2).op(OP.F32MUL).ld(ic2).op(OP.F32SUB).st(ic2);
          if (c.mode === 1)      // highpass: in - k*v1 - v2
            C.ld(in0).f(c.k).ld(v1).op(OP.F32MUL).op(OP.F32SUB).ld(v2).op(OP.F32SUB).st(out);
          else if (c.mode === 2) C.ld(v1).st(out); // bandpass
          else if (c.mode === 3) // notch: in - k*v1
            C.ld(in0).f(c.k).ld(v1).op(OP.F32MUL).op(OP.F32SUB).st(out);
          else C.ld(v2).st(out); // lowpass (and default)
          break;
        }
        case NT.SHAPER: { // WaveShaperT::process
          C.ld(in0).f(c.drive).op(OP.F32MUL).st(t0); // x = in*drive
          if (c.curve === 0) { // soft_clip: x / (1 + |x|)
            C.ld(t0).f(1).ld(t0).op(OP.F32ABS).op(OP.F32ADD).op(OP.F32DIV).st(out);
          } else if (c.curve === 1) { // hard_clip: std::clamp(x, -1, 1)
            C.f(-1).ld(t0).ld(t0).f(-1).op(OP.F32LT).op(OP.SELECT).st(t1); // x < -1 ? -1 : x
            C.f(1).ld(t1).f(1).ld(t1).op(OP.F32LT).op(OP.SELECT).st(out);  // 1 < r ? 1 : r
          } else if (c.curve === 3) { // fold: reflect into [-1, 1]
            C.blk().loop();
            C.ld(t0).f(1).op(OP.F32GT).ld(t0).f(-1).op(OP.F32LT).op(OP.I32OR).op(OP.I32EQZ).brif(1);
            C.ld(t0).f(1).op(OP.F32GT).if0();
            C.f(2).ld(t0).op(OP.F32SUB).st(t0);
            C.end();
            C.ld(t0).f(-1).op(OP.F32LT).if0();
            C.f(-2).ld(t0).op(OP.F32SUB).st(t0);
            C.end();
            C.br(0).end().end();
            C.ld(t0).st(out);
          } else if (c.curve === 4) { // sine_fold: sin(x * pi/2)
            C.ld(t0).f(c.hp).op(OP.F32MUL).call(fidx.sinf).st(out);
          } else { // tanh_clip (2, and default)
            C.ld(t0).call(fidx.tanhf).st(out);
          }
          break;
        }
        case NT.DCBLOCK: { // y = x - last_in + pole*last_out
          const [li, lo] = sL[k];
          C.ld(in0).ld(li).op(OP.F32SUB).f(c.pole).ld(lo).op(OP.F32MUL).op(OP.F32ADD).st(out);
          C.ld(in0).st(li);
          C.ld(out).st(lo);
          break;
        }
        case NT.NOISE: { // kernel-local NoiseGen (xorshift32 + one-pole tilt)
          const [s, pink] = sL[k];
          C.ld(s).ld(s).i(13).op(OP.I32SHL).op(OP.I32XOR).st(s);
          C.ld(s).ld(s).i(17).op(OP.I32SHRU).op(OP.I32XOR).st(s);
          C.ld(s).ld(s).i(5).op(OP.I32SHL).op(OP.I32XOR).st(s);
          C.ld(s).op(OP.F32CONVERTI32S).f(c.c231).op(OP.F32MUL).st(t0); // w
          C.f(fr(0.98)).ld(pink).op(OP.F32MUL).f(fr(0.02)).ld(t0).op(OP.F32MUL).op(OP.F32ADD).st(pink);
          // out = (w + color*(pink*6 - w)) * amp
          C.ld(t0).f(c.color).ld(pink).f(6).op(OP.F32MUL).ld(t0).op(OP.F32SUB)
            .op(OP.F32MUL).op(OP.F32ADD).f(c.amp).op(OP.F32MUL).st(out);
          break;
        }
      }
      // feedback source: also record this sample into cur[] for next block
      if (fbCur[k] !== undefined) {
        C.ld(I).i(2).op(OP.I32SHL).ld(out).f32store(fbCur[k]);
      }
    }

    // dst[i] = out[output]; i++
    C.ld(DST).ld(I).i(2).op(OP.I32SHL).op(OP.I32ADD).ld(oL[plan.output]).f32store(0);
    C.ld(I).i(1).op(OP.I32ADD).st(I);
    C.br(0).end().end();

    // epilogue: spill state, capture feedback (prev <- cur, n*4 bytes)
    for (let k = 0; k < cfgs.length; k++)
      stateWords[k].forEach((w, wi) => {
        C.i(0).ld(sL[k][wi]);
        if (w.k === "f32") C.f32store(stateOff[k] + wi * 4); else C.i32store(stateOff[k] + wi * 4);
      });
    for (const s of fbSources) {
      C.i(fbPrev[s]).i(fbCur[s]).ld(N).i(2).op(OP.I32SHL).op(OP.MEMCOPY);
    }
    C.end(); // function end

    // ── assemble module ────────────────────────────────────────────────────────
    const mod = new Bytes();
    mod.byte(0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00);
    // type section: 0 = (f32)->f32, 1 = (f32,f32)->f32, 2 = (i32,i32)->()
    const tFF = 0, tFFF = 1, tPROC = 2;
    const types = new Bytes().u(3)
      .byte(0x60).u(1).byte(T_F32).u(1).byte(T_F32)
      .byte(0x60).u(2).byte(T_F32, T_F32).u(1).byte(T_F32)
      .byte(0x60).u(2).byte(T_I32, T_I32).u(0);
    mod.section(1, types);
    // imports
    if (importNames.length) {
      const imp = new Bytes().u(importNames.length);
      for (const nm of importNames)
        imp.str("env").str(nm).byte(0x00).u(nm === "fmodf" ? tFFF : tFF);
      mod.section(2, imp);
    }
    mod.section(3, new Bytes().u(1).u(tPROC));                     // functions
    mod.section(5, new Bytes().u(1).byte(0x01).u(pages).u(pages)); // memory min==max
    mod.section(6, new Bytes().u(1).byte(T_I32, 0x00)              // global: dst (const)
      .byte(OP.I32CONST).s(dstOff).byte(OP.END));
    const exp = new Bytes().u(3)
      .str("memory").byte(0x02).u(0)
      .str("process").byte(0x00).u(PROCESS_IDX)
      .str("dst").byte(0x03).u(0);
    mod.section(7, exp);
    // code
    const body = new Bytes();
    { // run-length-encode locals
      const runs = [];
      for (const t of C.locals) {
        if (runs.length && runs[runs.length - 1][1] === t) runs[runs.length - 1][0]++;
        else runs.push([1, t]);
      }
      body.u(runs.length);
      for (const [count, t] of runs) body.u(count).byte(t);
      body.push(C);
    }
    mod.section(10, new Bytes().u(1).u(body.a.length).push(body));
    // data: initial state (ADSR stage, noise seed; everything else zero)
    if (stateSize > 0) {
      const init = new Uint8Array(stateSize);
      const dv = new DataView(init.buffer);
      for (let k = 0; k < cfgs.length; k++)
        stateWords[k].forEach((w, wi) => {
          if (w.init === 0) return;
          if (w.k === "f32") dv.setFloat32(stateOff[k] + wi * 4, w.init, true);
          else dv.setInt32(stateOff[k] + wi * 4, w.init, true);
        });
      const data = new Bytes().u(1).byte(0x00)
        .byte(OP.I32CONST).s(0).byte(OP.END).u(init.length);
      for (const b of init) data.byte(b);
      mod.section(11, data);
    }

    return {
      bytes: new Uint8Array(mod.a),
      imports: importNames,
      stats: {
        nodes: cfgs.length, edges: plan.edges.length, order,
        moduleBytes: mod.a.length, memPages: pages, dstOff,
        feedbackSources: fbSources.length,
      },
    };
  }

  // Build the instantiate-imports object from the kernel's f2_* bridge exports.
  function imports(libm) {
    return { env: { tanhf: libm.tanhf, ladder_tanhf: libm.ladder_tanhf,
                    sinf: libm.sinf, fmodf: libm.fmodf } };
  }
  // Pluck the full libm bridge off a kernel instance's exports.
  function libmOf(ex) {
    return {
      tanhf: ex.f2_tanhf, ladder_tanhf: ex.f2_ladder_tanhf,
      sinf: ex.f2_sinf, cosf: ex.f2_cosf, expf: ex.f2_expf,
      tanf: ex.f2_tanf, powf: ex.f2_powf, fmodf: ex.f2_fmodf,
    };
  }

  // ── dev↔stable handoff: the interpreter's equal-power crossfade shape ────────
  // (crossfade.hpp Kernel::process: go = cos(t·π/2), gn = sin(t·π/2)), applied
  // in the worklet between the interpreter's output (a) and the compiled
  // module's output (b). Endpoints are exact: t=0 → a bit-exact, t=1 → b
  // bit-exact, so the seam is confined to the fade window by construction.
  const HALF_PI = Math.PI / 2;
  class Handoff {
    constructor(sampleRate) {
      this.sr = sampleRate;
      this.mode = "interp"; // interp | toCompiled | toInterp | compiled
      this.x = 0;           // fade position in samples (compiled weight = x/len)
      this.len = 1;
    }
    armToCompiled(fadeMs) {
      const len = Math.max(1, Math.round(fadeMs * 0.001 * this.sr));
      if (this.mode === "compiled") return;
      this.x = this.mode === "interp" ? 0 : (this.x / this.len) * len;
      this.len = len;
      this.mode = "toCompiled";
    }
    armToInterp() {
      if (this.mode === "interp") return;
      if (this.mode === "compiled") this.x = this.len;
      this.mode = "toInterp";
    }
    // Blend a (interpreter) and b (compiled) into out for n frames; advances the
    // fade and resolves terminal modes. Only call while mode is a fade.
    blend(out, a, b, n) {
      const dir = this.mode === "toCompiled" ? 1 : -1;
      for (let i = 0; i < n; i++) {
        let t = (this.x + dir * i) / this.len;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        out[i] = a[i] * Math.cos(t * HALF_PI) + b[i] * Math.sin(t * HALF_PI);
      }
      this.x += dir * n;
      if (dir > 0 && this.x >= this.len) { this.x = this.len; this.mode = "compiled"; }
      if (dir < 0 && this.x <= 0) { this.x = 0; this.mode = "interp"; }
      return this.mode;
    }
  }

  const F2 = {
    emit, imports, libmOf, decodeLKB0, Handoff,
    SUPPORTED, NT, RING_SIZE, MAX_BLOCK,
  };
  globalThis.F2 = F2;
  if (typeof module !== "undefined" && module.exports) module.exports = F2;
})();
