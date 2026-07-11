// wclap-host-extensions.test.mjs — proves the WebCLAP host answers the CLAP
// host-provided extensions a Pulp CLAP plugin queries (MF-7 parity), driving the
// REAL host-vtable trampolines through a real WebAssembly.Table + Memory. No
// WebCLAP .wasm and no browser are needed: the CLAP struct marshaling and the
// funcref-identity trampolines are exercised directly.
//
//   Run:  node core/format/src/wasm/wclap-host-extensions.test.mjs   (exit 0 = PASS)
//
// The offline host (wclap-host.mjs) and the worklet host (wclap-processor.js)
// build the SAME host vtable with the SAME ABI offsets + trampolines; testing the
// importable offline host validates the shared design (the worklet's inlined ABI
// is guarded against drift by the package parity test).

import { WebClapHost } from "./wclap-host.mjs";

let failed = 0;
const ok = (cond, msg) => { console.log(`${cond ? "  ok  " : "FAIL  "}${msg}`); if (!cond) failed++; };
const eq = (a, b, msg) => ok(a === b, `${msg} (got ${JSON.stringify(a)}, want ${JSON.stringify(b)})`);

// ── Stub a minimal wasm environment: real growable funcref Table (so the
// host-callback trampolines install + invoke exactly as in a real module), a
// bump allocator, and a plain Memory. This is all _buildHost() and the vtable
// funcrefs touch. ──────────────────────────────────────────────────────────
const logs = [];
const events = { latency: [], tail: [], stateDirty: 0, rescan: [], restart: 0 };
const host = new WebClapHost({
  onLog: (fd, text) => logs.push([fd, text]),
  hooks: {
    onLatencyChanged: (s) => events.latency.push(s),
    onTailChanged: (s) => events.tail.push(s),
    onStateDirty: () => events.stateDirty++,
    onParamsRescan: (flags) => events.rescan.push(flags),
    onRequestRestart: () => events.restart++,
  },
});
host.memory = new WebAssembly.Memory({ initial: 32 }); // non-shared; fresh heap
const table = new WebAssembly.Table({ initial: 1, element: "anyfunc" }); // idx 0 reserved
let bump = 1024;
const malloc = (n) => { const p = bump; bump = (bump + (n || 0) + 7) & ~7; return p; };
host.instance = { exports: { __indirect_function_table: table, malloc, free: () => {} } };
host.ex = host.instance.exports;
// A stub plugin the host latency/tail callbacks read through.
host.currentPlugin = { currentLatency: () => 512, currentTail: () => 9 };

const h = host._buildHost();
ok(h > 0, "_buildHost() returns a host struct pointer");

// clap_host_t.get_extension @32
const getExt = (id) => host.call(host.u32(h + 32), h, host.cstr(id));

// ── 1. get_extension returns a stable, non-null vtable for each supported id,
//       and null for an unsupported one.
for (const id of ["clap.log", "clap.thread-check", "clap.latency", "clap.tail", "clap.state", "clap.params"]) {
  const p = getExt(id);
  ok(p > 0, `get_extension("${id}") returns a vtable`);
  eq(getExt(id), p, `get_extension("${id}") is stable across calls`);
  eq(p, host._hostExtById[id], `get_extension("${id}") matches the built vtable`);
}
eq(getExt("clap.gui"), 0, 'get_extension("clap.gui") returns null (unsupported)');

// ── 2. clap.log routes the plugin message to onLog, ERROR severity → fd 2.
const logExt = getExt("clap.log");
host.call(host.u32(logExt + 0), h, 3 /*ERROR*/, host.cstr("boom"));
host.call(host.u32(logExt + 0), h, 1 /*INFO*/, host.cstr("hi"));
eq(logs.length, 2, "clap.log delivered two messages");
ok(logs[0][0] === 2 && logs[0][1] === "[clap] boom", "ERROR log → fd 2, prefixed text");
ok(logs[1][0] === 1 && logs[1][1] === "[clap] hi", "INFO log → fd 1, prefixed text");

// ── 3. clap.thread-check reflects the audio-thread flag both ways.
const tc = getExt("clap.thread-check");
const isMain = () => host.call(host.u32(tc + 0), h);
const isAudio = () => host.call(host.u32(tc + 4), h);
host._inAudioThread = false;
ok(isMain() === 1 && isAudio() === 0, "outside process(): main-thread, not audio-thread");
host._inAudioThread = true;
ok(isMain() === 0 && isAudio() === 1, "inside process(): audio-thread, not main-thread");
host._inAudioThread = false;

// ── 4. clap.latency / clap.tail changed() re-read the plugin and fire the hook.
host.call(host.u32(getExt("clap.latency") + 0), h);
host.call(host.u32(getExt("clap.tail") + 0), h);
eq(JSON.stringify(events.latency), JSON.stringify([512]), "latency.changed → plugin latency forwarded");
eq(JSON.stringify(events.tail), JSON.stringify([9]), "tail.changed → plugin tail forwarded");

// ── 5. clap.state mark_dirty fires the hook.
host.call(host.u32(getExt("clap.state") + 0), h);
eq(events.stateDirty, 1, "state.mark_dirty → onStateDirty");

// ── 6. clap.params: rescan carries flags; clear + request_flush are safe no-ops.
const params = getExt("clap.params");
host.call(host.u32(params + 0), h, 7 /*flags*/);          // rescan
host.call(host.u32(params + 4), h, 3 /*param_id*/, 1);    // clear
host.call(host.u32(params + 8), h);                       // request_flush
eq(JSON.stringify(events.rescan), JSON.stringify([7]), "params.rescan → flags forwarded");

// ── 7. request_restart fires its hook; request_callback only sets the deferred
//       flag the render loop drains (never runs on_main_thread reentrantly).
host.call(host.u32(h + 36), h); // request_restart
eq(events.restart, 1, "request_restart → onRequestRestart");
host._mainThreadCallbackPending = false;
host.call(host.u32(h + 44), h); // request_callback
eq(host._mainThreadCallbackPending, true, "request_callback → sets pending main-thread flag");
host.call(host.u32(h + 40), h); // request_process (no-op, must not throw)

console.log(failed ? `\n${failed} check(s) FAILED` : "\nAll WebCLAP host-extension checks passed.");
process.exit(failed ? 1 : 0);
