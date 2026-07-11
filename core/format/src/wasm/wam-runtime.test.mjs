// Pure-JS unit tests for the wam-runtime wire protocol — no wasm, no browser,
// so they run in the same lane as wam-scope.test.mjs. They pin the three
// contracts the worklet, the Node runner, and the main-thread host must agree
// on: the MIDI-out record layout, per-URL processor naming, and the bridge's
// graceful degradation when a stale DSP module lacks the newer wam_* exports.

import { parseMidiOutRecords, processorNameForUrl, makeBridge, makeWamAudioPorts } from "./wam-runtime.mjs";

let failed = 0;
const eq = (name, got, want) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : ` — got ${JSON.stringify(got)} want ${JSON.stringify(want)}`));
  if (!ok) failed++;
};
const ok = (name, cond, detail = "") => {
  console.log((cond ? "ok   " : "FAIL ") + name + (detail ? " — " + detail : ""));
  if (!cond) failed++;
};

// Build a MIDI-out byte stream the way append_midi_record does on the C++ side:
// [int32 sampleOffset LE][uint16 len LE][len bytes] per record.
function pack(records) {
  const total = records.reduce((n, r) => n + 6 + r.bytes.length, 0);
  const u8 = new Uint8Array(total);
  const dv = new DataView(u8.buffer);
  let i = 0;
  for (const r of records) {
    dv.setInt32(i, r.offset, true);
    dv.setUint16(i + 4, r.bytes.length, true);
    u8.set(r.bytes, i + 6);
    i += 6 + r.bytes.length;
  }
  return u8;
}

// ── parseMidiOutRecords: round-trip ────────────────────────────────────────
{
  const recs = [
    { offset: 0, bytes: [0x90, 60, 100] },
    { offset: 11, bytes: [0x80, 60, 0] },
    { offset: 128, bytes: [0xF0, 0x7D, 0x01, 0x02, 0xF7] }, // a sysex
  ];
  const buf = pack(recs);
  const out = parseMidiOutRecords(buf, buf.length);
  eq("round-trips all records", out.map((e) => ({ offset: e.offset, bytes: [...e.bytes] })), recs);
}

// ── parseMidiOutRecords: a truncated trailing record is dropped, not read OOB ─
{
  const buf = pack([{ offset: 5, bytes: [0x90, 62, 80] }]);
  const truncated = buf.slice(0, buf.length - 1);      // lose the last payload byte
  const out = parseMidiOutRecords(truncated, truncated.length);
  eq("drops a truncated trailing record", out, []);
}

// ── parseMidiOutRecords: honours `len` shorter than the buffer ──────────────
{
  const buf = pack([{ offset: 1, bytes: [0x90, 60, 1] }, { offset: 2, bytes: [0x80, 60, 0] }]);
  const out = parseMidiOutRecords(buf, 9);             // only the first 9-byte record
  ok("stops at len even when the buffer holds more", out.length === 1 && out[0].offset === 1,
     `${out.length} events`);
}

// ── processorNameForUrl: stable, distinct, and correctly namespaced ─────────
{
  const a = "https://example.com/mono-synth/wam-processor.js";
  const b = "https://example.com/gain/wam-processor.js";
  ok("stable for the same URL", processorNameForUrl(a) === processorNameForUrl(a));
  ok("distinct for different URLs", processorNameForUrl(a) !== processorNameForUrl(b));
  ok("carries the pulp-wam-processor prefix", processorNameForUrl(a).startsWith("pulp-wam-processor-"),
     processorNameForUrl(a));
  ok("query/hash change the name (input-sensitive)",
     processorNameForUrl(a) !== processorNameForUrl(a + "?v=2"));
}

// ── makeBridge: a stale DSP module missing the newer exports degrades, not throws ─
{
  // The minimum an old wam-dsp.js exported, WITHOUT wam_read_param_values /
  // wam_param_epoch (both postdate the first WAM ABI).
  const stale = {
    memory: new WebAssembly.Memory({ initial: 1 }),
    malloc: () => 0, free: () => {},
    wam_init: () => 1, wam_call_ctors: () => {},
    wam_descriptor: () => 0, wam_parameters: () => 0,
  };
  let bridge;
  ok("makeBridge over a stale export set does not throw", (() => {
    try { bridge = makeBridge(stale); return true; } catch { return false; }
  })());
  // The processor constructor feature-detects these before calling them; the
  // bridge must therefore surface them as absent (undefined), not as throwing stubs.
  ok("newer bridge methods are absent on a stale module (feature-detectable)",
     typeof bridge.readParamValues !== "function" && typeof bridge.paramEpoch !== "function",
     `readParamValues=${typeof bridge.readParamValues} paramEpoch=${typeof bridge.paramEpoch}`);
}

// ── SF-3: the removed interleave round-trip was behaviour-preserving ────────
// The WAM path used to transpose planar→interleaved (JS) →planar (C++) →DSP→
// planar→interleaved (C++) →planar (JS): four O(frames×channels) transposes.
// The two on each side are exact inverses, so the planar buffer the DSP sees —
// and the planar buffer the host reads back — is byte-identical whether it came
// through the old interleave round-trip or the new straight planar copy. Prove
// it as pure array algebra (no wasm needed): the four transposes cancel.
{
  // OLD input side: JS interleave, then C++ de-interleave.
  const oldPlanarSeenByDsp = (channels, ch, frames) => {
    const inter = new Float32Array(ch * frames);
    for (let f = 0; f < frames; f++) for (let c = 0; c < ch; c++) inter[f * ch + c] = channels[c][f];
    const planar = Array.from({ length: ch }, () => new Float32Array(frames));
    for (let f = 0; f < frames; f++) for (let c = 0; c < ch; c++) planar[c][f] = inter[f * ch + c];
    return planar;
  };
  // NEW input side: a straight per-channel planar copy (what writeInput does).
  const newPlanarSeenByDsp = (channels, ch, frames) => channels.map((src) => src.slice(0, frames));

  const eqPlanar = (a, b) => a.length === b.length && a.every((ch, i) => ch.length === b[i].length && ch.every((v, j) => v === b[i][j]));

  for (const [ch, frames] of [[1, 128], [2, 128], [2, 64]]) {
    const channels = Array.from({ length: ch }, (_, c) => Float32Array.from({ length: 128 }, (_, f) => Math.sin(0.13 * f + c) * 0.7 - 0.001 * f));
    const oldSeen = oldPlanarSeenByDsp(channels, ch, frames);
    const newSeen = newPlanarSeenByDsp(channels, ch, frames);
    ok(`interleave round-trip cancels for ${ch}ch × ${frames} — DSP sees identical planar`,
       eqPlanar(oldSeen, newSeen));
    // And the same identity holds symmetrically on the output side (a DSP that
    // wrote `newSeen` yields the same host planar buffers either way), so the
    // end-to-end WAM output is unchanged.
  }
}

// ── makeWamAudioPorts: the planar pointer-array marshalling round-trips ──────
// Back a real wasm heap with a bump allocator, run writeInput → an identity
// "wam_process" that reads the two pointer arrays → readInto, and confirm the
// output channels equal the input exactly. This proves the JS side of the new
// planar ABI is a straight copy (no transpose, no corruption) end-to-end.
{
  const memory = new WebAssembly.Memory({ initial: 1 }); // 64 KiB — ample for 2×128
  let brk = 1024;
  const exports = { memory, malloc: (n) => { const p = brk; brk += (n + 7) & ~7; return p; }, free: () => {} };
  const bridge = makeBridge(exports);
  const ports = makeWamAudioPorts(bridge, 2, 128);

  const L = Float32Array.from({ length: 128 }, (_, i) => Math.sin(i * 0.31) * 0.5);
  const R = Float32Array.from({ length: 128 }, (_, i) => Math.cos(i * 0.17) * -0.5);
  ports.writeInput([L, R], 128);

  // Identity "wam_process": read each channel pointer out of the pointer arrays
  // and copy input channel → output channel, exactly as the C++ processor would
  // render straight through the planar buffers.
  const dv = new DataView(memory.buffer);
  const identity = (inPtr, outPtr, ch, frames) => {
    const f32 = new Float32Array(memory.buffer);
    for (let c = 0; c < ch; c++) {
      const inChan = dv.getUint32(inPtr + c * 4, true) >> 2;
      const outChan = dv.getUint32(outPtr + c * 4, true) >> 2;
      for (let f = 0; f < frames; f++) f32[outChan + f] = f32[inChan + f];
    }
  };
  identity(ports.inPtr, ports.outPtr, 2, 128);

  const outL = new Float32Array(128), outR = new Float32Array(128);
  ports.readInto([outL, outR], 128);
  ok("planar ports round-trip channel 0 verbatim", outL.every((v, i) => v === L[i]));
  ok("planar ports round-trip channel 1 verbatim", outR.every((v, i) => v === R[i]));
  ok("planar ports keep channels distinct (no L/R swap or sum)",
     outL[0] === L[0] && outR[0] === R[0] && outL[0] !== outR[0]);

  // A missing input channel is zero-filled, not leaked from a previous block.
  ports.writeInput([L], 128); // only channel 0 supplied
  identity(ports.inPtr, ports.outPtr, 2, 128);
  const z = new Float32Array(128).fill(9); // pre-dirty
  ports.readInto([new Float32Array(128), z], 128);
  ok("planar ports zero-fill a missing input channel", z.every((v) => v === 0));
}

console.log(failed ? `\n❌ ${failed} FAILED` : "\n✅ ALL WAM-RUNTIME CHECKS PASSED");
process.exit(failed ? 1 : 0);
