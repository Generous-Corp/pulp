// WS-C2 SPIKE — worklet-resident WebCLAP host (Architecture A).
//
// This is a *classic* AudioWorklet script (loaded via audioWorklet.addModule),
// NOT an ES module: AudioWorklet.addModule only runs classic scripts, so the
// existing ES-module `wclap-host.mjs` cannot be `import`ed here. This file
// therefore embeds a worklet-safe minimal CLAP host — the SAME trampoline-vtable
// technique and struct offsets as `core/format/src/wasm/wclap-host.mjs`, adapted
// to run entirely inside AudioWorkletGlobalScope and to render REAL TIME, one
// 128-frame quantum per `process()` call, with ZERO allocation on the audio
// thread (all wasm memory is pre-allocated at activate()).
//
// Why the whole host runs here (not on the main thread): a CLAP plugin's
// `process()` calls the host's in/out-event vtable *synchronously* during the
// render. A worklet-side wasm cannot synchronously call a main-thread JS vtable.
// So for real-time WebCLAP the host + WASI shim + trampolines must live inside
// the worklet, driven each quantum. See DECISION.md.
//
// What differs from wclap-host.mjs (all because AudioWorkletGlobalScope lacks
// these globals — and none of them is hard to replace):
//   * TextEncoder / TextDecoder    -> utf8Encode / utf8Decode (ASCII-safe; CLAP
//                                     ids and param names are ASCII).
//   * atob (trampoline b64 decode) -> b64decode (10-line polyfill).
//   * async WebAssembly.instantiate-> sync `new WebAssembly.Instance` over a
//                                     Module compiled on the MAIN thread and
//                                     posted in (heavy compile stays off the
//                                     audio thread; linking a precompiled module
//                                     is a few ms, done before we count quanta).
//   * per-block malloc/free + start/stop_processing each block  ->  buffers and
//     the clap_process_t are allocated ONCE in prepare(); start_processing is
//     called ONCE; processQuantum() only pokes memory + calls the process fn.

/* ─────────────────────────── worklet-scope polyfills ─────────────────────── */
function utf8Encode(s) {
  // ASCII fast-path is sufficient for CLAP extension ids / param names.
  const out = new Uint8Array(s.length + 1); // + NUL
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0x7f;
  return out; // NUL-terminated
}
function utf8Decode(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64decode(b64) {
  const lut = b64decode._lut ??= (() => { const t = new Int16Array(256).fill(-1);
    for (let i = 0; i < B64.length; i++) t[B64.charCodeAt(i)] = i; return t; })();
  const clean = b64.replace(/=+$/, "");
  const out = new Uint8Array((clean.length * 6) >> 3);
  let acc = 0, bits = 0, o = 0;
  for (let i = 0; i < clean.length; i++) {
    acc = (acc << 6) | lut[clean.charCodeAt(i)]; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 0xff; }
  }
  return out;
}

/* ─────────────────── CLAP host vtable trampolines (from wclap-host.mjs) ───── */
const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
};
const _trampolineModuleCache = {};
function trampolineModule(key) {
  if (!_trampolineModuleCache[key]) {
    _trampolineModuleCache[key] = new WebAssembly.Module(b64decode(TRAMPOLINES[key]));
  }
  return _trampolineModuleCache[key];
}

/* ─────────────────────────── WASI shim (from wclap-wasi.mjs) ──────────────── */
function makeWasiImports(getMemory, onText) {
  const dv = () => new DataView(getMemory().buffer);
  const fd_write = (fd, iovsPtr, iovsLen, nwrittenPtr) => {
    const view = dv();
    let total = 0; const chunks = [];
    for (let i = 0; i < iovsLen; i++) {
      const base = view.getUint32(iovsPtr + i * 8, true);
      const len = view.getUint32(iovsPtr + i * 8 + 4, true);
      total += len;
      if (onText && (fd === 1 || fd === 2) && len > 0) {
        chunks.push(new Uint8Array(getMemory().buffer, base, len).slice());
      }
    }
    view.setUint32(nwrittenPtr, total, true); // CRITICAL: or libc write loop spins
    if (chunks.length && onText) {
      let text = ""; for (const c of chunks) text += utf8Decode(c);
      onText(fd, text);
    }
    return 0;
  };
  const wasi = {
    fd_write,
    proc_exit: (code) => { throw new Error("wasi proc_exit(" + code + ")"); },
    environ_sizes_get: (c, s) => { const v = dv(); v.setUint32(c, 0, true); v.setUint32(s, 0, true); return 0; },
    environ_get: () => 0, clock_time_get: () => 0, fd_close: () => 0, fd_seek: () => 0,
    fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8, path_readlink: () => 8, sched_yield: () => 0,
  };
  return new Proxy(wasi, { get: (t, p) => (p in t ? t[p] : (() => 0)) });
}

/* ───────── CLAP struct offsets (wasm32, CLAP 1.2.x) — see wclap-host.mjs ───── */
const CLAP_PLUGIN_FACTORY_ID = "clap.plugin-factory";
const CLAP_EXT_PARAMS = "clap.params";
const CLAP_EVENT_PARAM_VALUE = 5;
const CLAP_CORE_EVENT_SPACE_ID = 0;
const PARAM_INFO = { size: 1320, id: 0, name: 12, min: 1296, max: 1304, def: 1312 };
const PARAM_EVENT_SIZE = 48;

/* ─────────────────── worklet-resident host + real-time plugin ─────────────── */
class WorkletWclapHost {
  constructor(onLog) {
    this.onLog = onLog;
    this.memory = new WebAssembly.Memory({ initial: 512, maximum: 16384, shared: true });
    this.instance = null; this.ex = null;
  }
  instantiateSync(moduleOrBytes) {
    // NB: posting a WebAssembly.Module INTO an AudioWorklet is silently dropped
    // in Chrome (the message never arrives; no throw on the sender). So the main
    // thread transfers the raw bytes and we compile HERE. Synchronous
    // `new WebAssembly.Module` of a multi-MB module is permitted in
    // AudioWorkletGlobalScope (unlike the main thread's 4 KB sync-compile cap).
    const module = moduleOrBytes instanceof WebAssembly.Module
      ? moduleOrBytes
      : new WebAssembly.Module(moduleOrBytes);
    const imports = {
      env: { memory: this.memory },
      wasi_snapshot_preview1: makeWasiImports(() => this.memory,
        (fd, t) => this.onLog && this.onLog(fd, t)),
    };
    this.instance = new WebAssembly.Instance(module, imports);
    this.ex = this.instance.exports;
    this.ex._initialize();
    return this;
  }
  get _dv() { return new DataView(this.memory.buffer); }
  u32(p) { return this._dv.getUint32(p, true); }
  setU32(p, v) { this._dv.setUint32(p, v, true); }
  f64(p) { return this._dv.getFloat64(p, true); }
  setF64(p, v) { this._dv.setFloat64(p, v, true); }
  call(idx, ...a) { return this.ex.__indirect_function_table.get(idx)(...a); }
  cstr(s) {
    const bytes = utf8Encode(s);
    const p = this.ex.malloc(bytes.length);
    new Uint8Array(this.memory.buffer, p, bytes.length).set(bytes);
    return p;
  }
  readCstr(p, limit = 4096) {
    if (!p) return "";
    const u8 = new Uint8Array(this.memory.buffer);
    let e = p; const end = p + limit;
    while (e < end && u8[e]) e++;
    return utf8Decode(u8.slice(p, e));
  }
  _addFn(key, fn) {
    const inst = new WebAssembly.Instance(trampolineModule(key), { h: { f: fn } });
    const tbl = this.ex.__indirect_function_table;
    const idx = tbl.length; tbl.grow(1); tbl.set(idx, inst.exports.fn);
    return idx;
  }
  _buildHost() {
    const h = this.ex.malloc(48);
    this.setU32(h + 0, 1); this.setU32(h + 4, 2); this.setU32(h + 8, 2);
    this.setU32(h + 12, 0);
    this.setU32(h + 16, this.cstr("Pulp WebCLAP RT Host"));
    this.setU32(h + 20, this.cstr("Pulp"));
    this.setU32(h + 24, this.cstr("https://github.com/Generous-Corp/pulp"));
    this.setU32(h + 28, this.cstr("0.0.1"));
    this.setU32(h + 32, this._addFn("ii->i", () => 0)); // get_extension: host offers none
    const noop = this._addFn("i->", () => {});
    this.setU32(h + 36, noop); this.setU32(h + 40, noop); this.setU32(h + 44, noop);
    return h;
  }
  createPlugin(index = 0) {
    const entry = this.ex.clap_entry.value;
    if (!this.call(this.u32(entry + 12), 0)) throw new Error("clap_entry.init() failed");
    const factory = this.call(this.u32(entry + 20), this.cstr(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) throw new Error("get_factory returned null");
    const count = this.call(this.u32(factory + 0), factory);
    if (index >= count) throw new Error("plugin index out of range");
    const desc = this.call(this.u32(factory + 4), factory, index);
    const id = this.readCstr(this.u32(desc + 12));
    const name = this.readCstr(this.u32(desc + 16));
    const ptr = this.call(this.u32(factory + 8), factory, this._buildHost(), this.cstr(id));
    if (!ptr) throw new Error("create_plugin returned null");
    return new RealtimeWclapPlugin(this, ptr, { id, name });
  }
}

// A CLAP plugin driven in real time: prepare() allocates every wasm buffer ONCE
// and calls start_processing once; processQuantum() does no allocation.
class RealtimeWclapPlugin {
  constructor(host, ptr, descriptor) {
    this.host = host; this.ptr = ptr; this.descriptor = descriptor;
    this.channels = 2; this.maxFrames = 128;
    this._paramEventPool = []; // reusable event structs
    this._outParamEvents = []; // captured OUTPUT param events (proves the "drops
                               // output events" gap is fixable worklet-side)
  }
  _fn(off) { return this.host.u32(this.ptr + off); }
  init() { if (!this.host.call(this._fn(8), this.ptr)) throw new Error("init failed"); return this; }
  activate(sr, minF, maxF) {
    if (!this.host.call(this._fn(16), this.ptr, sr, minF, maxF)) throw new Error("activate failed");
    return this;
  }
  params() {
    const h = this.host;
    const ext = h.call(this._fn(40), this.ptr, h.cstr(CLAP_EXT_PARAMS));
    if (!ext) return [];
    const count = h.call(h.u32(ext + 0), this.ptr);
    const buf = h.ex.malloc(PARAM_INFO.size);
    const out = [];
    for (let i = 0; i < count; i++) {
      if (!h.call(h.u32(ext + 4), this.ptr, i, buf)) continue;
      out.push({ id: h.u32(buf + PARAM_INFO.id), name: h.readCstr(buf + PARAM_INFO.name, 256),
        min: h.f64(buf + PARAM_INFO.min), max: h.f64(buf + PARAM_INFO.max),
        default: h.f64(buf + PARAM_INFO.def) });
    }
    h.ex.free(buf);
    return out;
  }

  // Allocate every wasm structure ONCE. After this returns, processQuantum() is
  // allocation-free and audio-thread-safe.
  prepare(channels, maxFrames) {
    const h = this.host;
    this.channels = channels; this.maxFrames = maxFrames;

    const chBuf = (arr) => { const p = h.ex.malloc(maxFrames * 4); arr.push(p); return p; };
    this._inCh = []; this._outCh = [];
    const inPtrs = [], outPtrs = [];
    for (let c = 0; c < channels; c++) { inPtrs.push(chBuf(this._inCh)); outPtrs.push(chBuf(this._outCh)); }

    const ptrArray = (ptrs) => { const p = h.ex.malloc(ptrs.length * 4);
      ptrs.forEach((q, i) => h.setU32(p + i * 4, q)); return p; };
    const audioBuf = (chPtrs) => { const p = h.ex.malloc(24);
      h.setU32(p + 0, ptrArray(chPtrs)); h.setU32(p + 4, 0);
      h.setU32(p + 8, chPtrs.length); h.setU32(p + 12, 0);
      h.setU32(p + 16, 0); h.setU32(p + 20, 0); return p; };
    this._inBuf = audioBuf(inPtrs);
    this._outBuf = audioBuf(outPtrs);

    // Reusable in/out event lists. out_events.try_push now CAPTURES param events
    // instead of dropping them (see _outParamEvents) — the onParamsChanged path.
    this._curEvents = [];
    this._inEvents = h.ex.malloc(12);
    h.setU32(this._inEvents + 0, 0);
    h.setU32(this._inEvents + 4, h._addFn("i->i", () => this._curEvents.length));
    h.setU32(this._inEvents + 8, h._addFn("ii->i", (_ctx, i) => this._curEvents[i] ?? 0));
    this._outEvents = h.ex.malloc(8);
    h.setU32(this._outEvents + 0, 0);
    h.setU32(this._outEvents + 4, h._addFn("ii->i", (_ctx, evPtr) => {
      // Capture output CLAP_EVENT_PARAM_VALUE events (host-visible param changes).
      const type = h._dv.getUint16(evPtr + 10, true);
      if (type === CLAP_EVENT_PARAM_VALUE) {
        this._outParamEvents.push({ id: h.u32(evPtr + 16), value: h.f64(evPtr + 40) });
      }
      return 1; // accepted
    }));

    // clap_process_t, allocated once and reused.
    this._proc = h.ex.malloc(40);
    h.setU32(this._proc + 0, 0); h.setU32(this._proc + 4, 0);
    h.setU32(this._proc + 8, maxFrames);
    h.setU32(this._proc + 12, 0);
    h.setU32(this._proc + 16, this._inBuf); h.setU32(this._proc + 20, this._outBuf);
    h.setU32(this._proc + 24, 1); h.setU32(this._proc + 28, 1);
    h.setU32(this._proc + 32, this._inEvents); h.setU32(this._proc + 36, this._outEvents);

    // A small pool of pre-built param-value event structs, filled per quantum.
    for (let i = 0; i < 16; i++) this._paramEventPool.push(h.ex.malloc(PARAM_EVENT_SIZE));

    if (!h.call(this._fn(24), this.ptr)) throw new Error("start_processing failed"); // ONCE
    this._started = true;
    return this;
  }

  // Fill a pooled param-event struct.
  _writeParamEvent(slot, id, value) {
    const h = this.host, e = this._paramEventPool[slot];
    h.setU32(e + 0, PARAM_EVENT_SIZE); h.setU32(e + 4, 0);
    h._dv.setUint16(e + 8, CLAP_CORE_EVENT_SPACE_ID, true);
    h._dv.setUint16(e + 10, CLAP_EVENT_PARAM_VALUE, true);
    h.setU32(e + 12, 0); h.setU32(e + 16, id); h.setU32(e + 20, 0);
    h._dv.setInt32(e + 24, -1, true); h._dv.setInt16(e + 28, -1, true);
    h._dv.setInt16(e + 30, -1, true); h._dv.setInt16(e + 32, -1, true);
    h.setF64(e + 40, value);
    return e;
  }

  // Render ONE quantum. `inputChannels` are Float32Array[channel] of length
  // `frames`; `paramEvents` are [{id,value}] latched at frame 0. Writes result
  // into `outputChannels` (Float32Array[channel]). No allocation.
  processQuantum(inputChannels, frames, paramEvents, outputChannels) {
    const h = this.host;
    // Copy input into the wasm in-buffers.
    for (let c = 0; c < this.channels; c++) {
      const src = inputChannels[c] || inputChannels[0];
      new Float32Array(h.memory.buffer, this._inCh[c], frames).set(src.subarray(0, frames));
    }
    // Param events (from pool).
    this._curEvents = paramEvents.slice(0, this._paramEventPool.length)
      .map((pe, i) => this._writeParamEvent(i, pe.id, pe.value));
    // frames_count may be < maxFrames on the final quantum.
    h.setU32(this._proc + 8, frames);
    this._outParamEvents.length = 0;

    const status = h.call(this._fn(36), this.ptr, this._proc); // process()
    this._curEvents = [];
    if (status < 0) throw new Error("process() status " + status);

    // Copy wasm out-buffers into the worklet outputs.
    for (let c = 0; c < this.channels; c++) {
      const out = new Float32Array(h.memory.buffer, this._outCh[c], frames);
      outputChannels[c].set(out.subarray(0, frames));
    }
    return this._outParamEvents; // any host-visible param changes this quantum
  }
}

/* ────────────────────────────── the processor ────────────────────────────── */
class WclapProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions || {};
    this.toneHz = o.toneHz || 220;
    this.toneAmp = o.toneAmp ?? 0.3;
    this.useInternalTone = o.useInternalTone ?? true; // PoC: self-contained source
    this.phase = 0;
    this.ready = false;
    this.pendingParams = []; // {id,value} queued from main thread
    this.quanta = 0;
    const reportHz = o.reportHz || 20;
    this.reportEvery = Math.max(1, Math.round(sampleRate / 128 / reportHz)); // ~reportHz meter posts/sec
    this._rmsAcc = 0; this._rmsN = 0; this._inAcc = 0;
    this._toneBuf = [new Float32Array(128), new Float32Array(128)];
    this.port.onmessage = (e) => this._onMessage(e.data);
    this.port.postMessage({ type: "log", fd: 0, text: "[trace] processor constructed" });
  }

  _onMessage(msg) {
    if (msg.type === "load") {
      const trace = (s) => this.port.postMessage({ type: "log", fd: 0, text: "[trace] " + s });
      try {
        trace("load: constructing host");
        this.host = new WorkletWclapHost((fd, t) =>
          this.port.postMessage({ type: "log", fd, text: t.replace(/\n$/, "") }));
        trace("instantiateSync (compiling in worklet)");
        this.host.instantiateSync(msg.bytes || msg.module);
        trace("createPlugin");
        this.plugin = this.host.createPlugin(0);
        trace("init");
        this.plugin.init();
        trace("activate");
        this.plugin.activate(sampleRate, 1, 128);
        trace("params");
        this.paramList = this.plugin.params();
        trace("prepare");
        this.plugin.prepare(2, 128);
        trace("ready!");
        this.ready = true;
        this.port.postMessage({ type: "ready", descriptor: this.plugin.descriptor,
          params: this.paramList, sampleRate });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String(err && err.stack || err) });
      }
    } else if (msg.type === "param") {
      // Latest value per id wins; latched at the next quantum's frame 0.
      const existing = this.pendingParams.find((p) => p.id === msg.id);
      if (existing) existing.value = msg.value; else this.pendingParams.push({ id: msg.id, value: msg.value });
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0] ? out[0].length : 128;
    if (!this.ready) { for (const ch of out) ch.fill(0); return true; }

    // Build the input block: internal test tone (self-contained PoC source) or
    // the routed audio input (effect mode).
    let inCh;
    if (this.useInternalTone) {
      const inc = (2 * Math.PI * this.toneHz) / sampleRate;
      const L = this._toneBuf[0], R = this._toneBuf[1];
      for (let i = 0; i < frames; i++) { const s = this.toneAmp * Math.sin(this.phase);
        L[i] = R[i] = s; this.phase += inc; if (this.phase > 2 * Math.PI) this.phase -= 2 * Math.PI; }
      inCh = this._toneBuf;
    } else {
      const inp = inputs[0];
      inCh = (inp && inp.length) ? inp : this._toneBuf.map((b) => { b.fill(0); return b; });
    }

    // Drain queued param changes into this quantum's event list (latched once).
    const paramEvents = this.pendingParams;
    this.pendingParams = [];

    const outParam = this.plugin.processQuantum(inCh, frames, paramEvents, out);

    // Forward host-visible output param changes (onParamsChanged path).
    if (outParam.length) this.port.postMessage({ type: "paramsChanged", changes: outParam.slice() });

    // Measurement: real-time proof + level.
    this.quanta++;
    let so = 0, si = 0;
    for (let i = 0; i < frames; i++) { so += out[0][i] * out[0][i]; si += inCh[0][i] * inCh[0][i]; }
    this._rmsAcc += so; this._inAcc += si; this._rmsN += frames;
    if ((this.quanta % this.reportEvery) === 0) {
      this.port.postMessage({ type: "meter", quanta: this.quanta,
        outRms: Math.sqrt(this._rmsAcc / this._rmsN), inRms: Math.sqrt(this._inAcc / this._rmsN),
        currentTime, sampleRate });
      this._rmsAcc = 0; this._inAcc = 0; this._rmsN = 0;
    }
    return true;
  }
}

registerProcessor("wclap-rt", WclapProcessor);
