// Shared WAM WASM runtime helpers.
//
// Single source of truth for (a) the WASI/env import stubs needed to
// instantiate a headless Pulp WAM DSP module and (b) the heap marshalling for
// the wam_* C ABI. Consumed by the deterministic Node runner, the AudioWorklet
// processor, and the browser host so the import object and the
// JS<->WASM-heap copying are not re-implemented (and allowed to drift) three
// times.
//
// Pure JS — no Node- or browser-specific APIs — so it loads in Node, a worklet,
// and the main thread alike. In particular it must NOT use TextEncoder/
// TextDecoder, which are undefined in AudioWorkletGlobalScope.

// Minimal UTF-8 codec (AudioWorkletGlobalScope has no TextEncoder/TextDecoder).
function utf8Encode(str) {
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let c = str.codePointAt(i);
    if (c > 0xffff) i++; // surrogate pair consumed
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 0x3f), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
  }
  return Uint8Array.from(out);
}
function utf8Decode(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length;) {
    let c = bytes[i++];
    if (c < 0x80) s += String.fromCodePoint(c);
    else if (c < 0xe0) s += String.fromCodePoint(((c & 0x1f) << 6) | (bytes[i++] & 0x3f));
    else if (c < 0xf0) s += String.fromCodePoint(((c & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f));
    else s += String.fromCodePoint(((c & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f));
  }
  return s;
}

// Build the import object for `new WebAssembly.Instance(module, imports)`.
// `getMemory()` returns the live exported WebAssembly.Memory; it is a callback
// because the module exports its own memory, which only exists after
// instantiation (the import functions are not invoked until runtime).
export function makeWasmImports(getMemory) {
  const dv = () => new DataView(getMemory().buffer);
  return {
    env: {
      _abort_js: () => { throw new Error("wasm called abort_js"); },
      _tzset_js: () => {},
      // Headless DSP buffers are tiny; the module's initial heap is sufficient.
      emscripten_resize_heap: () => false,
    },
    wasi_snapshot_preview1: {
      environ_get: () => 0,
      environ_sizes_get: (cntPtr, sizePtr) => {
        dv().setUint32(cntPtr, 0, true);
        dv().setUint32(sizePtr, 0, true);
        return 0;
      },
      fd_close: () => 0,
      fd_seek: () => 0,
      // Discard writes (runtime::log_info) but report them fully consumed.
      fd_write: (fd, iovPtr, iovCnt, nwrittenPtr) => {
        const d = dv();
        let total = 0;
        for (let i = 0; i < iovCnt; i++) total += d.getUint32(iovPtr + i * 8 + 4, true);
        d.setUint32(nwrittenPtr, total, true);
        return 0;
      },
    },
  };
}

// AudioWorklet processor names are global to an AudioContext, so every Pulp
// plugin registering the same literal name meant two DIFFERENT plugins could not
// coexist in one context: the second AudioWorkletNode silently bound to the
// first plugin's DSP (a chained synth would render the MIDI effect's — silent —
// output). Derive the name from the processor module's own absolute URL instead.
// Each plugin ships its own copy of wam-processor.js next to its wam-dsp.js, so
// the URLs differ and the names do too; loading the SAME plugin twice reuses one
// name, which is correct (registerProcessor runs once per module evaluation).
//
// The worklet passes `import.meta.url`; the main thread passes the same URL
// resolved against the document base. Both call this one function so they cannot
// disagree. FNV-1a/32 — no crypto needed, just a stable short token.
export function processorNameForUrl(url) {
  let hash = 0x811c9dc5;
  for (let i = 0; i < url.length; i++) {
    hash ^= url.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return `pulp-wam-processor-${hash.toString(16).padStart(8, "0")}`;
}

// Decode the packed record stream written by wam_midi_out_drain:
//   [int32 sample_offset][uint16 byte_len][byte_len raw MIDI bytes] ...
// `bytes` is a Uint8Array view of the drain buffer; `len` is the byte count
// actually copied (min(cap, available)). A trailing partial record — which is
// what truncation looks like — is dropped rather than mis-decoded.
export function parseMidiOutRecords(bytes, len) {
  const events = [];
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let i = 0;
  while (i + 6 <= len) {
    const offset = dv.getInt32(i, true);
    const size = dv.getUint16(i + 4, true);
    if (i + 6 + size > len) break;                 // truncated tail
    events.push({ offset, bytes: bytes.slice(i + 6, i + 6 + size) });
    i += 6 + size;
  }
  return events;
}

// Wrap an instantiated module's exports with typed-array views and the wam_*
// ABI. All heap access goes through fresh views because memory growth
// invalidates old ArrayBuffer references.
export function makeBridge(exports) {
  const mem = () => exports.memory;
  // Cache the heap views instead of rebuilding a Uint8Array/Float32Array on
  // every call. These are hit on the audio render thread — f32() on the planar
  // copy in and copy out (see makeWamAudioPorts) and u8() on every MIDI drain —
  // so a fresh view per call was steady per-block GC pressure. Rebuild only when
  // the heap buffer changes: ALLOW_MEMORY_GROWTH swaps the ArrayBuffer (identity
  // changes) and a shared/growable buffer extends in place (byteLength changes),
  // so covering both never reads a grown heap through a stale, too-short view.
  let viewBuffer = null;
  let viewByteLength = -1;
  let u8View = null;
  let f32View = null;
  const refreshViews = () => {
    const buf = mem().buffer;
    if (buf !== viewBuffer || buf.byteLength !== viewByteLength) {
      viewBuffer = buf;
      viewByteLength = buf.byteLength;
      u8View = new Uint8Array(buf);
      f32View = new Float32Array(buf);
    }
  };
  const u8 = () => { refreshViews(); return u8View; };
  const f32 = () => { refreshViews(); return f32View; };
  const writeCStr = (s) => {
    const bytes = utf8Encode(s + "\0");
    const p = exports.malloc(bytes.length);
    u8().set(bytes, p);
    return p;
  };
  const readCStr = (p) => {
    const h = u8();
    let e = p;
    while (h[e] !== 0) e++;
    return utf8Decode(h.subarray(p, e));
  };

  const bridge = {
    exports, u8, f32, writeCStr, readCStr,
    malloc: (n) => exports.malloc(n),
    free: (p) => exports.free(p),
    // --no-entry build: run static constructors (constructs the global bridge).
    callCtors() { if (exports.__wasm_call_ctors) exports.__wasm_call_ctors(); },
    init(sampleRate, blockSize) { return exports.wam_init(sampleRate, blockSize) !== 0; },
    // PLANAR channel-pointer ABI (matches WCLAP / native CLAP). `inPtr`/`outPtr`
    // are wasm pointer ARRAYS — one channel-buffer pointer per channel — laid out
    // by makeWamAudioPorts. wam_process reads/writes those channel buffers
    // directly; there is no interleave round-trip.
    process(inPtr, outPtr, channels, frames) { exports.wam_process(inPtr, outPtr, channels, frames); },
    setParam(id, value) { const p = writeCStr(id); exports.wam_set_param(p, value); exports.free(p); },
    getParam(id) { const p = writeCStr(id); const v = exports.wam_get_param(p); exports.free(p); return v; },
    midi(status, d1, d2, offset) { exports.wam_midi(status, d1, d2, offset); },
    // Full F0..F7 payload. Returns false when the plugin dropped it.
    sysex(bytes, offset = 0) {
      const p = exports.malloc(bytes.length);
      u8().set(bytes, p);
      const ok = exports.wam_midi_sysex(p, bytes.length, offset | 0) !== 0;
      exports.free(p);
      return ok;
    },
    // Copy the last block's MIDI output into `dstPtr` (a caller-owned wasm
    // allocation of `cap` bytes). Returns bytes AVAILABLE — greater than `cap`
    // means the tail was truncated.
    drainMidiOut(dstPtr, cap) { return exports.wam_midi_out_drain(dstPtr, cap); },
    // One-shot DSP-state reset: the NEXT process() sees ctx.reset_requested.
    reset() { exports.wam_reset(); },
    // Re-prepare for a sample-rate / block-size change. Serviced between render
    // quanta (the worklet's port.onmessage runs on the audio render thread, not
    // a separate control thread) — never between the interleave and process().
    prepare(sampleRate, blockSize) { exports.wam_prepare(sampleRate, blockSize); },
    // Processor-reported delay-compensation latency, in samples.
    latencySamples() { return exports.wam_latency_samples(); },
    // Push a host transport snapshot (Web Audio has none of its own); it is
    // copied into ProcessContext at the top of each process().
    setTransport(isPlaying, bpm, positionBeats, positionSamples, tsigNum, tsigDen) {
      exports.wam_set_transport(isPlaying ? 1 : 0, bpm, positionBeats,
                                positionSamples, tsigNum | 0, tsigDen | 0);
    },
    // Monotonic counter of parameter changes, the plugin's own writes included.
    paramEpoch() { return exports.wam_param_epoch() >>> 0; },
    // Bulk value read in parametersJson() order. `dstPtr` is a caller-owned wasm
    // allocation of `capacity` floats; returns the total parameter count.
    readParamValues(dstPtr, capacity) {
      return exports.wam_read_param_values(dstPtr, capacity);
    },
    descriptorJson() { return readCStr(exports.wam_descriptor()); },
    parametersJson() { return readCStr(exports.wam_parameters()); },
    readState() {
      const sz = exports.wam_state_size();
      const p = exports.malloc(sz);
      exports.wam_read_state(p);
      const bytes = u8().slice(p, p + sz);
      exports.free(p);
      return bytes;
    },
    writeState(bytes) {
      const p = exports.malloc(bytes.length);
      u8().set(bytes, p);
      const ok = exports.wam_write_state(p, bytes.length);
      exports.free(p);
      return ok === 1;
    },
  };

  // Version-skew safety: several exports postdate the first WAM ABI. If a DSP
  // module was built before them, its bridge method would call an absent export
  // and throw. Drop each such method so callers can feature-detect it the
  // obvious way (`if (wam.drainMidiOut)`), and the worklet degrades instead of
  // crashing on the audio thread. A DSP built from this tree has them all.
  const optional = {
    sysex: "wam_midi_sysex",
    drainMidiOut: "wam_midi_out_drain",
    reset: "wam_reset",
    prepare: "wam_prepare",
    latencySamples: "wam_latency_samples",
    setTransport: "wam_set_transport",
    paramEpoch: "wam_param_epoch",
    readParamValues: "wam_read_param_values",
  };
  for (const [method, exportName] of Object.entries(optional)) {
    if (typeof exports[exportName] !== "function") delete bridge[method];
  }
  return bridge;
}

// Planar audio ports for the wam_process ABI — the single place the JS<->wasm
// audio marshalling lives, so the worklet and the Node runners cannot lay the
// buffers out differently.
//
// Web Audio hands JS planar per-channel Float32Arrays; the Pulp Processor is
// planar; WCLAP and native CLAP pass planar channel-pointer ARRAYS. So the WAM
// ABI is planar too: wam_process(inPtr, outPtr, channels, frames), where each
// *Ptr is a wasm array of `maxChannels` pointers, one per channel buffer. This
// owns those buffers + the two pointer arrays for the life of the module, so a
// caller copies WHOLE channels in/out (a straight typed-array copy — no
// per-sample interleave transpose) instead of re-implementing the layout.
//
// Emscripten's malloc never relocates a live allocation, so the channel
// addresses written into the pointer arrays here stay valid even if the heap
// grows later; only the cached typed-array views (bridge.f32()) are refreshed.
export function makeWamAudioPorts(bridge, maxChannels, maxFrames) {
  const inBufs = [], outBufs = [];
  for (let c = 0; c < maxChannels; c++) {
    inBufs.push(bridge.malloc(maxFrames * 4));
    outBufs.push(bridge.malloc(maxFrames * 4));
  }
  const inPtr = bridge.malloc(maxChannels * 4);
  const outPtr = bridge.malloc(maxChannels * 4);
  {
    // One-time, off the audio thread: stamp the channel pointers into the two
    // pointer arrays. A throwaway DataView is fine here (not a hot path).
    const dv = new DataView(bridge.exports.memory.buffer);
    for (let c = 0; c < maxChannels; c++) {
      dv.setUint32(inPtr + c * 4, inBufs[c], true);
      dv.setUint32(outPtr + c * 4, outBufs[c], true);
    }
  }
  return {
    inPtr, outPtr, maxChannels, maxFrames,
    // Copy each source channel (a Float32Array) into its wasm channel buffer.
    // A straight per-channel copy — no interleave. A missing source channel is
    // zero-filled so a stale block never leaks through.
    writeInput(channels, frames) {
      const h = bridge.f32();
      for (let c = 0; c < maxChannels; c++) {
        const base = inBufs[c] >> 2;
        const src = channels && channels[c];
        if (src) h.set(src.subarray(0, frames), base);
        else h.fill(0, base, base + frames);
      }
    },
    // Copy the wasm output channels back into the caller's per-channel
    // Float32Arrays (min of what the caller has and what we own).
    readInto(channels, frames) {
      const h = bridge.f32();
      const n = Math.min(channels.length, maxChannels);
      for (let c = 0; c < n; c++) {
        const base = outBufs[c] >> 2;
        channels[c].set(h.subarray(base, base + frames));
      }
    },
    // Random-access sample helpers (used by the deterministic Node runners).
    setInputSample(c, f, v) { bridge.f32()[(inBufs[c] >> 2) + f] = v; },
    outputSample(c, f) { return bridge.f32()[(outBufs[c] >> 2) + f]; },
  };
}
