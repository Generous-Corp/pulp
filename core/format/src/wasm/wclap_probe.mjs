// Proof-of-life probe for a WebCLAP (.wasm) module.
//
// Instantiates the module with a host-provided shared memory + minimal WASI
// stubs, runs the reactor initializer, and verifies the exported `clap_entry`
// global resolves to a real clap_plugin_entry_t (CLAP 1.x) whose init / deinit /
// get_factory function pointers are callable through the indirect function table.
//
// This is the WebCLAP analogue of core/format/src/wasm/wam_node_runner.mjs: it
// proves the module is a live, contract-correct WebCLAP without needing a full
// CLAP host. Driving the factory, parameters, and audio is the host harness's
// job — see wclap_host_runner.mjs.
//
// Usage:  node wclap_probe.mjs <path-to.wasm>
// Exit:   0 = PASS, non-zero = fail (prints the reason).
import { readFileSync } from "node:fs";
import { createWclapMemory, makeWasiImports } from "./wclap-wasi.mjs";

const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error("usage: node wclap_probe.mjs <path-to.wasm>");
  process.exit(2);
}

const buf = readFileSync(wasmPath);
const module = await WebAssembly.compile(buf);

// Shared, growable memory sized to the link's --max-memory, plus the shared WASI
// shim (whose fd_write is implemented correctly — a 0-returning stub that omits
// *nwritten makes any logging path spin forever).
const memory = createWclapMemory();
const instance = await WebAssembly.instantiate(module, {
  env: { memory },
  wasi_snapshot_preview1: makeWasiImports(() => memory),
});

const ex = instance.exports;
const required = ["_initialize", "clap_entry", "malloc", "free", "cabi_realloc",
                  "__indirect_function_table"];
const missing = required.filter((n) => !(n in ex));
if (missing.length) {
  console.error(`FAIL: missing exports: ${missing.join(", ")}`);
  process.exit(1);
}

// Reactor init (sets up libc/TLS). Must not trap.
ex._initialize();

// `clap_entry` is an exported global holding the address of the
// clap_plugin_entry_t struct in linear memory.
const entryPtr = ex.clap_entry.value;
if (typeof entryPtr !== "number" || entryPtr <= 0) {
  console.error(`FAIL: clap_entry pointer invalid: ${entryPtr}`);
  process.exit(1);
}

const dv = new DataView(memory.buffer);
// clap_plugin_entry_t layout: clap_version{u32 major,u32 minor,u32 patch} then
// init / deinit / get_factory as function-table indices (i32 each).
const major = dv.getUint32(entryPtr + 0, true);
const minor = dv.getUint32(entryPtr + 4, true);
const patch = dv.getUint32(entryPtr + 8, true);
const initIdx = dv.getUint32(entryPtr + 12, true);
const deinitIdx = dv.getUint32(entryPtr + 16, true);
const factoryIdx = dv.getUint32(entryPtr + 20, true);

const tbl = ex.__indirect_function_table;
const okIdx = (i) => i > 0 && i < tbl.length && typeof tbl.get(i) === "function";
if (major !== 1) {
  console.error(`FAIL: unexpected CLAP major version ${major} (want 1)`);
  process.exit(1);
}
if (!okIdx(initIdx) || !okIdx(deinitIdx) || !okIdx(factoryIdx)) {
  console.error("FAIL: clap_entry function pointers do not resolve in the table");
  process.exit(1);
}

console.log(`clap_entry @${entryPtr}: CLAP ${major}.${minor}.${patch} ` +
            `(init=#${initIdx} deinit=#${deinitIdx} get_factory=#${factoryIdx})`);
console.log(`PASS: ${wasmPath} is a live WebCLAP module`);
