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

// Wrap an instantiated module's exports with typed-array views and the wam_*
// ABI. All heap access goes through fresh views because memory growth
// invalidates old ArrayBuffer references.
export function makeBridge(exports) {
  const mem = () => exports.memory;
  const u8 = () => new Uint8Array(mem().buffer);
  const f32 = () => new Float32Array(mem().buffer);
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

  return {
    exports, u8, f32, writeCStr, readCStr,
    malloc: (n) => exports.malloc(n),
    free: (p) => exports.free(p),
    // --no-entry build: run static constructors (constructs the global bridge).
    callCtors() { if (exports.__wasm_call_ctors) exports.__wasm_call_ctors(); },
    init(sampleRate, blockSize) { return exports.wam_init(sampleRate, blockSize) !== 0; },
    process(inPtr, outPtr, channels, frames) { exports.wam_process(inPtr, outPtr, channels, frames); },
    setParam(id, value) { const p = writeCStr(id); exports.wam_set_param(p, value); exports.free(p); },
    getParam(id) { const p = writeCStr(id); const v = exports.wam_get_param(p); exports.free(p); return v; },
    midi(status, d1, d2, offset) { exports.wam_midi(status, d1, d2, offset); },
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
}
