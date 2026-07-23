// A minimal pure-JS WebCLAP host: load a WebCLAP `.wasm`, synthesize the CLAP
// host vtable, and drive a plugin through its full lifecycle (init → factory →
// create → init → activate → process → destroy), including parameter control.
//
// HOW THE HOST CALLBACKS WORK: a CLAP plugin calls host function pointers
// (clap_host_t::get_extension, request_*) that must be wasm-callable funcrefs —
// a plain JS function cannot sit in the module's indirect function table. The
// host vtable is synthesized from JS using tiny per-signature *trampoline*
// modules (see TRAMPOLINES / _addFn below): importing a JS function into wasm
// gives it a funcref identity, so a one-line wasm wrapper that forwards to it is
// a real funcref. This works in EVERY engine with no experimental flag — the
// browser (where `WebAssembly.Function` is unavailable) and Node alike — so NO
// compiled C++ host shim is needed.
//
// This is the engine behind both the Node host runner (wclap_host_runner.mjs)
// and the browser host; it runs unchanged on the main thread in either. All CLAP
// struct offsets below are for the wasm32 ABI and pinned to the CLAP 1.2.x layout
// the WebCLAP module is built against.
import { createWclapMemory, makeWasiImports } from "./wclap-wasi.mjs";

const CLAP_PLUGIN_FACTORY_ID = "clap.plugin-factory";
const CLAP_EXT_PARAMS = "clap.params";
const CLAP_EXT_STATE = "clap.state";
const CLAP_EXT_LOG = "clap.log";
const CLAP_EXT_THREAD_CHECK = "clap.thread-check";
const CLAP_EXT_LATENCY = "clap.latency";
const CLAP_EXT_TAIL = "clap.tail";
const CLAP_EVENT_PARAM_VALUE = 5;
const CLAP_CORE_EVENT_SPACE_ID = 0;

// Host-extension vtable offsets + plugin latency/tail get() — see wclap-abi.mjs
// (the single source of truth) for the canonical definitions these mirror.
const HOST_LOG = { size: 4, log: 0 };
const HOST_THREAD_CHECK = { size: 8, is_main_thread: 0, is_audio_thread: 4 };
const HOST_LATENCY = { size: 4, changed: 0 };
const HOST_TAIL = { size: 4, changed: 0 };
const HOST_STATE = { size: 4, mark_dirty: 0 };
const HOST_PARAMS = { size: 12, rescan: 0, clear: 4, request_flush: 8 };
const PLUGIN_LATENCY = { get: 0 };
const PLUGIN_TAIL = { get: 0 };
// clap_plugin_state_t vtable + the two stream structs it is driven with (wasm32).
// int64 sizes/returns, hence the "iiI->I" trampoline.
const STATE_EXT = { save: 0, load: 4 };
const OSTREAM = { size: 8, ctx: 0, write: 4 };
const ISTREAM = { size: 8, ctx: 0, read: 4 };

// clap_param_info_t (wasm32): id@0, flags@4, cookie@8, name[256]@12,
// module[1024]@268, min@1296, max@1304, default@1312.  Size 1320.
const PARAM_INFO = { size: 1320, id: 0, name: 12, min: 1296, max: 1304, def: 1312 };
// clap_plugin_params_t vtable: count@0, get_info@4, get_value@8, value_to_text@12,
// text_to_value@16, flush@20. value_to_text is CLAP's ONLY display source — the
// param info struct has no unit field (see deriveDisplayUnit in wclap-abi.mjs).
const PARAMS_EXT = { count: 0, get_info: 4, get_value: 8, value_to_text: 12, text_to_value: 16, flush: 20 };
// clap_event_param_value (wasm32): header(16) + param_id@16 + cookie@20 +
// note_id@24 + port_index@28 + channel@30 + key@32 + (pad) + value@40.  Size 48.
const PARAM_EVENT_SIZE = 48;

// Host-callback trampoline modules, one per signature shape used by the CLAP
// host vtable + event lists. Each is a ~48-byte wasm module that imports a JS
// function `h.f` and exports a wasm wrapper `fn` that forwards to it. Source WAT
// (e.g. for "ii->i"):
//   (module (import "h" "f" (func $f (param i32 i32) (result i32)))
//           (func (export "fn") (param i32 i32) (result i32)
//             (call $f (local.get 0) (local.get 1))))
// Keys: "<params>-><result>" where "i" is i32 and "I" is i64. "iiI->I" is the
// clap_ostream.write / clap_istream.read signature (uint64 size, int64 result).
const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
  "ii->": "AGFzbQEAAAABBgFgAn9/AAIHAQFoAWYAAAMCAQAHBgECZm4AAQoKAQgAIAAgARAACw==",
  "iii->": "AGFzbQEAAAABBwFgA39/fwACBwEBaAFmAAADAgEABwYBAmZuAAEKDAEKACAAIAEgAhAACw==",
  "iiI->I": "AGFzbQEAAAABCAFgA39/fgF+AgcBAWgBZgAAAwIBAAcGAQJmbgABCgwBCgAgACABIAIQAAs=",
};

export class WebClapHost {
  constructor({ name = "Pulp WebCLAP Host", vendor = "Pulp",
                url = "https://github.com/Generous-Corp/pulp", version = "0.0.1",
                onLog = null, hooks = {} } = {}) {
    this.meta = { name, vendor, url, version };
    this.onLog = onLog;            // (fd, text) => void
    this.hooks = hooks;            // { onLatencyChanged, onTailChanged, onStateDirty, onParamsRescan, onRequestRestart }
    this.memory = createWclapMemory();
    this.instance = null;
    this.ex = null;
    this.getExtensionLog = [];     // ids the plugin queried (diagnostic)
    this.currentPlugin = null;     // set by createPlugin; host callbacks reach the plugin through it
    // clap.thread-check: true only while the plugin's process() is on the stack.
    this._inAudioThread = false;
    this._mainThreadCallbackPending = false;
    this._hostExtById = null;      // id → vtable ptr, built once in _buildHost
  }

  // ── instantiation ─────────────────────────────────────────────────────────
  async instantiate(wasmBytesOrModule) {
    const module = wasmBytesOrModule instanceof WebAssembly.Module
      ? wasmBytesOrModule
      : await WebAssembly.compile(wasmBytesOrModule);
    const imports = {
      env: { memory: this.memory },
      wasi_snapshot_preview1: makeWasiImports(() => this.memory,
        (fd, text) => this.onLog && this.onLog(fd, text)),
    };
    this.instance = await WebAssembly.instantiate(module, imports);
    this.ex = this.instance.exports;
    this.ex._initialize();           // reactor init (libc/TLS)
    return this;
  }

  // ── low-level memory helpers ──────────────────────────────────────────────
  // Cache a single DataView over the wasm heap. A fresh `new DataView` per
  // accessor call was dozens of short-lived allocations per process() block on
  // the audio thread (every u32/f64/setU32/… goes through this). Rebuild only
  // when the underlying buffer changes: memory.grow() detaches and replaces a
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
  f64(p) { return this._dv.getFloat64(p, true); }
  setF64(p, v) { this._dv.setFloat64(p, v, true); }
  call(idx, ...a) { return this.instance.exports.__indirect_function_table.get(idx)(...a); }

  cstr(s) {
    const bytes = new TextEncoder().encode(s + "\0");
    const p = this.ex.malloc(bytes.length);
    new Uint8Array(this.memory.buffer, p, bytes.length).set(bytes);
    return p;
  }
  readCstr(p, limit = 4096) {
    if (!p) return "";
    const u8 = new Uint8Array(this.memory.buffer);
    let e = p; const end = p + limit;
    while (e < end && u8[e]) e++;
    // .slice() copies into a non-shared ArrayBuffer; TextDecoder.decode rejects
    // a view backed by a SharedArrayBuffer (the memory is shared for threads).
    return new TextDecoder().decode(u8.slice(p, e));
  }
  // Install a JS callback into the module's indirect function table as a
  // wasm-callable funcref, and return its table index.
  //
  // The plugin invokes host callbacks through the table, so a plain JS function
  // cannot be installed directly. `WebAssembly.Function` (type reflection) would
  // wrap one, but it is unavailable in browsers. Instead we use a tiny
  // trampoline module per signature: importing a JS function into wasm gives it
  // a funcref identity, and a one-line wasm wrapper that forwards to that import
  // is a real funcref that works in EVERY engine — no experimental flag, in the
  // browser and in Node alike. The trampoline is instantiated per callback so
  // each table slot wraps its own JS closure.
  // `sig` is either a trampoline key ("iiI->I") or the {parameters, results}
  // shape, which is all-i32 by construction.
  _addFn(sig, fn) {
    const key = typeof sig === "string"
      ? sig
      : `${"i".repeat(sig.parameters.length)}->${sig.results.length ? "i" : ""}`;
    const mod = WebClapHost._trampolineModule(key);
    const inst = new WebAssembly.Instance(mod, { h: { f: fn } });
    const tbl = this.instance.exports.__indirect_function_table;
    const idx = tbl.length;
    tbl.grow(1);
    tbl.set(idx, inst.exports.fn);
    return idx;
  }

  // Lazily compile (and cache) the trampoline module for a signature key.
  static _trampolineModule(key) {
    WebClapHost._trampolineCache ??= {};
    if (!WebClapHost._trampolineCache[key]) {
      const b64 = TRAMPOLINES[key];
      if (!b64) throw new Error(`no WebCLAP host trampoline for signature '${key}'`);
      const bytes = typeof Buffer !== "undefined"
        ? Buffer.from(b64, "base64")
        : Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
      WebClapHost._trampolineCache[key] = new WebAssembly.Module(bytes);
    }
    return WebClapHost._trampolineCache[key];
  }

  // ── build clap_host_t (48 bytes) ──────────────────────────────────────────
  // Mirrors the worklet host's host-vtable (wclap-processor.js): the same set of
  // host-provided extensions a Pulp CLAP plugin queries, wired to the same JS
  // effects. Every extension vtable is built ONCE here so get_extension is a pure
  // id lookup and no host callback allocates a table slot after setup.
  _buildHost() {
    const P1 = { parameters: ["i32"], results: [] };
    const P1R = { parameters: ["i32"], results: ["i32"] };
    const P2 = { parameters: ["i32", "i32"], results: [] };
    const P2R = { parameters: ["i32", "i32"], results: ["i32"] };
    const P3 = { parameters: ["i32", "i32", "i32"], results: [] };

    const h = this.ex.malloc(48);
    this.setU32(h + 0, 1); this.setU32(h + 4, 2); this.setU32(h + 8, 2); // clap_version 1.2.2
    this.setU32(h + 12, 0);                                              // host_data
    this.setU32(h + 16, this.cstr(this.meta.name));
    this.setU32(h + 20, this.cstr(this.meta.vendor));
    this.setU32(h + 24, this.cstr(this.meta.url));
    this.setU32(h + 28, this.cstr(this.meta.version));

    // clap.log
    const logExt = this.ex.malloc(HOST_LOG.size);
    this.setU32(logExt + HOST_LOG.log, this._addFn(P3, (_host, severity, msgPtr) => {
      if (this.onLog) this.onLog(severity >= 3 ? 2 : 1, "[clap] " + this.readCstr(msgPtr, 1024));
    }));
    // clap.thread-check
    const tcExt = this.ex.malloc(HOST_THREAD_CHECK.size);
    this.setU32(tcExt + HOST_THREAD_CHECK.is_main_thread, this._addFn(P1R, () => (this._inAudioThread ? 0 : 1)));
    this.setU32(tcExt + HOST_THREAD_CHECK.is_audio_thread, this._addFn(P1R, () => (this._inAudioThread ? 1 : 0)));
    // clap.latency
    const latExt = this.ex.malloc(HOST_LATENCY.size);
    this.setU32(latExt + HOST_LATENCY.changed, this._addFn(P1, () => {
      const s = this.currentPlugin ? this.currentPlugin.currentLatency() : 0;
      this.hooks.onLatencyChanged && this.hooks.onLatencyChanged(s);
    }));
    // clap.tail
    const tailExt = this.ex.malloc(HOST_TAIL.size);
    this.setU32(tailExt + HOST_TAIL.changed, this._addFn(P1, () => {
      const s = this.currentPlugin ? this.currentPlugin.currentTail() : 0;
      this.hooks.onTailChanged && this.hooks.onTailChanged(s);
    }));
    // clap.state
    const stateExt = this.ex.malloc(HOST_STATE.size);
    this.setU32(stateExt + HOST_STATE.mark_dirty, this._addFn(P1, () => {
      this.hooks.onStateDirty && this.hooks.onStateDirty();
    }));
    // clap.params
    const paramsExt = this.ex.malloc(HOST_PARAMS.size);
    this.setU32(paramsExt + HOST_PARAMS.rescan, this._addFn(P2, (_host, flags) => {
      this.hooks.onParamsRescan && this.hooks.onParamsRescan(flags);
    }));
    this.setU32(paramsExt + HOST_PARAMS.clear, this._addFn(P3, () => {}));
    this.setU32(paramsExt + HOST_PARAMS.request_flush, this._addFn(P1, () => {}));

    this._hostExtById = {
      [CLAP_EXT_LOG]: logExt, [CLAP_EXT_THREAD_CHECK]: tcExt,
      [CLAP_EXT_LATENCY]: latExt, [CLAP_EXT_TAIL]: tailExt,
      [CLAP_EXT_STATE]: stateExt, [CLAP_EXT_PARAMS]: paramsExt,
    };
    this.setU32(h + 32, this._addFn(P2R, (_host, extId) => {
      const id = this.readCstr(extId);
      this.getExtensionLog.push(id);
      return this._hostExtById[id] || 0;
    }));
    this.setU32(h + 36, this._addFn(P1, () => this.hooks.onRequestRestart && this.hooks.onRequestRestart())); // request_restart
    this.setU32(h + 40, this._addFn(P1, () => {}));                                                            // request_process
    this.setU32(h + 44, this._addFn(P1, () => { this._mainThreadCallbackPending = true; }));                   // request_callback
    return h;
  }

  // ── entry → factory → create_plugin ───────────────────────────────────────
  createPlugin(index = 0) {
    const entry = this.ex.clap_entry.value;
    if (!this.call(this.u32(entry + 12), 0)) throw new Error("clap_entry.init() failed");
    const factory = this.call(this.u32(entry + 20), this.cstr(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) throw new Error("get_factory(plugin-factory) returned null");
    const count = this.call(this.u32(factory + 0), factory);
    if (index >= count) throw new Error(`plugin index ${index} >= count ${count}`);
    const desc = this.call(this.u32(factory + 4), factory, index);
    const id = this.readCstr(this.u32(desc + 12));
    const name = this.readCstr(this.u32(desc + 16));
    const hostPtr = this._buildHost();
    const ptr = this.call(this.u32(factory + 8), factory, hostPtr, this.cstr(id));
    if (!ptr) throw new Error(`create_plugin(${id}) returned null`);
    const plugin = new WebClapPlugin(this, ptr, { id, name, count });
    this.currentPlugin = plugin;   // host latency/tail callbacks read the live plugin
    return plugin;
  }
}

// clap_plugin_t fn-pointer offsets (wasm32): init@8, destroy@12, activate@16,
// deactivate@20, start_processing@24, stop_processing@28, reset@32, process@36,
// get_extension@40, on_main_thread@44.
export class WebClapPlugin {
  constructor(host, ptr, descriptor) {
    this.host = host;
    this.ptr = ptr;
    this.descriptor = descriptor;
  }
  _fn(off) { return this.host.u32(this.ptr + off); }

  init() {
    if (!this.host.call(this._fn(8), this.ptr)) throw new Error("plugin.init() failed");
    return this;
  }
  activate(sampleRate, minFrames, maxFrames) {
    if (!this.host.call(this._fn(16), this.ptr, sampleRate, minFrames, maxFrames)) {
      throw new Error("plugin.activate() failed");
    }
    return this;
  }
  destroy() { this.host.call(this._fn(12), this.ptr); }

  // Query a plugin extension by id (get_extension @40). Returns the extension
  // pointer, or 0 if the plugin does not expose it.
  _ext(id) { return this.host.call(this._fn(40), this.ptr, this.host.cstr(id)); }

  // Run the plugin's on_main_thread handler (@44). A Pulp plugin drains its
  // pending latency/tail-change flags here and calls the host latency/tail
  // .changed() callbacks. Invoked by process() after process() unwinds.
  onMainThread() { this.host.call(this._fn(44), this.ptr); }

  // Current plugin-reported latency / tail in samples (clap.latency/clap.tail).
  currentLatency() {
    const h = this.host;
    if (this._latencyExt === undefined) this._latencyExt = this._ext(CLAP_EXT_LATENCY) || 0;
    if (!this._latencyExt) return 0;
    return h.call(h.u32(this._latencyExt + PLUGIN_LATENCY.get), this.ptr) >>> 0;
  }
  currentTail() {
    const h = this.host;
    if (this._tailExt === undefined) this._tailExt = this._ext(CLAP_EXT_TAIL) || 0;
    if (!this._tailExt) return 0;
    return h.call(h.u32(this._tailExt + PLUGIN_TAIL.get), this.ptr) >>> 0;
  }

  // Render a parameter value as a CLAP host displays it — clap_plugin_params
  // .value_to_text(plugin, id, value, buf, size) → "35.00 %". CLAP defines no
  // unit field on clap_param_info, so this is the only display source; a caller
  // that needs the bare unit feeds two probes to deriveDisplayUnit (wclap-abi.mjs).
  // Returns null when the plugin exposes no clap.params / declines the call.
  valueToText(id, value) {
    const h = this.host;
    if (this._paramsExt === undefined) this._paramsExt = this._ext(CLAP_EXT_PARAMS) || 0;
    if (!this._paramsExt) return null;
    const fn = h.u32(this._paramsExt + PARAMS_EXT.value_to_text);
    if (!fn) return null;
    if (!this._textBuf) this._textBuf = h.ex.malloc(256);
    if (!h.call(fn, this.ptr, id, value, this._textBuf, 256)) return null;
    return h.readCstr(this._textBuf, 256);
  }

  // The plugin's CURRENT value for a parameter — clap_plugin_params.get_value().
  // A host cannot mirror this in its own map: the plugin rewrites its OWN
  // parameters on a state load (and on a preset change), and get_value is the only
  // way to learn what they became. Returns null when the plugin exposes no
  // clap.params. Main thread.
  paramValue(id) {
    const h = this.host;
    if (this._paramsExt === undefined) this._paramsExt = this._ext(CLAP_EXT_PARAMS) || 0;
    if (!this._paramsExt) return null;
    const fn = h.u32(this._paramsExt + PARAMS_EXT.get_value);
    if (!fn) return null;
    if (!this._valueBuf) this._valueBuf = h.ex.malloc(8);   // double out-param
    if (!h.call(fn, this.ptr, id, this._valueBuf)) return null;
    return h.f64(this._valueBuf);
  }

  // Query the clap.params extension; returns
  // [{id, name, min, max, default, textProbes}]. `textProbes` are two
  // {value, text} value_to_text renderings the caller turns into a display unit
  // (deriveDisplayUnit) — the same seam the worklet host hands the browser adapter.
  params() {
    const ext = this._paramsExt = this._ext(CLAP_EXT_PARAMS) || 0;
    if (!ext) return [];
    const count = this.host.call(this.host.u32(ext + PARAMS_EXT.count), this.ptr);
    const infoBuf = this.host.ex.malloc(PARAM_INFO.size);
    const out = [];
    for (let i = 0; i < count; i++) {
      if (!this.host.call(this.host.u32(ext + PARAMS_EXT.get_info), this.ptr, i, infoBuf)) continue;
      const id = this.host.u32(infoBuf + PARAM_INFO.id);
      const def = this.host.f64(infoBuf + PARAM_INFO.def);
      const max = this.host.f64(infoBuf + PARAM_INFO.max);
      const min = this.host.f64(infoBuf + PARAM_INFO.min);
      const other = max !== def ? max : min;
      out.push({
        id,
        name: this.host.readCstr(infoBuf + PARAM_INFO.name, 256),
        min, max, default: def,
        textProbes: [{ value: def, text: this.valueToText(id, def) },
                     { value: other, text: this.valueToText(id, other) }],
      });
    }
    this.host.ex.free(infoBuf);
    return out;
  }

  // ── clap.state ────────────────────────────────────────────────────────────
  // The plugin's opaque save/load, driven with the clap_ostream_t / clap_istream_t
  // structs a real CLAP host hands it. What comes out is the SAME versioned,
  // CRC-checked "PLST" blob the native VST3/AU/CLAP builds write — and the same
  // one the WAM lane returns from wam_state_size/wam_read_state, which is why a
  // caller (the page's IR loader, the dual-ABI runner) can speak ONE state format
  // to both ABIs. Main-thread op; never call it concurrently with process().
  _ensureState() {
    if (this._stateReady) return this._stateExt;
    const h = this.host;
    this._stateExt = this._ext(CLAP_EXT_STATE) || 0;
    if (this._stateExt) {
      this._ostream = h.ex.malloc(OSTREAM.size);
      h.setU32(this._ostream + OSTREAM.ctx, 0);
      h.setU32(this._ostream + OSTREAM.write, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const n = Number(size);
        this._saveChunks.push(new Uint8Array(h.memory.buffer, bufPtr, n).slice());
        return BigInt(n);
      }));
      this._istream = h.ex.malloc(ISTREAM.size);
      h.setU32(this._istream + ISTREAM.ctx, 0);
      h.setU32(this._istream + ISTREAM.read, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const remaining = this._loadBuf.length - this._loadPos;
        const n = Math.min(remaining, Number(size));
        if (n > 0) {
          new Uint8Array(h.memory.buffer, bufPtr, n)
            .set(this._loadBuf.subarray(this._loadPos, this._loadPos + n));
          this._loadPos += n;
        }
        return BigInt(n);
      }));
    }
    this._stateReady = true;
    return this._stateExt;
  }

  /** The plugin's state blob, or null when the wasm exposes no clap.state. */
  getState() {
    const h = this.host;
    if (!this._ensureState()) return null;
    this._saveChunks = [];
    const ok = h.call(h.u32(this._stateExt + STATE_EXT.save), this.ptr, this._ostream);
    if (!ok) return new Uint8Array(0);
    let total = 0;
    for (const c of this._saveChunks) total += c.length;
    const out = new Uint8Array(total);
    let o = 0;
    for (const c of this._saveChunks) { out.set(c, o); o += c.length; }
    this._saveChunks = null;
    return out;
  }

  /** Restore a state blob. Returns false when the plugin rejects it (or has no clap.state). */
  setState(bytes) {
    const h = this.host;
    if (!this._ensureState()) return false;
    this._loadBuf = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this._loadPos = 0;
    return !!h.call(h.u32(this._stateExt + STATE_EXT.load), this.ptr, this._istream);
  }

  // Build the reusable in/out event lists ONCE per plugin. The callbacks read
  // mutable host state (this._curEvents), so the funcrefs — which cannot be
  // removed from a wasm table — are allocated a single time and reused across
  // every process() call, instead of leaking three table slots per block.
  _ensureEventLists() {
    if (this._inEvents) return;
    const h = this.host;
    this._curEvents = [];
    this._inEvents = h.ex.malloc(12);
    h.setU32(this._inEvents + 0, 0); // ctx
    h.setU32(this._inEvents + 4, h._addFn({ parameters: ["i32"], results: ["i32"] },
      () => this._curEvents.length));
    h.setU32(this._inEvents + 8, h._addFn({ parameters: ["i32", "i32"], results: ["i32"] },
      (_ctx, i) => this._curEvents[i] ?? 0));
    this._outEvents = h.ex.malloc(8);
    h.setU32(this._outEvents + 0, 0); // ctx
    h.setU32(this._outEvents + 4, h._addFn({ parameters: ["i32", "i32"], results: ["i32"] },
      () => 0)); // try_push: drop output events
  }

  // Render one block. `input` is Float32Array[] per channel; `paramEvents` is
  // [{id, value}] injected as CLAP_EVENT_PARAM_VALUE at frame 0. Returns the
  // output as Float32Array[] per channel.
  //
  // Every wasm allocation made for the call is freed before returning, so
  // process() can be driven per-block in a long-running host (e.g. the browser
  // host on every parameter change) without leaking the wasm heap.
  process(input, frames, { paramEvents = [] } = {}) {
    const h = this.host;
    const channels = input.length;
    this._ensureEventLists();

    const scratch = []; // every per-call malloc, freed in finally
    const alloc = (n) => { const p = h.ex.malloc(n); scratch.push(p); return p; };
    const ptrArray = (ptrs) => {
      const p = alloc(ptrs.length * 4);
      ptrs.forEach((q, i) => h.setU32(p + i * 4, q));
      return p;
    };
    const writeChannels = (data) => data.map((ch) => {
      const p = alloc(frames * 4);
      new Float32Array(h.memory.buffer, p, frames).set(ch.subarray(0, frames));
      return p;
    });

    try {
      const inCh = writeChannels(input);
      const outCh = Array.from({ length: channels }, () => alloc(frames * 4));
      for (const p of outCh) new Float32Array(h.memory.buffer, p, frames).fill(0);

      const audioBuf = (chPtrs) => {
        const p = alloc(24);
        h.setU32(p + 0, ptrArray(chPtrs)); h.setU32(p + 4, 0);
        h.setU32(p + 8, chPtrs.length); h.setU32(p + 12, 0);
        h.setU32(p + 16, 0); h.setU32(p + 20, 0);  // constant_mask
        return p;
      };
      const inBuf = audioBuf(inCh), outBuf = audioBuf(outCh);

      // Param-value events, exposed to the plugin via the reusable in_events list.
      this._curEvents = paramEvents.map(({ id, value }) => {
        const e = alloc(PARAM_EVENT_SIZE);
        h.setU32(e + 0, PARAM_EVENT_SIZE);            // header.size
        h.setU32(e + 4, 0);                           // header.time
        h._dv.setUint16(e + 8, CLAP_CORE_EVENT_SPACE_ID, true);
        h._dv.setUint16(e + 10, CLAP_EVENT_PARAM_VALUE, true);
        h.setU32(e + 12, 0);                          // header.flags
        h.setU32(e + 16, id);                         // param_id
        h.setU32(e + 20, 0);                          // cookie
        h._dv.setInt32(e + 24, -1, true);             // note_id
        h._dv.setInt16(e + 28, -1, true);             // port_index
        h._dv.setInt16(e + 30, -1, true);             // channel
        h._dv.setInt16(e + 32, -1, true);             // key
        h.setF64(e + 40, value);                      // value
        return e;
      });

      // clap_process_t (40 bytes).
      const proc = alloc(40);
      h.setU32(proc + 0, 0); h.setU32(proc + 4, 0);   // steady_time
      h.setU32(proc + 8, frames);
      h.setU32(proc + 12, 0);                          // transport
      h.setU32(proc + 16, inBuf); h.setU32(proc + 20, outBuf);
      h.setU32(proc + 24, 1); h.setU32(proc + 28, 1); // 1 in port, 1 out port
      h.setU32(proc + 32, this._inEvents); h.setU32(proc + 36, this._outEvents);

      if (!h.call(this._fn(24), this.ptr)) throw new Error("start_processing() failed");
      h._inAudioThread = true;
      const status = h.call(this._fn(36), this.ptr, proc);
      h._inAudioThread = false;
      h.call(this._fn(28), this.ptr); // stop_processing
      if (status < 0) throw new Error(`process() returned error status ${status}`);

      // If the plugin requested a host callback during process() (e.g. a latency
      // change), run on_main_thread() now that process() has unwound — the host
      // latency/tail .changed() it triggers is then delivered outside process().
      if (h._mainThreadCallbackPending) {
        h._mainThreadCallbackPending = false;
        this.onMainThread();
      }

      return outCh.map((p) => Float32Array.from(new Float32Array(h.memory.buffer, p, frames)));
    } finally {
      this._curEvents = [];
      for (const p of scratch) h.ex.free(p);
    }
  }
}
