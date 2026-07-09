# WS-C2 — Real-time WebCLAP architecture decision + PoC

**Date:** 2026-07-09
**Status:** DECISION + working proof-of-concept (this directory)
**Scope:** make WebCLAP run in REAL TIME behind the same shared demo player as WAM.
**Source of truth for the task:** master plan §3 (WS-C) + §9 (Codex corrections, which
win) in `pulp-web-abi/planning/2026-07-09-web-demos-and-framework-master-plan.md`.
**Also consulted:** `pulp-async-audit/planning/2026-07-09-ws-c2-webclap-architecture-constraints.md`
(the async-primitive audit's WS-C2 feedback) — its analysis is adopted; see §6.

---

## 1. Decision — **Architecture A: worklet-resident CLAP host.**

The whole CLAP host (the minimal WebClapHost + WASI shim + host-vtable
trampolines) runs **inside the AudioWorkletGlobalScope** and is driven one
128-frame quantum per `process()` call. Parameters (and, later, MIDI/state)
arrive over the node's `MessagePort` and are applied as CLAP events on the next
quantum. This is implemented and measured in this directory.

Option B (host in a Worker → SharedArrayBuffer ring → AudioWorklet drain) is
**rejected as the first build**, and reframed (per the async-audit feedback) not
as a separate architecture but as the `lead >= 1` setting of the *same* seam that
A implements at `lead == 0`. It is deferred until a *measured* per-plugin CPU
reason forces it.

### Why A (evidence)

| Axis | Worklet-resident (A) — CHOSEN | Worker + SAB ring (B) |
|---|---|---|
| **CLAP model fit** | CLAP `process()` is a per-block pull; AudioWorklet calls `process()` every 128-frame quantum. 1:1 map, synchronous, sample-locked. | Same DSP, but interposed behind a ring; block boundaries decouple from the audio clock. |
| **Host-vtable calls during `process()`** | In-scope: the in/out event vtable is JS in the *same* thread the wasm runs on. No cross-thread call. | The Codex blocker: a worklet-side wasm cannot synchronously call the main-thread host vtable — B only avoids this by moving the *whole* host to the worker, i.e. it is A-in-a-worker plus a ring. |
| **Latency** | None added — output is the current quantum. | Adds `lead` blocks of latency; bad for a MIDI/keyboard-driven instrument. |
| **Real-time safety** | No alloc in `process()` (all wasm buffers pre-allocated at `activate`); no GC pressure. Matches the audio-thread rules. | Same DSP cost **plus** a lock-free SPSC ring + underrun handling. `Atomics.wait` is **banned in the worklet**, so the consumer/producer must poll — more code, more failure modes. |
| **Browser support** | Needs COI + shared memory (already solved via Cloudflare `_headers`). The worklet module is a **classic script** — the only worklet module type, supported everywhere incl. Safari. | Needs the *same* COI + shared memory, so buys nothing on the matrix; adds SAB-ring correctness across Chrome/FF/Safari. |
| **Precedent** | **WAM v2** runs its wasm DSP in the `AudioWorkletProcessor`; **Signalsmith `wasm-clap-browserhost`** (a real CLAP→wasm browser host) is worklet-resident. Neither uses a Worker→ring hop. | No published browser precedent; no published benchmark vs A. |
| **"Identical to WAM"** | WAM is worklet-resident; matching it makes WebCLAP structurally the same (wasm in the worklet, sample-locked). | Would make WebCLAP structurally *different* from WAM — the opposite of the goal. |

**Net:** A is lower-latency, simpler, equal on browser support, matches both real
precedents and WAM itself, and is the mandatory floor anyway (see §6). B is a
later, measured-only refinement behind the same seam.

---

## 2. Does the host machinery even RUN in an AudioWorkletGlobalScope?

Yes — with three trivial substitutions. Audited what `wclap-host.mjs` /
`wclap-wasi.mjs` touch:

| Needs | In AudioWorkletGlobalScope? | Resolution in the PoC |
|---|---|---|
| `WebAssembly.Module/Instance/Memory/Table` | **Yes** | used directly |
| `DataView`, typed arrays | **Yes** | used directly |
| shared `WebAssembly.Memory({shared:true})` | **Yes on a COI page** | created *inside* the worklet |
| `TextEncoder` (`cstr`) | **No** | `utf8Encode` (ASCII; CLAP ids/names are ASCII) |
| `TextDecoder` (`readCstr`, `fd_write`) | **No** | `utf8Decode` |
| `atob` (trampoline b64 decode) | **No** | 10-line `b64decode` |
| `fetch` (module bytes) | **No** | main thread fetches; **bytes transferred in** |
| ES-module `import`/`export` | **No** (`addModule` runs classic scripts) | the worklet is a **self-contained classic script** |

None is a blocker. The ES-module constraint is the only structural one and it is
minor: `wclap-host.mjs` cannot be `import`ed into a worklet, so the host logic is
shipped as a classic worklet bundle. (Productionizing: factor the struct
offsets + host/WASI logic into a worklet-safe core that both the `.mjs`
main-thread/offline host and the classic worklet bundle build from — see §5.)

---

## 3. PoC result (measured, headless system Chrome, COI local server)

Run: `node validate.mjs` (starts the COOP/COEP server, drives headless Chrome).

```
crossOriginIsolated: true
plugin: PulpGain (com.pulp.gain); params: Input Gain, Output Gain, Bypass
real-time: Δquanta=437 over 1.20s wall (ctxΔ=1.17s); expected ~452 → ratio 0.968
baseline: in=0.212 out=0.212 Δ=0.00dB
after Input Gain=+6: out=0.422 Δ=6.00dB (rise 6.00dB)
PASS: WebCLAP renders REAL-TIME in an AudioWorklet and responds to a param change.
```

- **Real-time, not offline:** over 1.20 s of wall clock the worklet ran **437
  quanta** ≈ the expected `sampleRate/128 × wall` (**ratio 0.968**), and the
  `AudioContext` clock advanced in lockstep (`ctxΔ 1.17 s`). An offline/
  free-running render would not track the wall clock.
- **The plugin is hosted INSIDE the worklet:** activate + `clap_init`
  (`[pulp:info] CLAP: initialized 'PulpGain'`) all run in `AudioWorkletGlobalScope`.
- **Param audibly changes output:** default = unity passthrough (0.00 dB);
  driving the generated **Input Gain** control to +6 dB lifts output **+6.00 dB**
  on the next quanta.
- **Zero allocation on the audio thread:** every wasm buffer + the
  `clap_process_t` + a param-event pool are allocated once in `prepare()`;
  `start_processing` is called once; `processQuantum()` only pokes memory and
  calls the process fn.

### Spike finding worth recording
**Posting a `WebAssembly.Module` into an AudioWorklet is silently dropped in
Chrome** — the message never arrives and the sender does not throw. The fix
(used here) is to **transfer the raw bytes** and compile with synchronous
`new WebAssembly.Module` *in the worklet* (allowed in AudioWorkletGlobalScope,
unlike the main thread's 4 KB sync-compile cap). This must be encoded in the
adapter and the WS-F skill.

---

## 4. Files

| File | Role |
|---|---|
| `wclap-worklet.js` | **The deliverable.** Classic AudioWorklet script: worklet-safe minimal CLAP host (trampoline vtable, WASI shim, ASCII/b64 polyfills) + `RealtimeWclapPlugin` (alloc-free per-quantum) + `WclapProcessor`. |
| `poc.js` / `index.html` | Main-thread driver + page: fetch bytes → transfer to worklet → generated param controls → live meter/scope. |
| `serve.mjs` | COOP/COEP/CORP + MIME dev server (mirrors `browser-host/serve.mjs`). |
| `validate.mjs` | Headless-Chrome proof (real-time ratio + param rise + COI). |
| `adapters/wclap.js` | WS-B contract sketch on top of the PoC (params + audioNode built; MIDI/sysex/state/voice-pool flagged as SDK work). |

---

## 5. Remaining work to fully wire WebCLAP behind the shared player

1. **Factor a worklet-safe host core.** Extract the struct offsets + host/WASI
   logic shared by `wclap-host.mjs` (main-thread/offline) and this worklet bundle
   into one module both build from, so they cannot drift. Add the three polyfills
   there. (The PoC intentionally forks a focused copy to prove the architecture.)
2. **Output events (onParamsChanged / onMidiOut).** The PoC already fixes the
   "drops output events" gap for param events (out_events `try_push` captures
   `CLAP_EVENT_PARAM_VALUE`). Extend to note/midi output for `onMidiOut`.
   *(Codex flagged this as NEW work — done for params, remaining for MIDI.)*
3. **Input MIDI/sysex (`scheduleMidi`/`sendSysex`).** Build `clap_event_note` /
   `clap_event_midi` / `clap_event_midi_sysex` IN events in the worklet's
   in_events list (today it builds only `clap_event_param_value`).
4. **State (`getState`/`setState`).** Implement the CLAP `clap.state` extension
   (stream save/load vtable) in the worklet host. *(Codex: `wclap-host.mjs` has
   no state ext — NEW work.)* Bridge to the shell's PLST/"PWS1" container.
5. **Voice pool (`createSecondary`).** Chained-synth polyphony: N plugin
   instances in the worklet-resident host (share memory + host vtable) or one
   processor per voice.
6. **CLAP threading contract — the real follow-up spike (see §6).** Decide how a
   browser host answers `[main-thread]`/`[audio-thread]` annotations,
   `clap.thread-check`, and `clap.thread-pool`; write it down as a host
   capability declaration. Assume single-main + single-audio and **decline
   `clap.thread-pool`** until proven; verify real plugins tolerate the decline.
7. **Adapter integration.** Wire `adapters/wclap.js` into the WS-B shell seam
   (behind the shared player's adapter contract); add the ABI badge; build
   per-plugin `.wclap.wasm` in CI (start gain/chorus/delay).
8. **Three-browser matrix.** Re-run the proof on Firefox + Safari (shared memory
   created *inside* the worklet on a COI page is the browser-sensitive bit).
   This is WS-C1's proof-page job; reuse this validator.

---

## 5b. Host capability declaration — threading (measured)

The §5.6 threading spike, done cheaply here (probe + honest stub, not a full
spike). Probed live in `AudioWorkletGlobalScope` and posted in the `ready`
message:

```
worklet caps: { globalScope: "AudioWorkletGlobalScope",
                hasWorker: false, hasSharedArrayBuffer: true,
                memoryIsShared: true, moduleImportsThreadSpawn: false }
```

Facts and consequences:

- **`typeof Worker === "undefined"` in the worklet.** Under wasi-threads a
  spawned wasm thread *is* a Worker, so **wasm `thread-spawn` cannot succeed on
  the audio thread.** It is imported as module `"wasi"`, name `"thread-spawn"` —
  which the old shim did not provide at all (hard LinkError), or, if a build
  routed it through the Proxy'd `wasi_snapshot_preview1`, a benign `0` = "thread 0
  spawned" for a thread that never runs (**silent hang/corruption**). The PoC now
  installs an **honest failing stub** (`wasi["thread-spawn"] = () => -1`) so
  `pthread_create` returns EAGAIN and the plugin can fall back to serial.
- **PulpGain `moduleImportsThreadSpawn: false`.** wasm-ld tree-shook the import
  because PulpGain never spawns — which is *why the PoC passes*. A plugin that
  *does* call `pthread_create` will hit the stub. That failure mode lands the
  moment the lineup grows past gain/chorus/delay, so it must be handled now, not
  discovered later.
- **What the threaded build buys inside the worklet: nothing on the audio
  thread.** `memoryIsShared: true` but the thread-parallelism the shared-memory
  build exists to enable is unreachable in `AudioWorkletGlobalScope`. The threaded
  build's only *effect* here is the hard COI deployment gate (no plain static
  hosting, no third-party embed on a non-COI page).
- **`clap.thread-pool` is fork-join inside `process()`; a worklet can neither
  spawn nor block, so declining it is FORCED, not a conservative default.** A
  browser WCLAP host should return `null` from `host->get_extension` for
  `clap.thread-pool` and rely on the plugin's serial fallback. *(Open item — see
  the second-opinion research dispatched with this spike: whether real WCLAP
  plugins tolerate a declined `clap.thread-pool` / failed `thread-spawn`, and
  whether the WCLAP spec/tooling actually requires the threaded build or a
  single-threaded `wasm32-wasi` build would lift the COI gate.)*

**Proposed host capability contract:** *single main thread + one audio thread; no
wasm `thread-spawn`; `clap.thread-pool` declined.* Write it down and gate the
plugin lineup on it until the second opinion resolves whether a non-threaded
WebCLAP build is viable (which would re-open the plan's "No threadless WebCLAP"
principle — a plan-level decision, flagged, not made here).

## 6. Adoption of the async-audit WS-C2 feedback

The audit's WS-C2 note is adopted. Points folded into this decision:

- **"A vs B is a false binary — it is `lead==0` vs `lead>=1` of one seam."**
  Agreed and reflected in §1: the DSP invocation goes through a single seam;
  worklet-resident is the degenerate `lead==0` case, and a future ring is the
  same seam at `lead>=1`, chosen at runtime on `self.crossOriginIsolated`.
- **"Build worklet-resident first — you need it regardless; it is the floor."**
  Agreed; that is exactly what this PoC builds.
- **"Both real precedents (WAM v2, Signalsmith `wasm-clap-browserhost`) are
  worklet-resident; no one has benchmarked the ring hop."** Cited in §1 as
  decisive evidence for A and against building B on an unmeasured belief.
- **"`Atomics.wait` is banned in the worklet; the RT thread can't safely
  notify."** This is a concrete extra cost/foot-gun for B's ring (poll-only) and
  a further reason A is simpler. Recorded in §1.
- **"The real unresolved question is CLAP's threading contract, not the ring."**
  Agreed and promoted to the top of the remaining-work list (§5.6) as the next
  spike — orthogonal to, and more important than, the ring.
- **Fixed 128-frame quantum / no `renderSizeHint` reliance / pre-sized shared
  memory / no alloc in `process()`** — all already satisfied by the PoC's
  design (128-frame quantum, 512-page initial shared memory, alloc-free
  `processQuantum`).

One clarification vs the feedback: it notes worklet-resident single-threaded wasm
needs no COOP/COEP. Our WebCLAP module is built `wasm32-wasi-threads` (shared
memory), so it **does** require COI even worklet-resident. That does not change
the decision — COI is already solved (Cloudflare `_headers`) — but the
"no-headers floor" only applies if we also ship a non-threaded WebCLAP build,
which the plan deliberately does not (principle §1.1: "No threadless WebCLAP").
