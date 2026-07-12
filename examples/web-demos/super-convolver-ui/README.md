# SuperConvolver UI — Pulp's render stack in a browser canvas

This is Pulp's own View tree (real `Knob` / `Label` widgets, Yoga layout, the
Ink & Signal theme, Skia text shaping) compiled to WebAssembly and painted into a
`<canvas>` by **Skia Ganesh on WebGL2**.

The module is **DSP-free**. It links no `Processor`, no `core/audio`, no
`core/format`, no `core/gpu_audio`. It talks to whatever host mounted it over the
web-player `HostAdapter` seam (`getParameterInfo` / `setParameterValue` /
`onParamsChanged`, see `packages/pulp-web-player/src/adapters/adapter.d.ts`), so
the same `.wasm` serves both the WAM demo and the WebCLAP demo without touching
either ABI — and without `clap.gui`.

## Why Ganesh/WebGL2 and not Graphite/Dawn

The wasm Skia slice published by skia-builder is a Ganesh/WebGL2 build: it ships
no `libdawn_combined.a` and no Graphite backend. Pulp's *native* GPU UI is Skia
Graphite on Dawn; on the web the backend swaps below the render boundary
(`skia_surface_ganesh.cpp` instead of `skia_surface.cpp` + `gpu_surface_dawn.cpp`)
and nothing in `core/canvas`, the widgets, or the editor code changes. A later
Graphite-on-wasm convergence replaces one TU.

WebGL2 has no compute shaders, so GPU *audio* cannot ride this surface — it needs
WebGPU (emdawnwebgpu) and lives in a separate, Skia-free module.

## Build

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -S examples/web-demos/super-convolver-ui -B build-webui \
    -DCMAKE_BUILD_TYPE=Release \
    -DSKIA_DIR=<wasm Skia slice root> \
    -DPULP_WEBUI_CHOC_INCLUDE=<dir containing choc/>
cmake --build build-webui
```

`SKIA_DIR` is the directory containing `build/wasm-gpu/lib/Release/libskia.a`
(the `skia-build-wasm-wasm32-gpu-release.zip` asset, unpacked).
`-DPULP_WEBUI_YOGA_DIR=<yoga checkout>` skips the Yoga fetch when offline.

Output: `build-webui/PulpSuperConvolverUi.{js,wasm,data}`. The `.data` file is the
preloaded MEMFS image — it carries `icudtl.dat`, which SkUnicode needs for text
shaping.

The source subset, the link flags, and the export table are in
`tools/cmake/PulpWebUi.cmake`.

## Mounting it

```js
import { mountPulpUi } from "./pulp-ui.js";
const ui = await mountPulpUi(document.getElementById("plugin-ui"), adapter);
// … later
ui.destroy();
```

`mountPulpUi` wires both directions of the seam: `Module.onParamChange` /
`onGestureBegin` / `onGestureEnd` out to the adapter (gesture callbacks bracket
the edit so the host can group it for undo), and `adapter.onParamsChanged` back
in through `_pulp_ui_set_param`. A `ResizeObserver` drives `_pulp_ui_resize` with
the live `devicePixelRatio`.

## The C ABI

| Export | Direction | Purpose |
|---|---|---|
| `_pulp_ui_add_param(slot, name, min, max, default, unit)` | host → UI | Push one row of the parameter table. Call before `_pulp_ui_init`. |
| `_pulp_ui_init(selector, w, h, dpr)` | host → UI | Bind the canvas, build the view, start the rAF loop. |
| `_pulp_ui_resize(w, h, dpr)` | host → UI | Resize the backing store and the view. |
| `_pulp_ui_set_param(slot, value)` | host → UI | Apply a value (real units) from the plugin. |
| `_pulp_ui_get_param(slot)` | host → UI | Read the value the UI is showing. |
| `_pulp_ui_repaint()` | host → UI | Synchronous paint (the pixel fixture reads back in the same task). |
| `_pulp_ui_widget_rect(slot, kind, out)` | host → UI | Root-relative `[x,y,w,h]`; `kind` 0 = knob, 1 = name label. |
| `_pulp_ui_capture_png(out_ptr, out_len)` | host → UI | PNG of the live tree via Skia's CPU raster surface. Caller `_free`s. |
| `_pulp_ui_shutdown()` | host → UI | Tear down loop, host, and view. |

UI → host does not appear here: it leaves through `EM_JS` calls into
`Module.onParamChange(slot, value)`, `Module.onGestureBegin(slot)`, and
`Module.onGestureEnd(slot)`.

## Pixel fixture

`browser-test/validate.mjs` is the proof for the whole browser render stack — the
Ganesh surface, the font manager behind text shaping, the requestAnimationFrame
render loop, and the browser window host's pointer plumbing. None of them can be
unit-tested natively.

```sh
npm install --no-save playwright-core        # resolvable from browser-test/ (CI symlinks the
                                             # workspace node_modules in, as the WAM fixture does)
node browser-test/validate.mjs --build ../build-webui --artifact /tmp/pulp-ui.png
```

`--browser <path>` or `CHROME_PATH` selects the binary; `--headed` watches it run.

It mounts the module against a mock `HostAdapter` (no audio, no DSP wasm) in
headless Chrome and asserts:

1. a WebGL2 context exists;
2. the GPU drawing buffer is not blank, and neither is a Skia raster capture of
   the same tree;
3. the **name-label region has text ink** — the direct regression test for a null
   `SkFontMgr` / zero-width shaping, which renders a blank-but-not-black canvas;
4. a synthesized pointer drag over a knob emits `gestureBegin → setParameterValue…
   → gestureEnd` on the adapter, with a monotonically moving value;
5. `adapter.onParamsChanged` changes the pixels (host → UI direction);
6. a **real WebGL context loss** (`WEBGL_lose_context`) makes the surface report
   unavailable instead of silently rendering into a dead context, and a restore
   rebuilds Ganesh and repaints.

**What the CI green does and does not prove.** The fixture launches Chrome with
`--enable-unsafe-swiftshader`, because a headless CI runner has no GPU and current
Chrome refuses to fall back to SwiftShader without that opt-in (it hands back a
null WebGL2 context instead, which this fixture treats as a hard failure). So on
CI the WebGL2 context is real but **software-rasterized** — the checks prove the
Ganesh/WebGL2 code path, the font stack, the input plumbing, and the context-loss
recovery, NOT that a particular GPU driver renders it correctly. On a machine with
a GPU the same flag is a no-op and the run is a real-GPU proof.

It prints an FNV-1a fingerprint of the GPU readback for drift observation but does
**not** gate on it — GPU drivers vary. The gates are the content, text,
interaction, round-trip, and context-loss assertions above.
