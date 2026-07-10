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
const CLAP_EVENT_PARAM_VALUE = 5;
const CLAP_CORE_EVENT_SPACE_ID = 0;

// clap_param_info_t (wasm32): id@0, flags@4, cookie@8, name[256]@12,
// module[1024]@268, min@1296, max@1304, default@1312.  Size 1320.
const PARAM_INFO = { size: 1320, id: 0, name: 12, min: 1296, max: 1304, def: 1312 };
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
// Keys: "<params>-><result>" where params is "i"/"ii" and result is ""/"i".
const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
};

export class WebClapHost {
  constructor({ name = "Pulp WebCLAP Host", vendor = "Pulp",
                url = "https://github.com/danielraffel/pulp", version = "0.0.1",
                onLog = null } = {}) {
    this.meta = { name, vendor, url, version };
    this.onLog = onLog;            // (fd, text) => void
    this.memory = createWclapMemory();
    this.instance = null;
    this.ex = null;
    this.getExtensionLog = [];
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
  _addFn(sig, fn) {
    const key = `${sig.parameters.length === 2 ? "ii" : "i"}->${sig.results.length ? "i" : ""}`;
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
  _buildHost() {
    const h = this.ex.malloc(48);
    this.setU32(h + 0, 1); this.setU32(h + 4, 2); this.setU32(h + 8, 2); // clap_version 1.2.2
    this.setU32(h + 12, 0);                                              // host_data
    this.setU32(h + 16, this.cstr(this.meta.name));
    this.setU32(h + 20, this.cstr(this.meta.vendor));
    this.setU32(h + 24, this.cstr(this.meta.url));
    this.setU32(h + 28, this.cstr(this.meta.version));
    this.setU32(h + 32, this._addFn({ parameters: ["i32", "i32"], results: ["i32"] },
      (_host, extId) => { this.getExtensionLog.push(this.readCstr(extId)); return 0; }));
    const noop = this._addFn({ parameters: ["i32"], results: [] }, () => {});
    this.setU32(h + 36, noop); this.setU32(h + 40, noop); this.setU32(h + 44, noop);
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
    return new WebClapPlugin(this, ptr, { id, name, count });
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

  // Query the clap.params extension; returns [{id, name, min, max, default}].
  params() {
    const ext = this.host.call(this._fn(40), this.ptr, this.host.cstr(CLAP_EXT_PARAMS));
    if (!ext) return [];
    const count = this.host.call(this.host.u32(ext + 0), this.ptr);
    const infoBuf = this.host.ex.malloc(PARAM_INFO.size);
    const out = [];
    for (let i = 0; i < count; i++) {
      if (!this.host.call(this.host.u32(ext + 4), this.ptr, i, infoBuf)) continue;
      out.push({
        id: this.host.u32(infoBuf + PARAM_INFO.id),
        name: this.host.readCstr(infoBuf + PARAM_INFO.name, 256),
        min: this.host.f64(infoBuf + PARAM_INFO.min),
        max: this.host.f64(infoBuf + PARAM_INFO.max),
        default: this.host.f64(infoBuf + PARAM_INFO.def),
      });
    }
    return out;
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
      const status = h.call(this._fn(36), this.ptr, proc);
      h.call(this._fn(28), this.ptr); // stop_processing
      if (status < 0) throw new Error(`process() returned error status ${status}`);

      return outCh.map((p) => Float32Array.from(new Float32Array(h.memory.buffer, p, frames)));
    } finally {
      this._curEvents = [];
      for (const p of scratch) h.ex.free(p);
    }
  }
}
