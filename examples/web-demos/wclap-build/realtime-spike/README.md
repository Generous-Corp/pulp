# WS-C2 spike — real-time WebCLAP through an AudioWorklet

A proof-of-concept that a WebCLAP plugin (**PulpGain**) renders in **real time**
inside an `AudioWorkletProcessor` — the worklet-resident CLAP host (Architecture
A). This is the WebCLAP analogue of how WAM runs its wasm in the worklet, so a
WebCLAP demo can sit behind the *same* shared player as WAM.

See **[`DECISION.md`](./DECISION.md)** for the architecture decision, the
evidence, the measured results, and the remaining work. It is written to be
shared as the WS-C2 spec.

## Run it

```bash
# 1. Put a built WebCLAP module here as PulpGain.wasm
#    (from examples/web-demos/wclap-build/ via tools/cmake/PulpWclap.cmake).
#    NOT committed — it is a build artifact (see .gitignore).

# 2. Serve with cross-origin isolation:
node serve.mjs           # http://localhost:8790/examples/web-demos/wclap-build/realtime-spike/
# Open it, move Input Gain, click "Un-mute output".
```

## Validate headlessly

```bash
npm install playwright-core     # dev-only; drives the system browser, no download
node validate.mjs --screenshot /tmp/wclap-rt-spike.png
# Asserts: crossOriginIsolated, plugin hosted in the worklet, REAL-TIME render
# (quanta ≈ sampleRate/128 per wall-second), unity baseline, +6 dB on Input Gain.
```

## Architecture in one paragraph

The main thread fetches the `.wasm` **bytes** and transfers them into the
worklet (posting a compiled `WebAssembly.Module` into an AudioWorklet is silently
dropped in Chrome). The worklet creates the shared `WebAssembly.Memory`, compiles
+ instantiates the module, and drives the full CLAP lifecycle
(`init → factory → create → activate → start_processing`) **in
`AudioWorkletGlobalScope`**. Each `process()` quantum, `RealtimeWclapPlugin`
copies input into pre-allocated wasm buffers, latches any queued param events,
calls the plugin's `process()` fn, and copies output back — **no allocation on
the audio thread**. Params arrive over the node's `MessagePort`.

Files: `wclap-worklet.js` (the host), `poc.js`/`index.html` (driver + UI),
`serve.mjs` (COOP/COEP), `validate.mjs` (headless proof), `adapters/wclap.js`
(WS-B contract sketch).
