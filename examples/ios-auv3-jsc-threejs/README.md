# Pulp iOS-D.3c — Three.js Rotating Cube AUv3 Demo

A minimal Audio Unit v3 plug-in whose editor pane runs the real
`three.webgpu.js` library under JSC (no JIT — App-Store-shippable)
and paints a rotating cube through Pulp's Dawn/Metal `GpuSurface`.

This is the "use the iOS-D.3b plumbing for real" demo. The audio path
is a deliberately silent bit-perfect pass-through; the focus is the
editor.

## What this exercises (all landed in iOS-D.3b)

| Slice | What it ships | Used here as |
|-------|---------------|--------------|
| 1 | `PluginViewHost::gpu_surface()` + `WidgetBridge::attach_gpu_surface()` | `navigator.gpu` / `canvas.getContext('webgpu')` route through Pulp's Dawn instance, not a mock |
| 2 | `tools/scripts/bundle_threejs_for_jsc.mjs` → `three.iife.js` | Loaded at runtime via `pulp::view::threejs_iife_source()`; `globalThis.THREE` populated under JSC without ESM modules |
| 3 | `_pulp_add_auv3_ios` POST_BUILD step | Drops `three.iife.js` + `web-compat-three-shim.js` into `<appex>/threejs/` |
| 4 | `presentable=true|false` flag on `__gpuCanvasConfigureImpl` / `__gpuCanvasDescribeCurrentTextureImpl` | Three.js draws hit the visible swapchain, not an offscreen texture |
| 5 | `PULP_WEBGPU_BRIDGE: queue.submit ok` log marker | Confirms command buffers actually submit |
| 6 | Program closeout + iOS bundle path fix | The .appex resource layout this README assumes |

## Build (iOS Simulator)

```bash
cmake -S . -B build-ios-sim-three -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON

cmake --build build-ios-sim-three \
  --target PulpThreeJsDemo_HostApp_Embed \
  --config Release -- -sdk iphonesimulator
```

## Build (physical iPad, signed)

Source the user's signing creds (under `~/.config/pulp/secrets/notary.env`)
to populate `DEVELOPMENT_TEAM`, then configure for the device SDK:

```bash
set -a && source ~/.config/pulp/secrets/notary.env && set +a

cmake -S . -B build-ios-device-three -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=${DEVELOPMENT_TEAM:-95CX6P84C4}

cmake --build build-ios-device-three \
  --target PulpThreeJsDemo_HostApp_Embed \
  --config Release -- -sdk iphoneos -allowProvisioningUpdates
```

## Install + open on the iPad

```bash
# UDID of the user's iPad Pro 11" 3rd-gen.
xcrun devicectl device install app \
  --device 1E896926-6486-5871-B572-9E5DDEE4DAD7 \
  build-ios-device-three/Release-iphoneos/PulpThreeJsDemo.app

xcrun devicectl device process launch \
  --device 1E896926-6486-5871-B572-9E5DDEE4DAD7 \
  --terminate-existing \
  com.danielraffel.pulpdev.threejsdemo.host
```

The container app's button presents the AUv3 editor; the rotating
cube paints inside the editor pane.

## Validation log markers

Grep the device console (Xcode → Window → Devices and Simulators →
View Device Logs) for these markers. Order matters — each marker
unblocks the next stage:

```
AU iOS: view controller loaded, ... gpu=1
GpuSurface: backend_type=Metal
[plugin-gpu-host] GpuSurface attached to WidgetBridge via ScriptedUiSession (iOS AUv3)
PULP_THREE_DEMO: IIFE loaded (N bytes)
PULP_THREE_DEMO: scene script loaded (N bytes)
PULP_THREEJS: globalThis.THREE available (NN exports)
PULP_THREE_SHIM: ready
PULP_THREE_SHIM: webgpu-renderer-present
PULP_WEBGPU_BRIDGE: ready
PULP_WEBGPU_BRIDGE: canvas.getContext('webgpu') ok (presentable=true)
PULP_WEBGPU_BRIDGE: queue.submit ok (canvas=..., commands=N)
PULP_THREE_DEMO: renderer.init ok
PULP_THREE_RENDER: first frame submitted
PULP_THREE_RENDER: 60-frame avg <ms>/<FPS>
```

The `presentable=true|false` flag is the program's most load-bearing
diagnostic. `false` means JS draws went to an offscreen texture, not
the visible swapchain — the editor will be black even though
`PULP_THREE_RENDER: first frame submitted` printed. See
`.agents/skills/ios/SKILL.md` § "Three.js inside AUv3 on iOS" if that
happens.

## Failure modes by design

Every load-bearing resource is checked individually with a loud
`PULP_THREE_DEMO:` log line and a visible-on-device fallback. If the
cube never appears, the editor pane paints one of:

- "Three.js not loaded (IIFE missing)" — `three.iife.js` is missing
  from the `.appex`. Check that Node.js was present on the build host
  (the `tools/cmake/PulpAuv3.cmake` POST_BUILD step skips with a
  STATUS warning if not).
- "Three.js not loaded — see PULP_THREE_DEMO log lines" — the
  concatenated tempfile couldn't be written, or the
  `ScriptedUiSession::load` call failed. Console will explain which.
- "Three.js script failed — see PULP_THREE_DEMO log lines" —
  the JS evaluate threw. Console will show the message.

The plumbing primitives (IIFE bundler, NSBundle loader, GpuSurface
plumbing, presentable flag) all ship regardless of whether the demo
runs successfully on a given iPad. The worst case is a clear failure
message on a paint surface that still works for any other Pulp UI.

## Files

- `CMakeLists.txt` — `pulp_add_ios_auv3` + `pulp_add_ios_host_app`,
  with a local FetchContent for Three.js so the bundler step fires
  on iOS (the root `PulpDependencies.cmake` gate requires
  `PULP_BUILD_TESTS` which iOS forces off).
- `src/threejs_demo.{hpp,mm}` — Processor with the silent
  pass-through audio path and a `create_view()` that loads the
  Three.js IIFE + scene script into a `ScriptedUiSession`.
- `src/au_v3_entry.cpp` — AU v3 factory registration (mirrors
  `examples/ios-auv3-synth/src/au_v3_entry.cpp`).
- `js/scene.js` — The rotating cube; copied to
  `<appex>/threejs/scene.js` at build time, read by the C++ side via
  NSBundle, prepended with the Three.js IIFE + diagnostic shim, and
  handed to a JSC `evaluate()` in one shot.

## Related

- `planning/2026-05-29-ios-d3b-threejs-webgpu-program.md` —
  the program this demo closes out (§ 11 updated to reference this
  example).
- `.agents/skills/ios/SKILL.md` § "Three.js inside AUv3 on iOS" —
  gotchas, log-marker chain, buffered-draw probe.
- `.agents/skills/threejs-bridge/SKILL.md` — bridge architecture
  shared with the macOS V8 lane.
- `examples/ios-auv3-chainer/` — sibling iOS-D.2 example that does
  custom-paint instead of scripted-UI. Useful comparison if scripted
  UI is failing to load and you want to validate that the .appex
  packaging + AU controller path is still healthy.
