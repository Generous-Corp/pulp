# PulpGain WebCLAP — browser host

A minimal **in-browser** host for the PulpGain WebCLAP module. It loads
`PulpGain.wasm`, drives it through the real CLAP lifecycle with
[`WebClapHost`](../../../../core/format/src/wasm/wclap-host.mjs) — the same engine
the Node runner uses, unchanged, on the browser main thread — renders audio, and
exposes a generated control per plugin parameter.

No browser flags. The CLAP host vtable is synthesized with tiny wasm *trampoline*
modules (see `wclap-host.mjs`), so it does not depend on `WebAssembly.Function`
(which browsers do not expose). The module imports a shared `WebAssembly.Memory`,
so the page must be **cross-origin isolated** — `serve.mjs` sends the required
COOP/COEP headers.

## Run it

```bash
# 1. Build the WebCLAP module (once):
#    see ../CMakeLists.txt — produces ../build/PulpGain.wasm
# 2. Serve the repo with cross-origin isolation:
node serve.mjs            # http://localhost:8787/examples/web-demos/wclap-build/browser-host/
```

Open the URL, move the **Input Gain** slider, and click **Render & play**.

## Validate headlessly

`validate.mjs` drives the page in headless Chrome/Canary and asserts the plugin
activates, renders audio at unity (passthrough), and responds to a parameter
control (`Input Gain +6 dB` lifts the output ~+6 dB). It captures a UI
screenshot.

```bash
npm install playwright-core     # dev-only; drives the system browser, no download
node validate.mjs --screenshot /tmp/wclap-browser.png
# --browser <path>  to point at a specific Chrome/Canary/Chromium
# --headed          to watch it run
```

## Audio model

The host renders a block through `plugin.process()` on the main thread and plays
the result via an `AudioBuffer`. Real-time `AudioWorklet` streaming (host on a
worker, the worklet draining a `SharedArrayBuffer` ring) is a later refinement;
this host proves the module is hosted and audible in the browser with a working
generated UI.
