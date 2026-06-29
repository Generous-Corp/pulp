// Minimal WASI shim for hosting a WebCLAP module outside a full WASI runtime.
//
// A WebCLAP module built for wasm32-wasi-threads imports `env.memory` (a shared,
// host-supplied memory) plus a handful of `wasi_snapshot_preview1` functions. A
// host that only needs to drive the CLAP API (no real filesystem) can satisfy
// these with light stubs — with ONE catch that is easy to get wrong:
//
//   `fd_write` MUST report the number of bytes written via *nwritten. A stub
//   that returns 0 (success) WITHOUT writing *nwritten makes libc's write loop
//   spin forever ("0 bytes written → retry"). Any plugin code that logs (e.g.
//   Pulp's runtime::log_info during clap_init) then hangs the host. This shim
//   implements fd_write correctly and surfaces stdout/stderr text via onText.
//
// This is shared by wclap_probe.mjs (contract check) and wclap-host.mjs (full
// host), so the correct shim lives in exactly one place.

/// Create a shared WebAssembly.Memory sized for a WebCLAP module.
/// `maximum` must match the module's link-time --max-memory (1 GiB / 64 KiB).
export function createWclapMemory({ initialPages = 512, maxPages = 16384 } = {}) {
  return new WebAssembly.Memory({ initial: initialPages, maximum: maxPages, shared: true });
}

/// Build the import object for instantiating a WebCLAP module.
/// @param getMemory  () => WebAssembly.Memory  (late-bound so the same shim can
///                   be created before the memory if needed).
/// @param onText     (fd, text) => void  optional sink for fd 1/2 writes.
export function makeWasiImports(getMemory, onText = null) {
  const dv = () => new DataView(getMemory().buffer);

  const fd_write = (fd, iovsPtr, iovsLen, nwrittenPtr) => {
    const view = dv();
    let total = 0;
    const chunks = [];
    for (let i = 0; i < iovsLen; i++) {
      const base = view.getUint32(iovsPtr + i * 8, true);
      const len = view.getUint32(iovsPtr + i * 8 + 4, true);
      total += len;
      if (onText && (fd === 1 || fd === 2) && len > 0) {
        chunks.push(new Uint8Array(getMemory().buffer, base, len));
      }
    }
    // CRITICAL: report bytes written, or libc's write loop never terminates.
    view.setUint32(nwrittenPtr, total, true);
    if (chunks.length) {
      const text = chunks.map((c) => new TextDecoder().decode(c)).join("");
      onText(fd, text);
    }
    return 0;
  };

  const wasi = {
    fd_write,
    proc_exit: (code) => { throw new WasiProcExit(code); },
    environ_sizes_get: (countPtr, sizePtr) => {
      const view = dv();
      view.setUint32(countPtr, 0, true);
      view.setUint32(sizePtr, 0, true);
      return 0;
    },
    environ_get: () => 0,
    clock_time_get: () => 0,
    fd_close: () => 0,
    fd_seek: () => 0,
    fd_prestat_get: () => 8,        // WASI EBADF — no preopened dirs
    fd_prestat_dir_name: () => 8,
    path_readlink: () => 8,
    sched_yield: () => 0,
  };

  // Any other preview1 import the module references resolves to a benign no-op
  // returning 0 (WASI success). Real I/O is out of scope for a CLAP-driving host.
  return new Proxy(wasi, { get: (t, p) => (p in t ? t[p] : (() => 0)) });
}

/// Thrown by the proc_exit stub so a caller can distinguish a clean module exit
/// from a host bug.
export class WasiProcExit extends Error {
  constructor(code) {
    super(`wasi proc_exit(${code})`);
    this.code = code;
  }
}
