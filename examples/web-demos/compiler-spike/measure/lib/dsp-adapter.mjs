// dsp-adapter.mjs — the generic DSP adapter contract + a WAM implementation.
//
// THE CONTRACT (what the S0 kernel VM drops in to get gate numbers for free):
// an adapter is a plain object with —
//
//   name          : string
//   channels      : number     (interleaved channel count)
//   frames        : number     (block size / quantum, e.g. 128)
//   sampleRate    : number
//   inBuf         : Float32Array  length channels*frames — caller WRITES input here
//   outBuf        : Float32Array  length channels*frames — filled by processQuantum()
//   processQuantum(): void      — render exactly ONE 128-frame block, in place
//   setParam(id, value): void   — (optional) apply a parameter change
//   midi(status, d1, d2, offset): void  — (optional) feed a MIDI byte triple
//   reset(): void               — (optional)
//   dispose(): void             — (optional)
//
// Everything measure-core.mjs does is expressed against this contract, so the
// AOT WAM plugin, the future kernel-VM wasm, and the future emitted-wasm module
// are all measured by the identical harnesses. To measure the kernel VM, write a
// `loadLiveKernel(bytes, opts)` that returns this same shape and pass it in.
//
// This WAM implementation reuses the shipped, single-source-of-truth runtime
// helpers (core/format/src/wasm/wam-runtime.mjs) so the import stubs and heap
// marshalling cannot drift from the real worklet/host path.

import { makeWasmImports, makeBridge } from "../../../../../core/format/src/wasm/wam-runtime.mjs";

// Build a WAM adapter from raw wasm bytes (Uint8Array/ArrayBuffer). Works in
// Node and in a browser page alike — no fs, no fetch here.
export function loadWamDsp(bytes, opts = {}) {
  const { sampleRate = 48000, frames = 128, channels = 2, name = "wam" } = opts;
  const mod = new WebAssembly.Module(bytes);
  let instance;
  instance = new WebAssembly.Instance(mod, makeWasmImports(() => instance.exports.memory));
  const wam = makeBridge(instance.exports);
  wam.callCtors();
  if (!wam.init(sampleRate, frames)) throw new Error("wam_init returned 0");

  const N = channels * frames;
  const inPtr = wam.malloc(N * 4);
  const outPtr = wam.malloc(N * 4);
  // The headless DSP heap never grows (emscripten_resize_heap → false), so views
  // over the exported buffer stay valid for the adapter's lifetime.
  const inBuf = new Float32Array(instance.exports.memory.buffer, inPtr, N);
  const outBuf = new Float32Array(instance.exports.memory.buffer, outPtr, N);

  return {
    name,
    channels,
    frames,
    sampleRate,
    inBuf,
    outBuf,
    processQuantum() { wam.process(inPtr, outPtr, channels, frames); },
    setParam(id, value) { wam.setParam(String(id), value); },
    getParam(id) { return wam.getParam(String(id)); },
    midi: wam.midi ? ((s, d1, d2, offset = 0) => wam.midi(s, d1, d2, offset)) : undefined,
    reset() { if (wam.reset) wam.reset(); },
    dispose() { try { wam.free(inPtr); wam.free(outPtr); } catch { /* module gone */ } },
    descriptorJson: () => wam.descriptorJson(),
    parametersJson: () => wam.parametersJson(),
    raw: wam,
  };
}

// Browser-side convenience: fetch wasm bytes then build the adapter.
export async function loadWamDspFromUrl(url, opts = {}) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`fetch ${url}: ${res.status}`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  return loadWamDsp(bytes, { name: url.split("/").pop(), ...opts });
}
