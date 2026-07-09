// Pure-JS unit tests for the wam-runtime wire protocol — no wasm, no browser,
// so they run in the same lane as wam-scope.test.mjs. They pin the three
// contracts the worklet, the Node runner, and the main-thread host must agree
// on: the MIDI-out record layout, per-URL processor naming, and the bridge's
// graceful degradation when a stale DSP module lacks the newer wam_* exports.

import { parseMidiOutRecords, processorNameForUrl, makeBridge } from "./wam-runtime.mjs";

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

console.log(failed ? `\n❌ ${failed} FAILED` : "\n✅ ALL WAM-RUNTIME CHECKS PASSED");
process.exit(failed ? 1 : 0);
