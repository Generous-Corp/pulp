// wclap-processor.js — the production worklet-resident WebCLAP host (Architecture A).
//
// A *classic* AudioWorklet script (loaded via `audioWorklet.addModule`), NOT an
// ES module: `addModule` only runs classic scripts, so the ES-module CLAP host
// (core/format/src/wasm/wclap-host.mjs) and the shared ABI table
// (./wclap-abi.mjs) cannot be `import`ed here. This bundle therefore INLINES the
// same CLAP struct offsets / event constants / trampolines as ./wclap-abi.mjs —
// which is the single source of truth; the package test
// test/wclap-abi-parity.test.mjs asserts the two never drift.
//
// It productizes the WS-C2 spike (examples/web-demos/wclap-build/realtime-spike/
// wclap-worklet.js): the whole minimal CLAP host + WASI shim + host-vtable
// trampolines run INSIDE AudioWorkletGlobalScope and render REAL TIME, one
// 128-frame quantum per process() call, with ZERO allocation on the audio thread
// (every wasm buffer, the clap_process_t, and pooled event structs are allocated
// once in prepare()). Beyond the spike it adds, all inside the worklet:
//   • MIDI/sysex IN  — scheduleMidi → clap_event_midi, sendSysex → clap_event_midi_sysex.
//   • param + MIDI OUT — out_events.try_push captures CLAP_EVENT_PARAM_VALUE
//     (onParamsChanged) and note/midi out (onMidiOut). (wclap-host.mjs DROPS these.)
//   • clap.state — save/load via the clap.state extension's ostream/istream
//     vtable, producing/consuming the SAME PLST blob the native builds use.
//   • honest descriptor flags from clap.audio-ports / clap.note-ports.
// Why the whole host lives here: a CLAP plugin's process() calls the host's
// in/out-event vtable SYNCHRONOUSLY during the render; a worklet-side wasm cannot
// synchronously call a main-thread JS vtable. See realtime-spike/DECISION.md.

/* ─────────────────────────── worklet-scope polyfills ─────────────────────── */
// AudioWorkletGlobalScope lacks TextEncoder/TextDecoder/atob; CLAP ids + param
// names are ASCII, so a byte loop is sufficient and never allocates a coder.
function utf8Encode(s) {
  const out = new Uint8Array(s.length + 1); // + NUL
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0x7f;
  return out;
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

/* ───── CLAP ABI — INLINED from ./wclap-abi.mjs (parity test guards drift) ──── */
const CLAP_PLUGIN_FACTORY_ID = "clap.plugin-factory";
const CLAP_EXT_PARAMS = "clap.params";
const CLAP_EXT_STATE = "clap.state";
const CLAP_EXT_AUDIO_PORTS = "clap.audio-ports";
const CLAP_EXT_NOTE_PORTS = "clap.note-ports";
const CLAP_CORE_EVENT_SPACE_ID = 0;
const CLAP_EVENT_NOTE_ON = 0;
const CLAP_EVENT_NOTE_OFF = 1;
const CLAP_EVENT_PARAM_VALUE = 5;
const CLAP_EVENT_MIDI = 10;
const CLAP_EVENT_MIDI_SYSEX = 11;
const CLAP_PARAM_IS_STEPPED = 1 << 0;
const PLUGIN = { init: 8, destroy: 12, activate: 16, deactivate: 20, start_processing: 24, stop_processing: 28, reset: 32, process: 36, get_extension: 40, on_main_thread: 44 };
const FACTORY = { count: 0, descriptor: 4, create: 8 };
const ENTRY = { init: 12, get_factory: 20 };
const DESC = { id: 12, name: 16 };
const PARAM_INFO = { size: 1320, id: 0, flags: 4, name: 12, min: 1296, max: 1304, def: 1312 };
const EVENT_HEADER = { size: 0, time: 4, space_id: 8, type: 10, flags: 12 };
const PARAM_EVENT = { size: 48, param_id: 16, cookie: 20, note_id: 24, port: 28, channel: 30, key: 32, value: 40 };
const NOTE_EVENT = { size: 40, note_id: 16, port: 20, channel: 22, key: 24, velocity: 32 };
const MIDI_EVENT = { size: 24, port: 16, data: 18 };
const SYSEX_EVENT = { size: 28, port: 16, buffer: 20, bytes: 24 };
const IN_EVENTS = { size: 12, ctx: 0, count: 4, get: 8 };
const OUT_EVENTS = { size: 8, ctx: 0, try_push: 4 };
const PROCESS = { size: 40, frames: 8, transport: 12, audio_in: 16, audio_out: 20, in_count: 24, out_count: 28, in_events: 32, out_events: 36 };
const AUDIO_BUFFER = { size: 24, data32: 0, data64: 4, channels: 8, latency: 12, constant_mask: 16 };
const HOST = { size: 48, name: 16, vendor: 20, url: 24, version: 28, get_extension: 32, request_restart: 36, request_process: 40, request_callback: 44 };
const STATE_EXT = { save: 0, load: 4 };
const OSTREAM = { size: 8, ctx: 0, write: 4 };
const ISTREAM = { size: 8, ctx: 0, read: 4 };
const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
  "iiI->I": "AGFzbQEAAAABCAFgA39/fgF+AgcBAWgBZgAAAwIBAAcGAQJmbgABCgwBCgAgACABIAIQAAs=",
};
/* ────────────────────────── end inlined ABI block ──────────────────────────── */

const _trampolineModuleCache = {};
function trampolineModule(key) {
  if (!_trampolineModuleCache[key]) {
    if (!TRAMPOLINES[key]) throw new Error("no WebCLAP host trampoline for '" + key + "'");
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
    view.setUint32(nwrittenPtr, total, true); // CRITICAL: else libc write loop spins
    if (chunks.length && onText) { let t = ""; for (const c of chunks) t += utf8Decode(c); onText(fd, t); }
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

/* ─────────────────── worklet-resident host + real-time plugin ─────────────── */
class WorkletWclapHost {
  constructor(onLog) {
    this.onLog = onLog;
    this.memory = new WebAssembly.Memory({ initial: 512, maximum: 16384, shared: true });
    this.instance = null; this.ex = null;
  }
  instantiateSync(bytes) {
    // Posting a WebAssembly.Module INTO an AudioWorklet is silently dropped in
    // Chrome, so the main thread transfers raw bytes and we compile HERE. A
    // synchronous multi-MB `new WebAssembly.Module` is permitted in
    // AudioWorkletGlobalScope (unlike the main thread's 4 KB sync-compile cap).
    const module = bytes instanceof WebAssembly.Module ? bytes : new WebAssembly.Module(bytes);
    this.instance = new WebAssembly.Instance(module, {
      env: { memory: this.memory },
      wasi_snapshot_preview1: makeWasiImports(() => this.memory, (fd, t) => this.onLog && this.onLog(fd, t)),
    });
    this.ex = this.instance.exports;
    this.ex._initialize();
    return this;
  }
  // Cache a single DataView over the wasm heap. A fresh `new DataView` per
  // accessor call was dozens of short-lived allocations per render quantum on
  // the audio thread (every u32/u16/f64/setU32/… routes through this). Rebuild
  // only when the buffer changes: memory.grow() detaches and replaces a
  // non-shared buffer (identity changes) and extends a shared one (byteLength
  // changes), so covering both keeps a grown heap from being read through a
  // stale view while allocating zero DataViews in steady state.
  get _dv() {
    const buf = this.memory.buffer;
    if (buf !== this._dvBuffer || buf.byteLength !== this._dvByteLength) {
      this._dvBuffer = buf;
      this._dvByteLength = buf.byteLength;
      this._dvView = new DataView(buf);
    }
    return this._dvView;
  }
  u32(p) { return this._dv.getUint32(p, true); }
  setU32(p, v) { this._dv.setUint32(p, v, true); }
  u16(p) { return this._dv.getUint16(p, true); }
  setU16(p, v) { this._dv.setUint16(p, v, true); }
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
    const h = this.ex.malloc(HOST.size);
    this.setU32(h + 0, 1); this.setU32(h + 4, 2); this.setU32(h + 8, 2); // clap_version 1.2.2
    this.setU32(h + 12, 0);                                              // host_data
    this.setU32(h + HOST.name, this.cstr("Pulp WebCLAP RT Host"));
    this.setU32(h + HOST.vendor, this.cstr("Pulp"));
    this.setU32(h + HOST.url, this.cstr("https://github.com/danielraffel/pulp"));
    this.setU32(h + HOST.version, this.cstr("0.0.1"));
    this.setU32(h + HOST.get_extension, this._addFn("ii->i", () => 0)); // host offers none
    const noop = this._addFn("i->", () => {});
    this.setU32(h + HOST.request_restart, noop);
    this.setU32(h + HOST.request_process, noop);
    this.setU32(h + HOST.request_callback, noop);
    return h;
  }
  createPlugin(index = 0) {
    const entry = this.ex.clap_entry.value;
    if (!this.call(this.u32(entry + ENTRY.init), 0)) throw new Error("clap_entry.init() failed");
    const factory = this.call(this.u32(entry + ENTRY.get_factory), this.cstr(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) throw new Error("get_factory returned null");
    const count = this.call(this.u32(factory + FACTORY.count), factory);
    if (index >= count) throw new Error("plugin index out of range");
    const desc = this.call(this.u32(factory + FACTORY.descriptor), factory, index);
    const id = this.readCstr(this.u32(desc + DESC.id));
    const name = this.readCstr(this.u32(desc + DESC.name));
    const ptr = this.call(this.u32(factory + FACTORY.create), factory, this._buildHost(), this.cstr(id));
    if (!ptr) throw new Error("create_plugin returned null");
    return new RealtimeWclapPlugin(this, ptr, { id, name });
  }
}

// A CLAP plugin driven in real time: prepare() allocates every wasm buffer +
// event-struct pool ONCE and calls start_processing once; processQuantum() does
// no allocation.
class RealtimeWclapPlugin {
  constructor(host, ptr, descriptor) {
    this.host = host; this.ptr = ptr; this.descriptor = descriptor;
    this.channels = 2; this.maxFrames = 128;
    this._paramPool = []; this._midiPool = []; this._sysexPool = []; this._sysexBufs = [];
    this._curEvents = [];       // pooled event pointers handed to the plugin this quantum
    this._outParamEvents = [];  // captured host-visible param changes
    this._outMidi = [];         // captured host-visible note/midi output
    this._stateReady = false;
  }
  _fn(off) { return this.host.u32(this.ptr + off); }
  init() { if (!this.host.call(this._fn(PLUGIN.init), this.ptr)) throw new Error("init failed"); return this; }
  activate(sr, minF, maxF) {
    if (!this.host.call(this._fn(PLUGIN.activate), this.ptr, sr, minF, maxF)) throw new Error("activate failed");
    return this;
  }
  _ext(id) { return this.host.call(this._fn(PLUGIN.get_extension), this.ptr, this.host.cstr(id)); }

  // Honest descriptor flags from clap.audio-ports / clap.note-ports.
  descriptorFlags() {
    const h = this.host;
    const portCount = (extId) => {
      const ext = this._ext(extId);
      if (!ext) return { in: 0, out: 0 };
      // clap_plugin_{audio,note}_ports_t.count(plugin, is_input) is at offset 0.
      return { in: h.call(h.u32(ext + 0), this.ptr, 1), out: h.call(h.u32(ext + 0), this.ptr, 0) };
    };
    const audio = portCount(CLAP_EXT_AUDIO_PORTS);
    const note = portCount(CLAP_EXT_NOTE_PORTS);
    const hasAudioInput = audio.in > 0, hasAudioOutput = audio.out > 0;
    const hasMidiInput = note.in > 0, hasMidiOutput = note.out > 0;
    // An instrument takes notes and makes audio without needing an audio input.
    const isInstrument = hasMidiInput && hasAudioOutput && !hasAudioInput;
    return { hasAudioInput, hasAudioOutput, hasMidiInput, hasMidiOutput, isInstrument };
  }

  params() {
    const h = this.host;
    const ext = this._paramsExt = this._ext(CLAP_EXT_PARAMS);
    if (!ext) return (this._paramIds = []);
    const count = h.call(h.u32(ext + 0), this.ptr);   // .count @0
    const buf = h.ex.malloc(PARAM_INFO.size);
    const out = [];
    for (let i = 0; i < count; i++) {
      if (!h.call(h.u32(ext + 4), this.ptr, i, buf)) continue;  // .get_info @4
      const min = h.f64(buf + PARAM_INFO.min), max = h.f64(buf + PARAM_INFO.max);
      const flags = h.u32(buf + PARAM_INFO.flags);
      const stepped = (flags & CLAP_PARAM_IS_STEPPED) !== 0;
      out.push({
        id: h.u32(buf + PARAM_INFO.id),
        name: h.readCstr(buf + PARAM_INFO.name, 256),
        min, max, default: h.f64(buf + PARAM_INFO.def),
        stepped, boolean: stepped && (max - min) === 1,
      });
    }
    h.ex.free(buf);
    this._paramIds = out.map((p) => p.id);
    return out;
  }

  // Read the plugin's CURRENT parameter values via clap.params.get_value (offset
  // 8). Used after a state load to re-sync the host mirror + UI (the plugin does
  // not emit param-out events on load). Returns [{id,value}].
  readParamValues() {
    const h = this.host;
    if (!this._paramsExt || !this._paramIds) return [];
    if (!this._pvBuf) this._pvBuf = h.ex.malloc(8); // reusable double scratch
    const out = [];
    for (const id of this._paramIds) {
      if (h.call(h.u32(this._paramsExt + 8), this.ptr, id, this._pvBuf)) {
        out.push({ id, value: h.f64(this._pvBuf) });
      }
    }
    return out;
  }

  // Allocate every wasm structure ONCE. After this, processQuantum() is
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
    const audioBuf = (chPtrs) => { const p = h.ex.malloc(AUDIO_BUFFER.size);
      h.setU32(p + AUDIO_BUFFER.data32, ptrArray(chPtrs)); h.setU32(p + AUDIO_BUFFER.data64, 0);
      h.setU32(p + AUDIO_BUFFER.channels, chPtrs.length); h.setU32(p + AUDIO_BUFFER.latency, 0);
      h.setU32(p + AUDIO_BUFFER.constant_mask, 0); h.setU32(p + AUDIO_BUFFER.constant_mask + 4, 0); return p; };
    this._inBuf = audioBuf(inPtrs);
    this._outBuf = audioBuf(outPtrs);

    // Reusable in/out event lists. get()/size() index this._curEvents; try_push
    // CAPTURES param + note/midi output events (the "drops output events" fix).
    this._inEvents = h.ex.malloc(IN_EVENTS.size);
    h.setU32(this._inEvents + IN_EVENTS.ctx, 0);
    h.setU32(this._inEvents + IN_EVENTS.count, h._addFn("i->i", () => this._curEvents.length));
    h.setU32(this._inEvents + IN_EVENTS.get, h._addFn("ii->i", (_c, i) => this._curEvents[i] ?? 0));
    this._outEvents = h.ex.malloc(OUT_EVENTS.size);
    h.setU32(this._outEvents + OUT_EVENTS.ctx, 0);
    h.setU32(this._outEvents + OUT_EVENTS.try_push, h._addFn("ii->i", (_c, evPtr) => {
      const type = h.u16(evPtr + EVENT_HEADER.type);
      if (type === CLAP_EVENT_PARAM_VALUE) {
        this._outParamEvents.push({ id: h.u32(evPtr + PARAM_EVENT.param_id), value: h.f64(evPtr + PARAM_EVENT.value) });
      } else if (type === CLAP_EVENT_NOTE_ON || type === CLAP_EVENT_NOTE_OFF) {
        const ch = h.u16(evPtr + NOTE_EVENT.channel) & 0x0f, key = h.u16(evPtr + NOTE_EVENT.key) & 0x7f;
        const vel = Math.max(0, Math.min(127, Math.round(h.f64(evPtr + NOTE_EVENT.velocity) * 127)));
        this._outMidi.push({ bytes: type === CLAP_EVENT_NOTE_ON ? [0x90 | ch, key, vel] : [0x80 | ch, key, 0] });
      } else if (type === CLAP_EVENT_MIDI) {
        this._outMidi.push({ bytes: [
          h._dv.getUint8(evPtr + MIDI_EVENT.data),
          h._dv.getUint8(evPtr + MIDI_EVENT.data + 1),
          h._dv.getUint8(evPtr + MIDI_EVENT.data + 2)] });
      }
      return 1; // accepted
    }));

    // clap_process_t, allocated once and reused.
    this._proc = h.ex.malloc(PROCESS.size);
    h.setU32(this._proc + 0, 0); h.setU32(this._proc + 4, 0);           // steady_time i64
    h.setU32(this._proc + PROCESS.frames, maxFrames);
    h.setU32(this._proc + PROCESS.transport, 0);
    h.setU32(this._proc + PROCESS.audio_in, this._inBuf);
    h.setU32(this._proc + PROCESS.audio_out, this._outBuf);
    h.setU32(this._proc + PROCESS.in_count, 1); h.setU32(this._proc + PROCESS.out_count, 1);
    h.setU32(this._proc + PROCESS.in_events, this._inEvents);
    h.setU32(this._proc + PROCESS.out_events, this._outEvents);

    // Pre-built event-struct pools (filled per quantum, never malloc'd there).
    for (let i = 0; i < 16; i++) this._paramPool.push(h.ex.malloc(PARAM_EVENT.size));
    for (let i = 0; i < 64; i++) this._midiPool.push(h.ex.malloc(MIDI_EVENT.size));
    for (let i = 0; i < 4; i++) {
      this._sysexPool.push(h.ex.malloc(SYSEX_EVENT.size));
      this._sysexBufs.push(h.ex.malloc(512)); // per-slot payload scratch
    }

    if (!h.call(this._fn(PLUGIN.start_processing), this.ptr)) throw new Error("start_processing failed");
    this._started = true;
    return this;
  }

  _writeParamEvent(slot, id, value) {
    const h = this.host, e = this._paramPool[slot];
    h.setU32(e + EVENT_HEADER.size, PARAM_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_PARAM_VALUE);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU32(e + PARAM_EVENT.param_id, id); h.setU32(e + PARAM_EVENT.cookie, 0);
    h._dv.setInt32(e + PARAM_EVENT.note_id, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.port, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.channel, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.key, -1, true);
    h.setF64(e + PARAM_EVENT.value, value);
    return e;
  }
  _writeMidiEvent(slot, bytes) {
    const h = this.host, e = this._midiPool[slot];
    h.setU32(e + EVENT_HEADER.size, MIDI_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_MIDI);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU16(e + MIDI_EVENT.port, 0);
    h._dv.setUint8(e + MIDI_EVENT.data, bytes[0] & 0xff);
    h._dv.setUint8(e + MIDI_EVENT.data + 1, bytes[1] & 0xff);
    h._dv.setUint8(e + MIDI_EVENT.data + 2, bytes[2] & 0xff);
    return e;
  }
  _writeSysexEvent(slot, bytes) {
    const h = this.host, e = this._sysexPool[slot], buf = this._sysexBufs[slot];
    const n = Math.min(bytes.length, 512);
    new Uint8Array(h.memory.buffer, buf, n).set(bytes.subarray ? bytes.subarray(0, n) : bytes.slice(0, n));
    h.setU32(e + EVENT_HEADER.size, SYSEX_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_MIDI_SYSEX);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU16(e + SYSEX_EVENT.port, 0);
    h.setU32(e + SYSEX_EVENT.buffer, buf); h.setU32(e + SYSEX_EVENT.bytes, n);
    return e;
  }

  // Render ONE quantum. inputChannels: Float32Array[]; paramEvents [{id,value}];
  // midiEvents [[status,d1,d2]]; sysexEvents [Uint8Array]. Writes outputChannels.
  // Returns { params: [...], midi: [...] } captured host-visible output events.
  processQuantum(inputChannels, frames, paramEvents, midiEvents, sysexEvents, outputChannels) {
    const h = this.host;
    for (let c = 0; c < this.channels; c++) {
      const src = inputChannels[c] || inputChannels[0];
      new Float32Array(h.memory.buffer, this._inCh[c], frames).set(src.subarray(0, frames));
    }
    // Fill pooled event structs (all latched at frame 0). Reuse this._curEvents
    // in place (length reset + push) so no fresh array is allocated per quantum.
    const cur = this._curEvents; cur.length = 0;
    const np = Math.min(paramEvents.length, this._paramPool.length);
    for (let i = 0; i < np; i++) cur.push(this._writeParamEvent(i, paramEvents[i].id, paramEvents[i].value));
    const nm = Math.min(midiEvents.length, this._midiPool.length);
    for (let i = 0; i < nm; i++) cur.push(this._writeMidiEvent(i, midiEvents[i]));
    const ns = Math.min(sysexEvents.length, this._sysexPool.length);
    for (let i = 0; i < ns; i++) cur.push(this._writeSysexEvent(i, sysexEvents[i]));

    h.setU32(this._proc + PROCESS.frames, frames);
    this._outParamEvents.length = 0; this._outMidi.length = 0;

    const status = h.call(this._fn(PLUGIN.process), this.ptr, this._proc);
    cur.length = 0;
    if (status < 0) throw new Error("process() status " + status);

    for (let c = 0; c < this.channels; c++) {
      const out = new Float32Array(h.memory.buffer, this._outCh[c], frames);
      outputChannels[c].set(out.subarray(0, frames));
    }
    return { params: this._outParamEvents, midi: this._outMidi };
  }

  // ── clap.state (main-thread op; runs in the worklet message loop, never
  //    concurrent with process()). Produces/consumes the SAME PLST blob the
  //    native VST3/AU/CLAP builds serialize. ────────────────────────────────
  _ensureState() {
    if (this._stateReady) return this._stateExt;
    const h = this.host;
    this._stateExt = this._ext(CLAP_EXT_STATE) || 0;
    if (this._stateExt) {
      // ostream/istream structs + funcrefs, built once and reused.
      this._ostream = h.ex.malloc(OSTREAM.size); h.setU32(this._ostream + OSTREAM.ctx, 0);
      h.setU32(this._ostream + OSTREAM.write, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const n = Number(size);
        this._saveChunks.push(new Uint8Array(h.memory.buffer, bufPtr, n).slice());
        return BigInt(n);
      }));
      this._istream = h.ex.malloc(ISTREAM.size); h.setU32(this._istream + ISTREAM.ctx, 0);
      h.setU32(this._istream + ISTREAM.read, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const remaining = this._loadBuf.length - this._loadPos;
        const n = Math.min(remaining, Number(size));
        if (n > 0) { new Uint8Array(h.memory.buffer, bufPtr, n).set(this._loadBuf.subarray(this._loadPos, this._loadPos + n)); this._loadPos += n; }
        return BigInt(n);
      }));
    }
    this._stateReady = true;
    return this._stateExt;
  }
  getState() {
    const h = this.host;
    if (!this._ensureState()) return null; // extension not exposed by this wasm
    this._saveChunks = [];
    const ok = h.call(h.u32(this._stateExt + STATE_EXT.save), this.ptr, this._ostream);
    if (!ok) return new Uint8Array(0);
    let total = 0; for (const c of this._saveChunks) total += c.length;
    const out = new Uint8Array(total); let o = 0;
    for (const c of this._saveChunks) { out.set(c, o); o += c.length; }
    this._saveChunks = null;
    return out;
  }
  setState(bytes) {
    const h = this.host;
    if (!this._ensureState()) return false;
    this._loadBuf = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this._loadPos = 0;
    return !!h.call(h.u32(this._stateExt + STATE_EXT.load), this.ptr, this._istream);
  }
  hasState() { return !!this._ensureState(); }
}

/* ────────────────────────────── the processor ────────────────────────────── */
const EMPTY = []; // shared, never mutated — handed to processQuantum on an idle quantum
class WclapProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions || {};
    this.useInternalTone = o.useInternalTone ?? false; // shared-player default: process routed input
    this.toneHz = o.toneHz || 220; this.toneAmp = o.toneAmp ?? 0.3; this.phase = 0;
    this.diag = o.diag ?? false;
    this.ready = false;
    this.pendingParams = []; this.pendingMidi = []; this.pendingSysex = [];
    this.quanta = 0;
    const reportHz = o.reportHz || 20;
    this.reportEvery = Math.max(1, Math.round(sampleRate / 128 / reportHz));
    this._rmsAcc = 0; this._inAcc = 0; this._rmsN = 0;
    this._toneBuf = [new Float32Array(128), new Float32Array(128)];
    this.port.onmessage = (e) => this._onMessage(e.data);
  }

  _onMessage(msg) {
    if (msg.type === "load") {
      try {
        this.host = new WorkletWclapHost((fd, t) => this.port.postMessage({ type: "log", fd, text: t.replace(/\n$/, "") }));
        this.host.instantiateSync(msg.bytes || msg.module);
        this.plugin = this.host.createPlugin(msg.pluginIndex || 0);
        this.plugin.init();
        this.plugin.activate(sampleRate, 1, 128);
        const flags = this.plugin.descriptorFlags();
        this.paramList = this.plugin.params();
        this.plugin.prepare(2, 128);
        this.ready = true;
        this.port.postMessage({ type: "ready",
          descriptor: { ...this.plugin.descriptor, ...flags, hasState: this.plugin.hasState() },
          params: this.paramList, sampleRate });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String(err && err.stack || err) });
      }
    } else if (msg.type === "param") {
      const ex = this.pendingParams.find((p) => p.id === msg.id);
      if (ex) ex.value = msg.value; else this.pendingParams.push({ id: msg.id, value: msg.value });
    } else if (msg.type === "midi") {
      this.pendingMidi.push(msg.bytes);
    } else if (msg.type === "sysex") {
      this.pendingSysex.push(msg.bytes instanceof Uint8Array ? msg.bytes : new Uint8Array(msg.bytes));
    } else if (msg.type === "getState") {
      let bytes = null, error = null;
      try { bytes = this.plugin ? this.plugin.getState() : null; } catch (e) { error = String(e && e.stack || e); }
      // Clone (a plain number[] payload), do NOT transfer: transferring an
      // ArrayBuffer OUT of an AudioWorkletGlobalScope is unreliable in Chrome
      // (the receiver gets a detached/empty buffer). State blobs are small.
      this.port.postMessage({ type: "stateResult", token: msg.token, bytes: bytes ? Array.from(bytes) : null, error });
    } else if (msg.type === "setState") {
      let ok = false, error = null;
      try { ok = this.plugin ? this.plugin.setState(msg.bytes) : false; } catch (e) { error = String(e && e.message || e); }
      // Re-sync the host mirror + UI to the loaded values (the plugin does not
      // emit param-out events on load), so widgets reflect the restored state.
      if (ok && this.plugin) {
        const changes = this.plugin.readParamValues();
        if (changes.length) this.port.postMessage({ type: "paramsChanged", changes });
      }
      this.port.postMessage({ type: "setStateResult", token: msg.token, ok, error });
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0] ? out[0].length : 128;
    if (!this.ready) { for (const ch of out) ch.fill(0); return true; }

    let inCh;
    if (this.useInternalTone) {
      const inc = (2 * Math.PI * this.toneHz) / sampleRate;
      const L = this._toneBuf[0], R = this._toneBuf[1];
      for (let i = 0; i < frames; i++) { const s = this.toneAmp * Math.sin(this.phase);
        L[i] = R[i] = s; this.phase += inc; if (this.phase > 2 * Math.PI) this.phase -= 2 * Math.PI; }
      inCh = this._toneBuf;
    } else {
      const inp = inputs[0];
      inCh = (inp && inp.length) ? inp : (this._toneBuf[0].fill(0), this._toneBuf[1].fill(0), this._toneBuf);
    }

    // Swap only when there is something to drain, else hand the plugin a shared
    // empty array — so a steady-state quantum allocates nothing on the audio thread.
    let params = EMPTY, midi = EMPTY, sysex = EMPTY;
    if (this.pendingParams.length) { params = this.pendingParams; this.pendingParams = []; }
    if (this.pendingMidi.length) { midi = this.pendingMidi; this.pendingMidi = []; }
    if (this.pendingSysex.length) { sysex = this.pendingSysex; this.pendingSysex = []; }

    const captured = this.plugin.processQuantum(inCh, frames, params, midi, sysex, out);
    if (captured.params.length) this.port.postMessage({ type: "paramsChanged", changes: captured.params.slice() });
    if (captured.midi.length) this.port.postMessage({ type: "midiOut", events: captured.midi.map((m) => ({ bytes: m.bytes.slice() })) });

    if (this.diag) {
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
    }
    return true;
  }
}

registerProcessor("pulp-wclap", WclapProcessor);
