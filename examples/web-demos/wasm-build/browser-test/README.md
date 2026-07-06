# WAMv2 browser load test

Headless, deterministic proof that a Pulp WAMv2 plugin loads and renders audio
in a real browser through the full AudioWorklet path. Uses an
`OfflineAudioContext`, so it is speakerless and reproducible.

## Run

1. Build the SINGLE_FILE worklet module (an ES-module factory):

   ```
   emcmake cmake -S . -B build -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>
   cmake --build build --target PulpGainWorklet-wam
   ```

2. Assemble the served files next to this directory:

   ```
   cp build/PulpGainWorklet.js          browser-test/wam-dsp.js
   cp ../../../core/format/src/wasm/wam-processor.js browser-test/
   cp ../../../core/format/src/wasm/wam-runtime.mjs  browser-test/
   cp ../../../core/format/src/wasm/wam-plugin.js    browser-test/
   ```

3. Serve and open:

   ```
   node browser-test/serve.mjs    # http://localhost:8731/
   ```

The page reports pass/fail and exposes `window.__result` for automation
(Playwright / chrome-devtools). Checks: plugin instantiates, render produces a
full finite non-silent buffer, unity passthrough ≈ 0.5, descriptor parses, GUI
renders.

## Notes

- The DSP module must be the SINGLE_FILE + MODULARIZE + EXPORT_ES6 build
  (`pulp_add_wam_plugin(... SINGLE_FILE)`): the BASE64-embedded wasm avoids
  fetch/async-compile in worklet scope, and the ES-module factory is imported by
  `wam-processor.js`.
- `registerProcessor` runs synchronously; the DSP is instantiated async inside
  the constructor (AudioWorklet `addModule` does not await a module top-level
  await), and the worklet stays silent until ready.
- `wam-runtime.mjs` avoids `TextEncoder`/`TextDecoder` (undefined in
  `AudioWorkletGlobalScope`).
