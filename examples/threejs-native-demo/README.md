# Pulp Native Three.js Demo

This example runs the real MIT `three.webgpu.js` renderer inside Pulp's native
JS engine on top of the Dawn-backed WebGPU bridge. It does not use a browser or
WebView.

Current demo modes:

- `--demo spectrum` (default): hybrid native shell with a live audio-reactive
  peak bar and FFT-driven spectrum field fed by Pulp's `VisualizationBridge`
- `--demo particles`: audio-reactive particle cloud driven by beat + RMS + FFT
  data from the same native bridge
- `--demo ribbon`: streaming 3D waveform ribbon driven by beat + RMS + FFT
  data from the same native bridge
- `--demo reverb`: room reverb scene with a native room shell, reflection
  lines, and a diffuse late-reverb cloud driven by room parameters + waveform
  data from the same native bridge
- `--demo gltf-box`: embedded textured GLB loaded through `fetch()` +
  `GLTFLoader.parse()` and rendered by the native WebGPU bridge
- `--demo cube`: the earlier animated green cube smoke scene
- all modes use the real `THREE.WebGPURenderer` on the Dawn-backed native
  canvas path
- pointer drag, wheel zoom, and trackpad pinch are routed through native input
  events
- real `OrbitControls` addon import and initialization works on the native
  canvas path
- native GPU window presentation goes through Pulp's `WindowHost`
- redraw is driven by Pulp's host `FrameClock` + `requestAnimationFrame`
- buffered GPU bridge uses structured native payloads instead of JSON text
  round-trips on the hot render path
- clean macOS quit path avoids the earlier V8 shutdown crash
- capture mode now primes several real frame callbacks before screenshotting so
  GPU-backed demos are captured after their first truthful present, not at an
  empty warm-up frame

## Build

This target requires the macOS GPU/Skia bundle. If
`external/skia-build` does not contain `libskia.a`, restore it first:

```bash
./tools/build-skia.sh mac
```

```bash
brew install node@24

cmake -S . -B build-threejs-runtime -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON \
  -DPULP_JS_ENGINE=v8 \
  -DV8_INCLUDE_DIR=/opt/homebrew/opt/node@24/include/node \
  -DV8_LIB_DIR=/opt/homebrew/opt/node@24/lib \
  -DV8_LIBRARY_PATH=/opt/homebrew/opt/node@24/lib/libnode.137.dylib

cmake --build build-threejs-runtime \
  --target pulp-threejs-native-demo pulp-test-threejs-bridge -j8
```

Use Homebrew `node@24` for this lane. The unversioned Homebrew `node` formula
may be newer; Node 26 / `libnode.147.dylib` currently compiles but aborts in
embedded V8 while evaluating the bridge/Three.js JavaScript. If `node@24`
changes its ABI, replace `libnode.137.dylib` with the file reported by:

```bash
ls /opt/homebrew/opt/node@24/lib/libnode*.dylib
```

On hosts without the Skia library, this configure-only bridge dependency check
still proves that V8, WebGPU, and the Three.js FetchContent dependency resolve:

```bash
cmake -S . -B build-threejs-runtime-smoke -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=OFF \
  -DPULP_JS_ENGINE=v8 \
  -DV8_INCLUDE_DIR=/opt/homebrew/opt/node@24/include/node \
  -DV8_LIB_DIR=/opt/homebrew/opt/node@24/lib \
  -DV8_LIBRARY_PATH=/opt/homebrew/opt/node@24/lib/libnode.137.dylib
```

## Sealed v8-builder V8 provider

Instead of a Homebrew `libnode`, the demo runs on a sealed V8 from the
[v8-builder](https://github.com/danielraffel/v8-builder) project — a single
`libv8.dylib` with no external ICU / zlib / Abseil dependencies and a pinned
runtime version. Fetch the sealed artifact once (it unpacks under
`external/v8-build/<platform>/`), then select `v8`; `FindV8.cmake` resolves it:

```bash
python3 tools/scripts/fetch_v8_for_release.py darwin-arm64   # once

cmake -S . -B build-v8seal -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON \
  -DPULP_JS_ENGINE=v8 \
  -DPULP_VALIDATE_V8_PROVIDER_STRICT=ON

cmake --build build-v8seal \
  --target pulp-threejs-native-demo pulp-test-js-engine -j8
```

`FindV8.cmake` resolves the sealed artifact, links the `v8::v8` imported target
(`@rpath/libv8.dylib`), and `core/view/CMakeLists.txt` emits provider-identity
compile definitions (`v8builder`, the resolved lib path, and the
`manifest.json`-pinned runtime version). The engine default is unchanged — this
is the explicit V8 lane only.

Prove which V8 is actually linked:

```bash
./build-v8seal/examples/threejs-native-demo/pulp-threejs-native-demo \
  --print-engine-identity
# → engine_type=V8 / runtime_version=15.1.27 / provider_kind=v8builder
#   pulp_has_v8=1 / gpu_backend=Metal / gpu_software=0

otool -L ./build-v8seal/examples/threejs-native-demo/pulp-threejs-native-demo \
  | grep -i 'v8\|node'      # → @rpath/libv8.dylib, no libnode
```

`PULP_VALIDATE_V8_PROVIDER_STRICT=ON` adds the `v8_provider_identity_strict`
CTest, a no-skip-pass gate that asserts the identity block and renders
`--demo cube --capture` to a non-empty PNG (proving V8 + Dawn/Skia coexist):

```bash
ctest --test-dir build-v8seal -R v8_provider_identity_strict --output-on-failure
```

## Run

```bash
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo
# same as: --demo spectrum
# try particles: --demo particles
# try ribbon: --demo ribbon
# try reverb: --demo reverb
# try gltf-box: --demo gltf-box
```

Explicit cube smoke mode:

```bash
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo --demo cube
```

Optional window size:

```bash
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo --size 768x768
```

Optional one-shot capture:

```bash
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo --demo spectrum --capture /tmp/threejs-demo.png
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo --demo gltf-box --size 540x720 --capture /tmp/threejs-gltf-box.png
```

Focused bridge smoke:

```bash
./build-threejs-runtime/test/web-compat/pulp-test-threejs-bridge "[threejs][gpu][phase13]"
./build-threejs-runtime/test/web-compat/pulp-test-threejs-bridge "[threejs][gpu][texture][datatexture]"
./build-threejs-runtime/test/web-compat/pulp-test-threejs-bridge "[threejs][gpu][gltf][loader]"
./build-threejs-runtime/test/web-compat/pulp-test-threejs-bridge "[threejs][gpu][gltf][fetch]"
ctest --test-dir build-threejs-runtime -R "threejs_native_demo_(capture_no_hang|gltf_box_capture_no_hang)" --output-on-failure
```

## macOS plugin-editor capture baseline

The runtime hardening lane uses this demo's hidden `WindowHost` capture path as
the macOS plugin-editor cube baseline before a DAW-hosted editor capture is
automated:

```bash
./build-threejs-runtime/examples/threejs-native-demo/pulp-threejs-native-demo \
  --demo cube --size 540x720 --capture /tmp/pulp-threejs-macos-editor-cube.png
```

Expected evidence:

- stdout includes `Three.js native demo ready (cube)` and `Captured demo frame:`.
- `/tmp/pulp-threejs-macos-editor-cube.png` exists and is non-empty.
- Visual inspection shows the cube scene, not a blank clear color or fallback UI.

## Why This Example Exists

This is the first visible native Three.js proof for the native bridge plan:

- real `THREE.WebGPURenderer`
- native `GPUCanvasContext`
- shared Dawn device between the bridge and the visible GPU host
- hybrid 2D+3D composition on the native widget/layout path
- four real audio-reactive examples via `VisualizationBridge`
  (spectrum + particle visualizer + waveform ribbon + room reverb)
- live redraw through the host frame loop (not just a one-shot frame)
- native pointer/input events reaching the Three.js canvas path
- trackpad pinch and wheel zoom support on the native host path
- no browser-hosted fallback
- native texture uploads from `DataTexture` and image-backed GLTF materials
  reach Dawn textures instead of staying in JS-side mock objects
