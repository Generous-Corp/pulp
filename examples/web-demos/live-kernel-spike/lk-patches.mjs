// Pulp Live Kernel — S0 spike — shared graph blobs + LKB0 encoder.
//
// One ES module used by BOTH the offline node harness (measure.mjs) and the
// main-thread browser driver (index.html). The classic worklet cannot import it,
// so the worklet only ever receives already-encoded ArrayBuffers.
//
// NodeType wire ids (see experimental/live_kernel/codec.hpp):
export const T = { OSC: 0, GAIN: 1, BIQUAD: 2, LADDER: 3, ADSR: 4, DELAY: 5, MIXER: 6 };

// Encode a graph description to the LKB0 binary blob (Uint8Array).
//   nodes:  [{ type, params: [[paramId, value], ...] }]
//   edges:  [{ src, sport=0, dst, dport=0, fb=0 }]
//   output: node index of the graph output
export function encodePlan({ nodes, edges = [], output }) {
  const params = [];
  nodes.forEach((n, i) => (n.params || []).forEach(([id, v]) => params.push([i, id, v])));
  const len = 12 + nodes.length * 4 + edges.length * 8 + params.length * 8;
  const buf = new ArrayBuffer(len);
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  u8[0] = 0x4c; u8[1] = 0x4b; u8[2] = 0x42; u8[3] = 0x30; // 'LKB0'
  u8[4] = 0;                       // version
  u8[5] = nodes.length;
  u8[6] = edges.length;
  u8[7] = params.length;
  dv.setUint16(8, output, true);
  dv.setUint16(10, 0, true);
  let off = 12;
  for (const n of nodes) {
    const ni = portsFor(n.type);
    u8[off] = n.type; u8[off + 1] = ni.in; u8[off + 2] = ni.out; u8[off + 3] = 0;
    off += 4;
  }
  for (const e of edges) {
    dv.setUint16(off, e.src, true);
    u8[off + 2] = e.sport || 0; u8[off + 3] = 0;
    dv.setUint16(off + 4, e.dst, true);
    u8[off + 6] = e.dport || 0; u8[off + 7] = e.fb ? 1 : 0;
    off += 8;
  }
  for (const [node, id, v] of params) {
    dv.setUint16(off, node, true);
    dv.setUint16(off + 2, id, true);
    dv.setFloat32(off + 4, v, true);
    off += 8;
  }
  return u8;
}

function portsFor(type) {
  if (type === T.OSC) return { in: 0, out: 1 };
  if (type === T.MIXER) return { in: 2, out: 1 };
  return { in: 1, out: 1 };
}

// ── The three spike patches (must match aot_twin.cpp exactly) ────────────────

// Patch 0 — musical ~10-node chain: 2 detuned saws -> mixer -> ADSR VCA ->
// ladder -> peaking biquad -> delay -> 3 gains.
export const MUSICAL = {
  nodes: [
    { type: T.OSC,    params: [[0, 110.0], [1, 1], [2, 0.3]] },   // 0 osc1 saw
    { type: T.OSC,    params: [[0, 110.77], [1, 1], [2, 0.3]] },  // 1 osc2 saw
    { type: T.MIXER,  params: [[0, 0.5]] },                        // 2 mix
    { type: T.ADSR,   params: [[0, 0.005], [1, 0.12], [2, 0.6], [3, 0.3], [4, 1]] }, // 3 adsr (gate on)
    { type: T.LADDER, params: [[0, 1200.0], [1, 0.4]] },          // 4 ladder
    { type: T.BIQUAD, params: [[0, 5], [1, 2500.0], [2, 1.0], [3, 6.0]] }, // 5 peaking
    { type: T.DELAY,  params: [[0, 0.18], [1, 0.35], [2, 0.35]] },// 6 delay
    { type: T.GAIN,   params: [[0, -3.0]] },                       // 7
    { type: T.GAIN,   params: [[0, 0.0]] },                        // 8
    { type: T.GAIN,   params: [[0, -2.0]] },                       // 9 out
  ],
  edges: [
    { src: 0, dst: 2, dport: 0 }, { src: 1, dst: 2, dport: 1 },
    { src: 2, dst: 3 }, { src: 3, dst: 4 }, { src: 4, dst: 5 },
    { src: 5, dst: 6 }, { src: 6, dst: 7 }, { src: 7, dst: 8 }, { src: 8, dst: 9 },
  ],
  output: 9,
};

// Patch 1 — worst case: osc -> nine trivial unity gains (max per-node buffer
// traffic, minimal per-node work — the interpreter's weakest case).
export const TRIVIAL = {
  nodes: [
    { type: T.OSC, params: [[0, 220.0], [1, 1], [2, 0.3]] },
    ...Array.from({ length: 9 }, () => ({ type: T.GAIN, params: [[0, 0.0]] })),
  ],
  edges: Array.from({ length: 9 }, (_, i) => ({ src: i, dst: i + 1 })),
  output: 9,
};

// Patch 2 — legal one-block feedback: osc -> mixer -> delay -> gain -> out,
// with gain output fed BACK into the mixer (feedback edge = one-block delay).
export const FEEDBACK = {
  nodes: [
    { type: T.OSC,   params: [[0, 110.0], [1, 1], [2, 0.25]] }, // 0
    { type: T.MIXER, params: [[0, 0.5]] },                       // 1
    { type: T.DELAY, params: [[0, 0.15], [1, 0.0], [2, 0.5]] },  // 2
    { type: T.GAIN,  params: [[0, -6.0]] },                       // 3 out
  ],
  edges: [
    { src: 0, dst: 1, dport: 0 },
    { src: 1, dst: 2 },
    { src: 2, dst: 3 },
    { src: 3, dst: 1, dport: 1, fb: 1 }, // one-block feedback
  ],
  output: 3,
};

// Patch 3 — F2-S1 representative subgraph: osc -> ladder -> gain (the suggested
// emitter spike target; must match aot_twin.cpp setup_olg exactly).
export const OLG = {
  nodes: [
    { type: T.OSC,    params: [[0, 220.0], [1, 1], [2, 0.3]] }, // 0 saw
    { type: T.LADDER, params: [[0, 1200.0], [1, 0.4]] },        // 1
    { type: T.GAIN,   params: [[0, -3.0]] },                     // 2 out
  ],
  edges: [{ src: 0, dst: 1 }, { src: 1, dst: 2 }],
  output: 2,
};

export const PATCHES = [MUSICAL, TRIVIAL, FEEDBACK, OLG];
export const PATCH_NAMES = ["musical(10-node)", "trivial(osc+9gain)", "feedback(1-block)", "olg(osc→ladder→gain)"];

// Smooth single-oscillator patches (SINE, waveform id 0) used for the click-free
// crossfade test: a click is an impulsive DISCONTINUITY, so it is detected as a
// spike in the second difference (discrete Laplacian). Saw/graph content has
// intrinsic per-period discontinuities and cannot be used for click detection —
// these two are C-infinity smooth so any injected click stands out.
export const CLICK_A = {
  nodes: [
    { type: T.OSC,  params: [[0, 220.0], [1, 0], [2, 0.3]] }, // sine
    { type: T.GAIN, params: [[0, 0.0]] },
  ],
  edges: [{ src: 0, dst: 1 }],
  output: 1,
};
export const CLICK_B = {
  nodes: [
    { type: T.OSC,  params: [[0, 330.0], [1, 0], [2, 0.3]] }, // sine, different pitch
    { type: T.GAIN, params: [[0, 0.0]] },
  ],
  edges: [{ src: 0, dst: 1 }],
  output: 1,
};
