---
name: engine
description: Query, recommend, and switch the Pulp JS engine backend (QuickJS, JavaScriptCore, V8). Handles "which JS engine", "switch to V8", "engine for Three.js".
requires:
  scripts: []
  tools: []
---

# JS Engine Skill

Manage the JavaScript engine backend used by Pulp's scripting layer. Three engines are available:

| Engine | Platform | Strengths | License |
|--------|----------|-----------|---------|
| **QuickJS** | All | Portable, small, zero dependencies. Default. | MIT |
| **JavaScriptCore** | Apple only | System framework, good JIT, zero-dep on macOS/iOS | LGPL-2.1 (system use OK) |
| **V8** | Desktop | Best JIT, ideal for heavy JS (Three.js), largest footprint | BSD-3-Clause |

## Web-compat preludes shipped with every engine

Every engine boots with the same set of `web-compat-*.js` preludes embedded
into `web_compat_preludes_gen.hpp` and evaluated in order by
`WidgetBridge`. The current set covers everything React 18 dev (and most
similar frameworks) feature-detect on construction:

| Surface | Where | Why |
|---------|-------|-----|
| `Element.nodeType` (=1) / `nodeName` (=tagName) | `web-compat-element.js` | React reconciler walks every node and bails before first commit without DOM-compatible node identity. |
| `Element.ELEMENT_NODE` / `TEXT_NODE` / `COMMENT_NODE` constants | `web-compat-element.js` | `node.ELEMENT_NODE === 1` fast-paths in React |
| Widget tag factories (`virtual-list`, `virtuallist`, `segmented`, `stepper`, etc.) | `web-compat-element.js` | DOM-lite custom tags construct the same native widgets as `@pulp/react`; keep this table in sync when a new widget tag is routed through `_ensureNative`. A tag needs FOUR tables in step, not one: this map, the `createX` factory (`factory_api.cpp`), `make_widget_for_tag()` (`widget_bridge.cpp`, the `__domAppend` path), and `wire_callbacks()` (`widget_callbacks.cpp`). The factory wires its callbacks inline while the tag path takes them from `wire_callbacks()`, so missing that one builds a real, clickable widget whose changes reach nothing — on the tag path only, from a script that is identical either way. |
| `createTextNode` → nodeType=3 + nodeName='#text' + `data`/`nodeValue` mirrors | `web-compat-document.js` | DOM Level 1 text-node spec; React's text-update path |
| `createComment` → nodeType=8, `createDocumentFragment` → nodeType=11 | `web-compat-document.js` | React portal sentinels + batched commits |
| `MutationObserver` / `IntersectionObserver` / `ResizeObserver` / `PerformanceObserver` no-ops | `web-compat-observers.js` | `typeof X === 'function'` feature-detects pass; React skips because no events ever fire |
| `XMLHttpRequest` no-op + spec readyState constants | `web-compat-observers.js` | React dev-mode error-stack lookup probes XHR |
| `Element.scrollTop`/`scrollLeft`/`scrollWidth`/`scrollHeight` (returns 0) | `web-compat-observers.js` | React dev focus warnings |
| `queueMicrotask` (Promise-based shim) | `web-compat-scheduler.js` | React 18 concurrent scheduler |
| `MessageChannel` + `MessagePort` (microtask-deferred postMessage) | `web-compat-scheduler.js` | React 18 scheduler prefers MC; falls back to setTimeout if missing (perf cliff, not a blocker) |
| `URLSearchParams` polyfill | `web-compat-scheduler.js` | React error-source URL parsing |
| `requestAnimationFrame` / `cancelAnimationFrame` (driven by native `__requestFrame__`) | `web-compat-scheduler.js` | Bundled-React frameworks reference the standard names; without this each consumer has to carry its own scheduler shim. |
| `setTimeout` / `clearTimeout` / `setInterval` / `clearInterval` (driven by native `__scheduleTimer__` deadline tracker) | `web-compat-scheduler.js` | React's scheduler yield path + plugin code; setTimeout(fn, 0) drains via microtask, positive delays drain in `service_frame_callbacks()`. |
| `performance.now()` (driven by native `__performanceNow__`) | `web-compat-scheduler.js` | Bundled-React modules read `performance.now` at module-eval time before the legacy `window.performance` shim is reachable. |
| Mirror block onto `window` (rAF/cAF/sT/cT/sI/cI/MC/qM/perf) | `web-compat-scheduler.js` | React 18's scheduler reads `window.setTimeout` / `window.requestAnimationFrame` specifically; the global must be reachable through both names. |

When adding new framework support, check the engine capability
comparison before assuming a polyfill is missing — the entry above is
exhaustive for React 18 dev. Add new files to `core/view/CMakeLists.txt`'s
`PULP_JS_PRELUDES` list AND to the `eval_or_throw` block in
`core/view/src/widget_bridge.cpp` (`embed_js.cmake` only embeds the
constants; the bridge constructor evaluates them).

### Canvas2D surface coverage

`web-compat-canvas.js` exposes `CanvasRenderingContext2D.prototype`
with the standard methods plus the gap-list closures and FilterBank-parity
additions that keep common Canvas2D calls from silently no-oping:

| Method | Notes |
|--------|-------|
| `measureText(text)` | Returns a full HTML5 `TextMetrics` object — `width` + `actualBoundingBox{Left,Right,Ascent,Descent}` + `fontBoundingBox{Ascent,Descent}`. Routed through `canvasMeasureText` which calls `SkiaCanvas::measure_text_with_font` for surface-less metrics. |
| `drawImage(img, …)` | 3 / 5 / 9-arg signatures supported; the 9-arg `(sx,sy,sw,sh,dx,dy,dw,dh)` form currently ignores the source rect — file a follow-up if a plugin needs sprite-sheet slicing. |
| `setLineDash([…])` / `getLineDash()` | Even-length patterns are taken verbatim; odd-length patterns are duplicated per the HTML5 spec. Phase comes from `lineDashOffset`. |
| `getImageData(x,y,w,h)` | Returns `{data: Uint8ClampedArray, width, height}`. The bridge currently returns zero-filled pixels (no live surface handle from JS-call context); consumers that need real pixels should round-trip through a render-host integration. |
| `putImageData(img, dx, dy)` | Decodes the typed array to base64 across the bridge and applies via `Canvas::write_pixels` on backends that implement it (Skia today). |
| `save()` / `restore()` | Forward to `canvasSave` / `canvasRestore`. JS-side caches of last-pushed text/line/global state are invalidated on save so the next draw re-pushes — the bridge captures the matching state on the C++ side via SkCanvas::save. |
| `translate` / `scale` / `rotate` / `setTransform` / `resetTransform` / `transform` | Forward to `canvasTranslate` / `canvasScale` / `canvasRotate` / `canvasSetTransform`. `transform` is best-effort: pure translation forwards to `canvasTranslate`; other matrices are silently dropped (the bridge has no concat primitive). `setTransform` accepts the (a,b,c,d,e,f) form and the single-DOMMatrix form. |
| `arc(cx,cy,r,a0,a1,ccw)` / `ellipse` | Approximated as cubic-Bezier segments (4-segment unit-circle scaling) so the path participates in `fill()` / `stroke()` / `clip()`. `arcTo` is a conservative two-segment lineTo approximation — sufficient for rounded marquee corners, not fidelity-critical. |
| `bezierCurveTo` / `quadraticCurveTo` / `rect` / `roundRect` | Forward to `canvasCubicTo` / `canvasQuadTo` / repeated `canvasLineTo`. `rect` emits an explicit closing lineTo back to the start so the resulting subpath is closed. `roundRect` honours the uniform-radius case; non-uniform `radii[]` falls back to `radii[0]`. |
| `clip(fillRule)` | Calls `canvasClip`. The fill rule is dropped — Pulp's bridge currently ignores even-odd vs nonzero, matching SkCanvas defaults. |
| `fillText(text,x,y)` / `strokeText` | `fillText` syncs global / text state and forwards to `canvasFillText` with the active fillStyle's colour (or first gradient stop). `strokeText` falls back to `fillText` with the strokeStyle colour — Pulp's bridge has no stroke-text command. |
| `createLinearGradient` / `createRadialGradient` / `createConicGradient` | Return a `CanvasGradient` object with `_kind`, `_params`, `_stops`, and an `addColorStop(offset, color)` method. Gradients are NOT pushed to the bridge until they're assigned to `fillStyle` / `strokeStyle` AND a draw fires — `_applyFillStyle()` / `_applyStrokeStyle()` flush via the matching linear, radial, two-circle radial, or conic bridge setter. |
| `fillStyle` / `strokeStyle` | Plain fields. `_applyFillStyle()` runs before every fill draw and flushes either a string colour (via `canvasSetFillColor`), a gradient (via `canvasSetLinearGradient` / `canvasSetRadialGradient` / `canvasSetConicGradient`), or a pattern (via `canvasSetFillPattern`), tracking `_activeFillKind` so a subsequent string assignment first calls `canvasClearGradient`. `_applyStrokeStyle()` routes string colours, linear/radial/conic gradients, and patterns through the stroke bridge setters where the backend supports them; CoreGraphics conic stroke and stroke pattern paths still fall back to a solid colour. |
| `globalAlpha` / `globalCompositeOperation` / `font` / `textAlign` / `textBaseline` / `lineCap` / `lineJoin` | Plain fields. Pushed to the bridge via `_syncGlobalState` / `_syncTextState` / `_syncLineState` lazy helpers — they only emit a `canvas*` call when the value differs from the last-sent cache, and the cache is invalidated on `save()` / `restore()`. |
| `createPattern` | Returns a `CanvasPattern` handle for non-empty file / data-URL image sources. Fill patterns flush through `canvasSetFillPattern`; Skia renders tiled image shaders and CoreGraphics uses a CGPattern tile callback for fills. Missing or undecodable images fail gracefully in the backend paint path. |

#### Why the shim must export every Canvas2D method

If any common method (`save`, `setTransform`, `createLinearGradient`,
`globalAlpha` setter, …) is missing from `CanvasRenderingContext2D.prototype`,
the very first call to it throws `TypeError: ... is not a function`. The
exception unwinds the calling function — and in a React render boundary,
the boundary swallows the throw and silently retries on the next commit.
Net effect: only the *prefix* of canvas calls before the throw makes it
to the bridge, and the rendered output is missing whatever the rest of
the frame would have drawn. The Spectr FilterBank standalone displayed
this exact symptom (a clean `clearRect` + nothing else, leaving the
parent's dark navy bg showing through where canvas content should have
been). The fix is shim coverage at the JS layer, not at the
CanvasWidget/SkiaCanvas pipeline below.

When adding a new Canvas2D method, audit:

1. Does the bridge expose a matching `canvas*` function in
   `core/view/src/widget_bridge.cpp`? If not, add it.
2. Is the path expressed as path-construction (records into the
   current Skia path) vs immediate-mode (draws now)? Match the spec —
   `arc()` is path-construction, not a stroke.
3. Does the new method need any of the cached state to flush (font /
   textAlign / lineCap / globalAlpha)? Call the relevant `_sync*`
   helper before forwarding to the bridge.
4. Add a regression test in `test/test_canvas2d_shim.cpp` covering the
   new method's existence + a representative end-to-end Skia render
   (the FilterBank-style raster test pattern).

## Commands

### `status` — Show current engine configuration

1. Read `CMakeCache.txt` in the build directory to find `PULP_JS_ENGINE`:
   ```bash
   grep PULP_JS_ENGINE build/CMakeCache.txt 2>/dev/null || echo "Not configured (default: QuickJS)"
   ```
2. Report which engines are available on this platform:
   - QuickJS: always
   - JSC: only on macOS/iOS
   - V8: only if `V8_INCLUDE_DIR` and `V8_LIB_DIR` are set
3. Show the current default engine for this build.

### `recommend <workload>` — Suggest the best engine

Based on the workload description:

- **Three.js / heavy 3D scenes**: Recommend **V8** (JIT compilation makes ~1MB library parse viable, complex scene graphs need fast execution). If V8 not available, warn that QuickJS will work but parse time may exceed 1 second.
- **Standard plugin UIs**: Recommend **QuickJS** (portable, proven, all existing code tested against it).
- **Apple-only shipping**: Recommend **JSC** (zero dependency, good performance, system framework).
- **Cross-platform shipping**: Recommend **QuickJS** (works identically everywhere).

### `switch <engine>` — Change the JS engine

**IMPORTANT: Always confirm with the user before switching.**

1. Determine the requested engine (quickjs, jsc, v8).
2. Check availability:
   - If `jsc` on non-Apple: explain it's not available, suggest alternatives.
   - If `v8` without V8 libs: explain V8 must be built/installed separately, link to docs.
3. **Use AskUserQuestion** to confirm the change:
   - Show the current engine
   - Show what will change
   - Warn about any implications (e.g., reconfigure + full rebuild required)
   - Ask "Switch JS engine to X?" with Yes/No options
4. If confirmed, run:
   ```bash
   cd <project_root>
   cmake -S . -B build -DPULP_JS_ENGINE=<engine>
   tools/ci/governed-build.sh cmake --build build
   ```
   Or via the CLI:
   ```bash
   pulp build --js-engine=<engine>
   ```
5. After build succeeds, run tests to verify nothing broke:
   ```bash
   ctest --test-dir build --output-on-failure -E "AudioWorkgroup|GpuSurface"
   ```

### Auto-detection hint

When reviewing or loading JS code, if you see any of these patterns, proactively suggest an engine:

- `THREE.Scene`, `THREE.WebGLRenderer`, `import * as THREE` → suggest V8
- Large JS files (>500KB) → suggest V8 for parse performance
- Apple-only target (iOS app, AU-only plugin) → mention JSC as an option

Use `recommend` logic above, but **never auto-switch** — always confirm first.

## Engine Selection Semantics

- `auto` (default): QuickJS everywhere unless the build explicitly
  sets a `PULP_DEFAULT_ENGINE_*` compile option. Backward compatible.
  Safe. Do not describe this as "Apple defaults to JSC"; that was an
  old header comment, not the implementation.
- `quickjs`: Explicit QuickJS. Same as auto today.
- `jsc`: JavaScriptCore on Apple. Build fails on non-Apple.
- `v8`: V8 on desktop and Android API 29+ via the pinned sealed `libv8` (fetched into `external/v8-build/`). Build fails if not fetched; Android API 26-28 and iOS fail closed for this provider. See "V8 provider library" below.

The engine choice is a **build-time** CMake option. Changing it requires reconfigure + rebuild. The abstraction ensures all JS bridge code works identically across engines — the switch is invisible to UI scripts.

## Cooperative interrupt & the runtime inspector (no step debugger)

`JsEngine` exposes `supports_interrupt()` / `request_interrupt()` — the one
sanctioned **cross-thread** entry point on this otherwise single-threaded
interface. It aborts a runaway evaluation (surfacing as a thrown "interrupted"
exception on the engine thread). Gotchas:

- **QuickJS reuses CHOC's handler — do NOT install your own.** CHOC's
  `QuickJSContext` already installs a `JS_SetInterruptHandler` backed by an
  atomic `shouldCancel`, exposed as the thread-safe `Context::cancel()`.
  `QuickJsEngine::request_interrupt()` just calls `context_.cancel()`. Installing
  a second `JS_SetInterruptHandler` would clobber CHOC's and break `cancel()`.
- **Arming while idle aborts the *next* eval.** QuickJS only clears the cancel
  flag on the next interrupt check, which fires only during JS execution. So
  callers must arm the interrupt only while an evaluation is actually in flight
  (`ScriptInspectorBridge` enforces this).
- **Inspector evaluation is bounded before values become generic CHOC data.**
  QuickJS implements `evaluate_bounded_json()` by walking the live `JSValue`
  with byte, depth, and cycle limits. Keep that serializer in the backend;
  materializing an unbounded object graph first defeats the result-size limit.
  The bridge gives evaluation two seconds, then a fixed 500 ms grace for the
  mandatory realm rebuild, all inside the standalone inspector's three-second
  main-thread RPC fence. The server's owned asynchronous worker leaves the same
  authenticated controller connection free to deliver `Runtime.interrupt` and
  is included in the module-unload shutdown fence.
- **Mainline QuickJS has NO source-line debugger protocol.** No breakpoints,
  stepping, suspended frames, or local-scope inspection — those live only in
  QuickJS *forks* (quickjs-ng / koush) Pulp does not vendor. The scripted-UI
  runtime inspector (`ScriptInspectorBridge` + the inspector `Runtime.*`
  domain) is therefore an honest **evaluate / capabilities / interrupt / device
  logs** console, not a step debugger; `Runtime.getCapabilities` reports
  `canBreak`/`canStep`/`canInspectLocals` = false. A real step debugger is a
  future engine-capability milestone (a debugger-enabled backend, or the Chrome
  DevTools inspector JSC/V8 expose). See `docs/reference/scripted-ui-inspector.md`.

## V8 provider library (how V8 is obtained)

Pulp does **not** build V8 from source, and (since 2026-06) does **not**
use a developer's Homebrew `libnode`. The provider is a **pinned, sealed
prebuilt `libv8`** from the
[danielraffel/v8-builder](https://github.com/danielraffel/v8-builder)
fork — the same pin/fetch/Find pattern as Skia:

1. **Pin:** the `V8` entry in `tools/deps/manifest.json`
   (`determinism.release_assets`, per-platform URL + sha256, tag `v8-m153-15.3.76.5-26cef0256b0e`).
2. **Fetch:** `python3 tools/scripts/fetch_v8_for_release.py <platform>`
   downloads + sha256-verifies + unpacks to `external/v8-build/<platform>/`
   (`include/` + `lib/`). Platforms: `darwin-arm64`, `darwin-x64`,
   `linux-x64`, `linux-arm64`, `windows-x64`, `windows-arm64`,
   `android-arm64`, `ios-simulator-arm64`. The iOS asset is an actual jitless
   simulator `V8.framework`; Pulp validates its provenance and headers but does
   not select or package it as an iOS/AUv3 runtime.
3. **Resolve:** `tools/cmake/FindV8.cmake` finds `external/v8-build/<key>`
   (or a baked `$V8_DIR`, or legacy overrides) and exposes the `v8::v8`
   imported target. The configure log prints
   `-- Pulp V8 provider: <path> (platform key: ...)`.

**Selecting V8 — that's all you need:**

```bash
python3 tools/scripts/fetch_v8_for_release.py darwin-arm64   # once
cmake -S . -B build -DPULP_JS_ENGINE=v8 \
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON                 # no V8_* paths
```

FindV8 resolves the fetched artifact automatically — no `V8_INCLUDE_DIR`/
`V8_LIB_DIR`/`V8_LIBRARY_PATH` needed. Those still exist as **advanced
local-experiment overrides** (point at a hand-built V8). `V8_DIR` points at
a baked V8 (golden VMs: `V8_DIR=~/pulp-v8-build`).

**Three behavior rules:**
- `PULP_JS_ENGINE=auto` **never** pulls in V8 — V8 is strictly opt-in via
  `=v8`. (Previously `auto` + `V8_INCLUDE_DIR` silently enabled it.)
- `PULP_JS_ENGINE=v8` on **iOS** is a configure-time `FATAL_ERROR`. The pinned
  m153 asset is a jitless simulator framework, but Pulp has no device/AUv3 V8
  runtime acceptance or packaging contract. Use QuickJS (the default) or JSC.
- `PULP_JS_ENGINE=v8` on **Android** requires API 29+ for the pinned m153
  provider. Pulp's general Android floor remains API 26; use QuickJS below 29.

**Why the sealed build (the ICU caveat):** the v8-builder `libv8` exports
only the `v8::`/`cppgc::` API and keeps its bundled ICU/zlib/Abseil
internal, so they don't collide with Skia's bundled-but-flat-named
ICU/HarfBuzz at the final link. Confirm any provider is seal-safe with
`nm -gU <lib> | c++filt | grep -cE 'icu_[0-9]+::|absl::'` — only
`v8::internal::` functions whose *signatures* mention those types should
appear, never re-exported `icu_NN::`/`absl::` library symbols. V8 is still
confined to `js_v8_engine.cpp` and never shares a TU with a Skia/Dawn
header.

## Windows sealed v8-builder V8 (clang-cl + Chromium libc++ `__Cr`)

The v8-builder sealed Windows `v8.dll` (e.g. `v8-m153-15.3.76.5-26cef0256b0e`) is a special
ABI lane, NOT a drop-in for MSVC cl. It is built (v8-builder
`build-v8.py:win_gn_args()`) with **pointer compression ON**, **Chromium's
bundled libc++ (`__Cr` ABI namespace)**, sandbox OFF, rtti OFF. So
`v8.dll.lib` exports its `std::`-bearing API mangled into `__Cr@std`
(verify: hundreds of `...@__Cr@std@@` symbols). A consumer built with
**MSVC cl + MSVC STL will not link** — its plain `@std@@` symbols never
resolve against the `__Cr@std` imports. The m153 archive's ABI shape and
required files are inspected and pinned. The following consumer contract was
last link/run proven with m152 (`15.2.124.7`) on the Win11-24H2 arm64 QEMU
golden (x64 V8 under ARM x64 emulation); the m153 consumer link/run remains a
required platform acceptance step:

1. Compile the V8 TU (`js_v8_engine.cpp`) with **clang-cl**, not cl.
   `tools/cmake/PulpV8Windows.cmake` hard-fails configure if the compiler
   is MSVC cl.
2. **`-DV8_COMPRESS_POINTERS`** — mandatory; without it the inline
   `v8-internal.h` tagged-field offsets mismatch the DLL and silently
   corrupt the heap.
3. **Chromium-style libc++ headers** with `_LIBCPP_ABI_NAMESPACE=__Cr`
   (routed via clang-cl's `/clang:-nostdinc++ /clang:-isystem` — bare
   `-nostdinc++` is silently dropped by clang-cl, which is the trap that
   makes a build fall back to MSVC STL). The official Windows LLVM package
   ships clang-cl but **no libc++**, so headers come from an llvm-project
   `libcxx/include` checkout. The `__config_site` must set
   `_LIBCPP_HAS_LOCALIZATION 1` or `<ostream>`/`<sstream>`/`std::endl`
   silently vanish.
4. **A `__Cr`-ABI `libc++.lib`** — `v8.dll` exports NONE of the
   out-of-line libc++ runtime (no `std::cout`, `basic_string`/
   `basic_ostream` ctors/dtors, `operator new`). Build it from
   llvm-project `runtimes` with `LLVM_ENABLE_RUNTIMES=libcxx`,
   `LIBCXX_ABI_NAMESPACE=__Cr`, `LIBCXX_ABI_VERSION=2`,
   `LIBCXX_CXX_ABI=vcruntime`, RTTI ON + exceptions ON (libc++ refuses
   exceptions-on/rtti-off). Link it + `msvcprt.lib` (the latter supplies
   the `__ExceptionPtr*` vcruntime glue), and define
   `_CRT_STDIO_ISO_WIDE_SPECIFIERS=1` to match the lib (else lld-link
   `/FAILIFMISMATCH`).

Wiring: select V8 the normal way (`PULP_JS_ENGINE=v8` after
`fetch_v8_for_release.py windows-x64`, which unpacks the sealed
`v8.dll.lib` under `external/v8-build/`, so `FindV8.cmake` resolves it).
On Windows, additionally supply `PULP_V8_WIN_LIBCXX_INCLUDE` and
`PULP_V8_WIN_LIBCXX_LIB` (the Chromium-style libc++ headers + the
`__Cr`-ABI `libc++.lib`). `core/view/CMakeLists.txt` links `v8::v8` and
then calls `pulp_v8_windows_apply_abi(pulp-view-script)` (no-op off
Windows). Last runtime proof (m152): choc V8 consumer init +
`evaluateExpression("40 + 2") == 42`, `V8::GetVersion() == 15.2.124.7`.
The m153 Windows archive has passed structural ABI inspection but not this
consumer link/run gate. The mac/linux sealed
artifacts use the **system** libc++ (no `__Cr`) and need none of this —
the `__Cr` contract is Windows-only.

**Cross-platform rollout + ship/sign/package** (Windows MSVC `/MT` ABI,
macOS nested-dylib re-signing, Android ABI gating, golden-VM bake) are
tracked in `planning/2026-06-06-v8-sealed-libv8-provider-migration-plan.md`.
macOS arm64 is the proven m153 runtime lane; other platforms are pinned but
pending per-platform verification.

### Sealed v8-builder provider (`FindV8.cmake` → `external/v8-build/`)

The pinned provider is a **sealed** `libv8.dylib` from the
[v8-builder](https://github.com/danielraffel/v8-builder) project: a single
dylib with `@rpath/libv8.dylib` install_name, no external ICU/zlib/Abseil
links, and a pinned runtime version recorded in `tools/deps/manifest.json`.
Fetch it once, then select `v8` — `FindV8.cmake` resolves the unpacked
artifact under `external/v8-build/<platform>/` and exposes the `v8::v8`
imported target; no `V8_*` paths are needed:

```bash
python3 tools/scripts/fetch_v8_for_release.py darwin-arm64   # once
cmake -S . -B build -DPULP_JS_ENGINE=v8 \
  -DPULP_VALIDATE_V8_PROVIDER_STRICT=ON \
  -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON
```

When `PULP_JS_ENGINE=v8`, `core/view/CMakeLists.txt` includes `FindV8.cmake`
(fatal if no sealed V8 is resolved — an explicit `=v8` request never silently
falls back), links `v8::v8`, and then fills in the provider-identity compile
defs from the resolved artifact: `PULP_V8_PROVIDER_KIND` (`v8builder`),
`PULP_V8_PROVIDER_PATH` (`${V8_RUNTIME_LIBRARY}`), and
`PULP_V8_EXPECTED_RUNTIME_VERSION` (parsed from the V8 dependency's pinned tag
in `manifest.json`). The build-tree rpath for `@rpath/libv8.dylib` is handled
by FindV8's imported target. This is the explicit V8 lane only — it does NOT
change the engine default (`auto` stays QuickJS; iOS is a configure-time
`FATAL_ERROR` because Pulp has no accepted device/AUv3 V8 runtime or packaging
contract for the simulator-only jitless provider).

**Identity surface:** `JsEngine::runtime_version()` (V8's
`v8::V8::GetVersion()`) / `provider_kind()` / `provider_path()` /
`expected_runtime_version()` (defaults empty for QuickJS/JSC), forwarded
through `ScriptEngine`. They prove which V8 is *actually linked*, not just
which headers compiled. `examples/threejs-native-demo --print-engine-identity`
emits a parseable `PULP_ENGINE_IDENTITY_BEGIN…END` block; `otool -L` the demo
to confirm `@rpath/libv8.dylib` and no libnode.

**Why the seal matters (observed A/B, 2026-06-05):** Homebrew
`libnode.147.dylib` links external Homebrew ICU and exports ~567 Abseil
symbols; under the full three.webgpu.js workload its bundled Abseil aborts with
`PerTableSeed`/`raw_hash_set.h:456` (SIGABRT) — the cube never renders, though
basic `evaluate()` and all `pulp-test-js-engine` cases still pass. The sealed
v8-builder `libv8.dylib` (no external ICU, 4 incidental absl symbols) renders
the cube cleanly next to Dawn/Skia. The libnode-147 three.js abort is also
documented in `examples/threejs-native-demo/README.md`.

**Strict CTest:** `PULP_VALIDATE_V8_PROVIDER_STRICT=ON` adds
`v8_provider_identity_strict` — a no-skip-pass gate (contrast with
`capture_test.cmake`, which tolerates no-V8/no-Dawn) that asserts the identity
block and renders `--demo cube --capture` to a non-empty PNG.

## Gotchas

### Three.js V8 builds use the pinned sealed libv8 (no Homebrew node)

The native Three.js bridge needs V8. Use the pinned sealed `libv8` — fetch
once, then just select `v8`:

```bash
python3 tools/scripts/fetch_v8_for_release.py darwin-arm64
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_JS_ENGINE=v8 -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON
```

Do not trust an old build directory just because CMake says V8 is enabled.
Check the linked dylib points at the pinned artifact:

```bash
otool -L build/examples/threejs-native-demo/pulp-threejs-native-demo | grep libv8
# → @rpath/libv8.dylib ; rpath includes external/v8-build/<key>/lib
```

If it reports a Homebrew `libnode.*.dylib`, you're on a stale build dir from
before the sealed-libv8 cutover — reconfigure clean. (The former
`-DV8_INCLUDE_DIR=/opt/homebrew/opt/node@24/...` libnode recipe is gone; see
the V8 provider section above.)

> Headless-render gotcha (non-V8): `threejs-native-demo` loads
> `demo.js.template` via `fs::path(__FILE__)`. ccache `CCACHE_BASEDIR`+
> `NOHASHDIR` (CI/VM builds) relativizes `__FILE__` so it resolves into the
> *build* tree (missing) → SIGABRT. Stage the template into the build dir or
> build without ccache. Unrelated to V8.

### Web-API global registration is hybrid native+JS by design

CHOC's `NativeFunction` signature can only carry `choc::value::Value` arguments — JS function values don't round-trip through it. So even though `requestAnimationFrame` / `setTimeout` / `setInterval` look like they "should" be C++-only bindings, the callbacks themselves have to live in a JS-side registry (`__frameCallbacks__`, `__timerCallbacks__`).

What is C++-side: id allocation in `WidgetBridge::__scheduleTimer__`, deadline tracking in `pending_timers_`, and the flush driver invoked from `service_frame_callbacks()`. What is JS-side: the registry table and the `setTimeout` / `requestAnimationFrame` global wrappers (in `web-compat-scheduler.js`) that allocate ids, stash callbacks, and call into the natives.

Don't refactor this into a "pure native" shape — there's no way to do it without copying the entire JS engine's value type into Pulp's bridge layer.

### The mock `GPUBuffer` must COMMIT `getMappedRange()` writes on `unmap()`

`web-compat-document-gpu-mock.js`'s `__createMockGPUBuffer` backs each buffer
with a JS `_bytes` Uint8Array that the buffered-draw serializer
(`web-compat-canvas-gpu.js`) ships to the native bridge. WebGPU's mapped-write
contract is: `getMappedRange()` returns an ArrayBuffer the caller writes into,
and `unmap()` commits those writes to the buffer. An ArrayBuffer cannot alias a
*sliced* view of `_bytes`, so `getMappedRange()` MUST hand back a standalone
range (seeded from `_bytes`), record it, and `unmap()` MUST copy each recorded
range back into `_bytes`. The original code returned `_bytes.buffer.slice(...)`
(an independent copy) with a no-op `unmap()`, so every mapped write was silently
dropped. Three.js's WebGPUBackend uploads all geometry via
`createBuffer({mappedAtCreation:true})` → `new T(getMappedRange()).set(...)` →
`unmap()`, so vertex/index buffers arrived all-zero and meshes collapsed to a
point — only `queue.writeBuffer`-backed uniforms survived. See the
threejs-bridge skill for the full runtime chronology.

### setTimeout(fn, 0) takes the microtask path, not the timer queue

`setTimeout(fn, 0)` deliberately bypasses `__scheduleTimer__` and routes through `Promise.resolve().then(...)` so it drains on the next `pump_message_loop()` call. This matches React's scheduler expectations and makes tests deterministic (no host frame loop needed). Positive-delay timeouts go through the native deadline tracker and only fire when `service_frame_callbacks()` runs.

If a consumer reports "my setTimeout(fn, 1) never fires", check that the host is actually calling `service_frame_callbacks()` from its frame loop — that's the drain hook for non-zero delays.

### `display: flex` defaults to `flex-direction: row`

Pulp's underlying widgets default to `FlexDirection::column` (RN convention). The CSS web platform default for `display: flex` is `flex-direction: row` — children lay out horizontally. Imported / extracted designs assume the web default, so `web-compat-style-decl.js` explicitly emits `setFlex(id, 'direction', 'row')` whenever a `CSSStyleDeclaration` resolves `display: flex` and the consumer has NOT also declared `flexDirection`, `flex-direction`, or a `flexFlow` shorthand that includes a direction token.

Order independence is intentional. Both of these end up column:
```js
el.style.flexDirection = 'column'; el.style.display = 'flex';
el.style.display = 'flex'; el.style.flexDirection = 'column';
```
The setter trap stores into `_props` BEFORE `_applyProperty` runs, so the display handler can see a previously-declared direction and skip the row default. A later explicit `flexDirection` overrides the row default through the normal handler.

`flexFlow` is content-aware: `flexFlow: 'wrap'` does NOT block the row default (CSS shorthand semantics — omitted `flex-direction` defaults to row), but `flexFlow: 'column wrap'` does. The check uses a `\b(row|column)\b` regex against `_props.flexFlow`.

Not changed by this fix: `createCol` / `createRow` / `createPanel` C++ paths preserve their explicit direction; typed React props in `pulp-react/prop-applier.ts` route directly through bridge setters and don't touch `style`.

### A restored style value is often the empty string, so honor CSS initial values

`web-compat-document.js`'s `:hover` translator snapshots `el.style[prop]` on
`mouseenter` and assigns it back on `mouseleave`. For the ordinary case — an
element that never carried an *inline* value for the hovered property — the
snapshot is the empty string, so the restore assigns `""`, not a number.

That makes `parseFloat(resolved) || 0` a trap in `_applyPaintProp`: `""`
parses to `NaN`, `NaN || 0` is `0`, and the widget is left fully transparent
while its rect, visibility and clip box are untouched — the element looks
deleted rather than un-styled, which sends you hunting in layout instead of
paint. When a paint property cannot parse its resolved value, fall back to
that property's **CSS initial value** (`opacity` → `1`), never to zero.

The same shape applies to any future numeric paint property routed through
this path. A test for it must NOT pre-assign the inline value, or it
exercises the parseable branch and passes regardless; assert
`isNaN(parseFloat(el.style.<prop>))` after the leave as a control.

### An empty CSS colour means REMOVE the declaration, not "leave it alone"

The empty string is CSSOM's spelling of removal: `el.style.background = ""`
deletes the declaration. Both halves of Pulp's colour lane used to read it as
"nothing to do" instead — `web-compat-style-decl-paint.js` skipped the bridge
call because `parseCSSColor("")` is falsy, and `style_visual_api.cpp` skipped
the paint because `hex.empty()`. Two independent no-ops for the same value, so
fixing either alone changes nothing and reads as "the fix did not work".

The symptom is a colour that will not go away. A dropdown deactivates a row by
assigning its captured base background, which is `""` for a row that was never
styled — so every row the highlight visited stayed lit, and the highlight read
as an accumulating frontier rather than a moving one. The element's own
bookkeeping stayed correct throughout, which is why an attribute assertion
(`data-pulp-popup-active`) passed while the pixels were wrong.

`View` already had `clear_background_color()` / `clear_background_gradient()`
and `has_background_color()`; the bridge simply never called them. Any new
colour-valued bridge entry point needs the same three-way split — empty
clears, parseable sets, unparseable is the only no-op — and the test for it
must assert `has_background_color()` on the view, with the *still-lit* element
as a positive control so a stuck colour cannot be confused with one that never
painted. `"transparent"` is NOT a substitute: `css_color.cpp` maps it to
`rgba(0,0,0,0)`, which paints nothing but still counts as a declaration.

`setTextColor` (`typography_api.cpp`) still drops empty and has no
`clear_text_color()` primitive, so `@pulp/react`'s documented removal contract
(`textColor` removed → `setTextColor(id,"")`) is currently inert on the native
side. Fix that half the same way when it next bites.

### `confirm_failure.sh` cannot verdict a `.js` prelude edit

Preludes are embedded into a generated `build/core/view/web_compat_preludes_gen.cpp`,
so a `.js` file produces no compile line of its own and the script refuses a
verdict — INCONCLUSIVE, every time, however real the edit. Prove a prelude
change by counting the changed text in that generated file instead: break the
JS, rebuild, confirm the count drops to zero AND the test goes red, restore,
confirm both come back. The count is the positive control that the edit
reached the binary at all.

Note also that `core/view/js/web-compat.js` is a monolith that is **not**
embedded. The embedded lane is the split `web-compat-*.js` set listed above,
so an edit to the monolith is inert — verify against `PULP_JS_PRELUDES`
before concluding a JS change had no effect.

### Canvas2D `textBaseline` initializes to `alphabetic`, not `top`

The Canvas2D initial value for `textBaseline` is `"alphabetic"`: the `y`
handed to `fillText` IS the baseline. Browser-authored canvas code that never
assigns `ctx.textBaseline` — which is most of it — relies on that, so
defaulting to `top` treats the same `y` as the top of the em box and pushes
every such caption down by one ascent. The symptom is subtle: text still
draws, in roughly the right place, just consistently low.

The default lives in three places that must agree, and changing one alone
produces a shim/native split that only shows up in a render:
`core/view/js/web-compat-canvas.js` (the shim's own `this.textBaseline`),
`core/view/src/widget_bridge/canvas2d_api.cpp` (the `canvasSetTextBaseline`
default argument and its string→int mapping), and
`core/view/src/canvas_widget.cpp` (the replay's initial `TextBaseline`).

`canvas::TextBaseline` enumerators are **append-only**: the bridge records the
enum's integer value into the command stream, so reordering it would silently
reinterpret every previously recorded `top` / `middle` / `bottom`.

### A canvas path run is buffered — any new shim emitter must flush it first

`moveTo` / `lineTo` in `core/view/js/web-compat-canvas.js` do not cross the
bridge per point. They accumulate into `this._pendPts`, and `_fp()` ships the
whole run as one `canvasPathPolyline` call. This exists because the crossing,
not the engine, is the cost on a canvas-heavy UI: a band drag measured
959,968 `canvasLineTo` calls in 115,880 runs — 81% of them inside runs of 64
points or more, and every run immediately preceded by a `canvasMoveTo`.
Collapsing a run into one call removes roughly half of all bridge crossings
without changing a single recorded command. Reach for this shape before
reaching for a different JS engine; swapping QuickJS for JSC or V8 does not
make a crossing cheaper.

The buffering has one invariant, and it is easy to break by accident: **any
shim method that calls a `canvas*` bridge global must call `this._fp()` as
its first statement.** Without it the new command is recorded ahead of the
buffered points, so a `stroke()` paints an empty path, or a `fillStyle`
applies to the wrong subpath. Nothing about the recorded stream looks wrong
in isolation — the commands are all present, just out of order.

`tools/scripts/check_canvas_path_flush.py` (ctest `canvas-path-flush-lint`)
enforces it. It parses the prototype methods out of the shim by brace depth,
so it fails closed if it stops recognizing the file: finding zero
bridge-emitting methods is treated as an error, not a clean result. `moveTo`,
`lineTo`, `rect` and `_fp` are the only exemptions, because they own the
buffer.

### CSS-shim gap fills — translator vs. bridge contract

Four classes of "silent drop" recur in `web-compat-style-decl.js`. When
adding a style property, walk all four before declaring done:

1. **Missing `case "X":`** — the property is nowhere in the switch, so
   `el.style.X = ...` writes to `_props[X]` and never reaches the
   bridge. Harness verdict: NOT-IMPL. Example: `backdropFilter`
   already had a bridge `setBackdropFilter`, but still needed the
   JS route.

2. **Coalesced shorthand only** — the shorthand routes (e.g.
   `textDecoration`) but the longhands (`textDecorationLine` /
   `-Color` / `-Style`) silently no-op. Per-attribute longhands MUST
   route to per-attribute bridge setters so a previously-set sibling
   isn't clobbered — the same pattern as the per-side border setters.
   Don't try to coalesce three independent
   property assignments into a single `setX(id, line, color, style)`
   call; the JS shim iterates assignments in source order, so the
   first call would always overwrite the next two with defaults.

3. **Keyword-vs-numeric coercion** — CSS / RN accept both keyword
   forms (`fontWeight: 'bold'`) and numeric (`fontWeight: 700`).
   `parseInt('bold')` returns `NaN`, the `|| 400` fallback then
   silently maps bold → normal. Translate before reaching the
   bridge. The same translation lives in
   `packages/pulp-react/src/prop-applier.ts` (`_normalizeFontWeight`)
   for parity with React-Native style objects — both paths must
   emit the same numeric weight.

4. **Not a CSS property at all** — React Native contributes style keys
   CSS never had (`hitSlop`). The shim still has to carry them,
   because the RN-style object and `el.style.X` are the same surface
   to a consumer, and the three-class walk above applies unchanged.
   The extra cost is that there is no CSS shorthand to inherit from,
   so `packages/pulp-react/src/prop-applier-paint.ts` has to accept
   BOTH the RN forms (a number, or `{top,right,bottom,left}`) and the
   CSS-shorthand form a designer will reach for anyway
   (`hitSlop: '12px 2px'`), and normalize to the four-value bridge
   call. Emitting the number form only is the silent half-fix: the
   object form then writes `[object Object]` into one edge.

**`__cssProperties__` array gotcha:** properties also need an entry in
the `__cssProperties__` array near the bottom of
`web-compat-style-decl.js` for `el.style.X = ...` to set the trap.
Properties only reachable via `setProperty('x-y', ...)` work without
this because `setProperty` converts kebab to camel and goes through the
same `_applyProperty` switch — but JSX consumers writing
`style.backdropFilter = "blur(10px)"` need both the array entry AND the
`case` block.

**Verifier harness ground truth:** run
`python3 tools/harness/verifier.py --surface=css --json` before and
after a CSS-shim change. The JSON delta tells you which entries
reclassified between NOT-IMPL → DIVERGE → PASS, and a `drift_count`
that drops without manual `compat.json` edits is the sign that the
catalog status was already correct and only the JS route was missing.

**Sticky-state setters (Canvas2D shadow / direction / filter / …):**
when wiring a new `ctx.X` setter that the bridge captures as sticky
state, follow this checklist or you'll land a silent no-op:

1. **Local field + spec default** on `CanvasRenderingContext2D` —
   numeric `0` for shadowBlur, string `"none"` for filter, etc.
2. **`_sentX` cache field** initialised to `null`; the
   `_syncXState` helper only flushes when the value differs.
3. **`_syncXState` helper** — coerce defensively (unknown strings
   → spec default), gate `isFinite()` for numeric fields per HTML5
   ("non-finite assignments are silently ignored"), then call the
   bridge fn and update the cache.
4. **Wire `_syncXState()` into every consuming draw** — fillRect,
   stroke, fillText, drawImage, etc. The fillRect set is the
   minimum; text + image draws need it too if the state visually
   affects them.
5. **Invalidate the cache in save() and restore()** — the C++
   GState pops the value, so the post-restore draw must re-flush.
   Forgetting this is a one-frame lag invisible in single-frame
   tests.
6. **Bridge fn registration** in `core/view/src/widget_bridge.cpp`
   — record a `CanvasDrawCmd` with the right `int_val` / `extra`
   / `text` field per the enum docstring.
7. **Dispatch + `cmd_type_to_string`** in
   `core/view/src/canvas_widget.cpp` — add to the paint switch AND
   the trace-logger name table at the top of the file.
8. **`Canvas` base virtual + `RecordingCanvas` capture** — even
   when Skia / CG have nothing to do, RecordingCanvas is what the
   canvas2d-shim tests assert against.

This pattern is now used for `shadow*`, miter / image-smoothing,
direction, and filter setters. Copy the same shape for the next
canvas2d catalog setter.

### String-valued custom CSS properties (`var(--mono)` etc.)

`setProperty('--name', value)` in `web-compat-style-decl.js` (and the
mirror in `web-compat.js`) has THREE tiers, not the original two:

1. **Length** (`parseCSSLength`) → `setMotionToken` writes
   `theme.dimensions[name]`.
2. **Color** (`parseCSSColor`) → `applyTokenDiff` writes
   `theme.colors[color.name]`.
3. **String fallback** → `setStringToken(name, value)` writes
   `theme.strings[name]`. This is what catches font families
   (`--mono: "JetBrains Mono"`) and any other arbitrary string.

Without tier 3, font-family-shaped custom properties were silently
dropped at set time and `var(--mono)` resolved to `0` (the
`getMotionToken` empty-token return). `getPropertyValue` mirrors the
same tier order — string token first, then numeric — so the
round-trip works.

`getStringToken` and `setStringToken` are bridge fns registered in
`core/view/src/widget_bridge.cpp` next to `getMotionToken` /
`setMotionToken`.

The React-side mirror lives in `packages/pulp-react/src/prop-applier.ts`
as `_resolveVar(value)`, with the same lookup tiers (developer-set
`__pulpCssVars` registry → `getStringToken` → `getMotionToken` →
fallback). Call it from every string-valued style prop case that
might receive `var(--name)` from JSX — see the `fontFamily` /
`color` / `borderColor*` / `outlineColor` / `textDecorationColor` /
`textShadowColor` / `shadowColor` / `background` cases for the
canonical wiring.

### Native global name ownership lives in the `JsEngine` base, not per-backend

`JsEngine::register_function`, `register_host_object`, and
`register_promise_function` are now non-virtual on the base class. They
each call `claim_native_symbol(name)` (which throws on duplicates) and
then delegate to the backend's `register_*_impl` hook. Backends only
implement the `_impl` half.

Why this matters: a duplicate-name registration is a silent bug that
either shadows the previous binding (QuickJS-shaped behavior) or
crashes at first call (JSC/V8-shaped behavior), and the symptom path
depends on which engine the user picked at build time. Catching it at
registration time, in the base class, makes the failure
backend-independent and immediate.

When adding a new registration surface (Promise variants, typed-array
bridges, host-object subforms, etc.), follow the same pattern:
public non-virtual entry point on `JsEngine` → claim the name → call a
new `_impl` hook each backend implements. Do not add a duplicate-name
table inside an individual backend; that path drifts.

`pulp-test-widget-bridge-api-contracts` scans for accidental literal
re-registrations across `WidgetBridge` (function, host-object, and
promise-function names) and is the cheapest tripwire if a split-up
registration module forgets the contract.

### Native events must have one DOM fan-out owner

When an element event is delivered through the shared `__dispatch__` path, its
per-event registration callback should only keep the native channel alive. Do
not dispatch the same DOM event there too: one wheel tick will otherwise reach
every listener twice even though engine-specific tests may still look healthy.

This includes append-time auto-registration for React root delegation:
`__pulpRegisterAutoDomEvents__` installs no-op click/pointer callbacks solely to
arm native delivery. `__dispatch__` owns the only Element dispatch, while the
native pointer registrar emits the matching mouse event as a separate channel.

Direct `@pulp/react` handlers return the internal
`__pulpEventPropagation` marker from their synthetic-event wrapper. Pointer
payloads carry a per-native-dispatch token so `__dispatchCallbackOnly__` can
suppress later native ancestor callbacks. Keep the token scoped and reentrant,
and key cancellation by both token and event name: cancelling `pointerdown`
must not silently cancel its independent compatibility `mousedown`. Level 1
(`stopPropagation`) still allows remaining same-target listeners; level 2
(`stopImmediatePropagation`) does not.

### The popup owner claims from the click, not from focus — and must ignore its own clicks

`__pulpPopupDefaultHandle__` (in `web-compat-document.js`) is the default
keyboard/dismiss state machine for any `aria-haspopup` trigger. It can only
answer ArrowUp/ArrowDown/Enter/Escape for a menu it has taken ownership of, and
it paints the row highlight from exactly one place — `paint()`, the sole writer
of `data-pulp-popup-active` and of the highlight background. If ownership never
happens, both the navigation and the highlight are silently absent, and they
fail together: one cause, two symptoms.

Two traps around that ownership:

- **A keydown path alone is not ownership.** The owner's keydown branch needs
  `triggerFrom(document.activeElement)`, and `document.activeElement` is written
  only by `Element.prototype.focus`. A native mouse click never runs one, so on
  the real user path that branch is dead. Ownership on that path has to come
  from the pointer/click the user actually made. A test that calls
  `trigger.focus()` in setup hand-satisfies the one condition the mouse path
  cannot, so it passes over a defect a user still sees — if a popup test focuses
  the trigger, it is not covering the mouse-opened case.
- **The menu does not exist yet when the click arrives.** The app's own click
  handler creates the popup, and a host that defers dispatch plus a reconciler
  that commits a tick later mean a single synchronous `activate()` inspects a
  DOM with no popup and gives up. Re-offer across a few frames instead of
  deciding once.
- **The owner clicks things itself** — a trigger to commit or close, an option
  to mirror a keyboard open onto the app's handler. Once element clicks are
  offered to the owner, those come back in as if a user had pressed the control,
  and a close reads as an open. Wrap owner-issued clicks in a counter and return
  at the *very top* of the handler while it is non-zero: above the stale-state
  sweep, not merely inside each branch. The sweep runs first, sees a popup the
  app has already removed, and dismisses the state the in-flight branch still
  needs to hand focus back to its trigger.

Restoring a row's background is its own trap: `parseCSSColor("")` returns null
and `_applyPaintProp` silently drops it, so assigning the empty string leaves
the highlight on every visited row, while `"transparent"` parses to `#00000000`
and paints a transparent fill *over* a background that came from a non-script
source. Remove the property and clear the native background instead
(`getBackground` / `clearBackground` on the style bridge).

Note that `__pulpActivateMaterializedElement__` invokes the React callback
directly and never enters `Element.prototype._dispatchEvent`, so a fixture that
opens a menu through it gets a menu no popup owner has claimed. Drive the
production line (`__dispatch__(el._id, "click", …)`) when the fixture is meant
to stand in for a user's mouse.
### A popup's keyboard cursor seeds from the marked selection, not an edge

`web-compat-document.js`'s semantic popup default opens a `role="listbox"` /
`role="menu"` with its cursor on the option the author marked as selected, and
only falls back to the `edge` the caller asked for (`"first"` for ArrowDown,
`"last"` for ArrowUp) when nothing is marked. `selectedIndexIn()` reads, in
descending order of authority: `aria-activedescendant` on the trigger or the
popup, `aria-selected="true"`, `aria-checked="true"`, the option's `checked`
property, then `aria-current`. Assigning the `checked` property writes no
attribute in this shim, so it is a genuinely separate signal from
`aria-checked`.

Two consequences for authors of scripted UI:

- **A listbox that marks nothing gets edge behaviour.** There is no inference
  from the trigger's own text: matching trigger text against option text would
  mis-seed on duplicate labels and on triggers that decorate the value
  ("Bands: 64"), so the shim does not do it. An app whose menu highlights the
  wrong row on open is missing `aria-selected` on its options — which is also
  the ARIA requirement for `role="option"` inside a listbox.
- **This matches the native widget.** `ui_components.cpp`'s `ComboBox::open`
  seeds `hover_index_ = selected_`. The scripted path and the native path now
  agree, so a design that moves between them keeps the same first highlight.

## ESM support per engine

| Engine | Public ESM API | Status |
|---|---|---|
| **V8** (pinned sealed `libv8` on desktop/Android) | `import.meta`, dynamic `import()`, `setModuleLoaderDelegate`-equivalent | Full ESM. Used by `examples/threejs-native-demo/main.cpp` for macOS Three.js. |
| **JSC** (system framework on iOS) | NO public ESM module loader on iOS | ❌ `JSScript.h` + `JSModuleLoaderDelegate` are private. Shipping them in an `.appex` risks App Store rejection. |
| **JSC** (system framework on macOS) | `JSScript` exposed via framework headers | 🟡 Available, but typically unused since macOS uses V8. |
| **QuickJS** (vendored under `external/quickjs/`) | Full ESM via `JS_NewModule*` | ✅ Used as a fallback when V8 is unavailable + jitless V8 isn't an option. |
| **Hermes** | Limited (no full ES module support) | ❌ Not viable for Three.js. |

**iOS Three.js workaround**: Bundle the ESM source into a self-contained IIFE that registers exports on `globalThis.THREE`, then JSC `evaluate()` it. The bundler is at `tools/scripts/bundle_threejs_for_jsc.mjs`; the iOS NSBundle loader at `core/view/src/threejs_resources_apple.mm`; the .appex build-time wiring at `tools/cmake/PulpAuv3.cmake`. Pattern is general — any ESM library can be bundled this way for the JSC iOS lane.

**Bundler implementation note**: The bundler delegates to esbuild (pinned in `tools/scripts/package.json` at 0.25.10). The earlier regex-only pass could not resolve sibling `import { ... } from "./three.core.js"` statements that Three.js's webgpu entry depends on, which caused JSC parse errors at runtime ("expecting `(`"). esbuild handles ESM resolution + http: import stripping out of the box. The bundler auto-installs esbuild on first run via `npm install --prefix tools/scripts/` when `node_modules/esbuild` is missing — no manual setup needed.

See `.agents/skills/{ios,threejs-bridge}/SKILL.md` for the runtime contract.

## Diagnostics for silent JS failures

Several silent-failure traps were caught during the iOS-D.3c Three.js bring-up. Each now has an explicit diagnostic surface in the engine layer.

### QuickJS unhandled-promise-rejection tracker (`core/view/src/js_quickjs_engine.cpp`)

QuickJS does not call JS-side `addEventListener("unhandledrejection", ...)` handlers. Without a host hook, a rejected promise with no `.catch` is silently dropped. The worst case is the `new Promise(async (resolve, reject) => { ... })` anti-pattern: a sync throw inside the async executor rejects the *inner* async function's promise (not the outer), and the outer Promise sits in pending forever.

`install_quickjs_rejection_tracker()` calls `JS_SetHostPromiseRejectionTracker` in `QuickJsEngine()`'s constructor and logs unhandled rejections + stack via `runtime::log_error("PULP_QJS_UNHANDLED_REJECTION: …")` / `PULP_QJS_UNHANDLED_REJECTION_STACK: …`. This single hook surfaced the real Three.js init error (TypeError on `self` not defined) in one shot after hours of fruitless other debugging — turn it on, grep for `PULP_QJS_UNHANDLED_REJECTION` in the log.

### JSC ObjC-exception bridge (`core/view/src/js_jsc_engine.mm`)

`[JSContext evaluateScript:]` can throw `NSException` for malformed scripts (bare `import` statements, syntax errors that the parser can't recover from). Without `@catch(NSException*)`, the exception unwinds through the Objective-C runtime and surfaces in C++ as the useless string "unknown exception". Now the engine catches `NSException` + falls back to `id`, formats the reason + name, and rethrows as `std::runtime_error` with the actual JSC error message. Grep for `PULP_JSC_EVAL_NSEXCEPTION` / `PULP_JSC_EVAL_OBJC` in logs to find scripts that JSC's parser rejected.

### `self` as a window alias in the web-compat document shim (`core/view/js/web-compat-document.js`)

Per browser spec, `self`, `window`, and `globalThis` all reference the same object in a page context. Three.js's `Animation` class (and many other libraries) keys its `requestAnimationFrame` lookup on `self`:

```js
this._context = typeof self !== "undefined" ? self : null;
```

If `self` is undefined, libraries set their context to `null` and crash with `TypeError` on the next rAF call. `globalThis.self = window;` is the spec-conforming fix and is now installed alongside the other window-global aliases (`localStorage`, `sessionStorage`). Holds for any standards-compliant library that follows the browser globals convention.

### `eval_or_throw` logs the JS error before wrapping (`core/view/src/widget_bridge.cpp`)

When `WidgetBridge` evaluates user JS via `eval_or_throw`, the catch chain re-throws as `std::runtime_error("failed to evaluate <name>: <err>")`. A cross-translation-unit `std::exception` typeinfo mismatch can cause the caller's `catch(const std::exception&)` to fall through to `catch(...)`, which then loses the original message. The fix: `runtime::log_error("PULP_EVAL_THROW: name={} js_len={} ..._error={}", ...)` is called *before* re-throwing in all three catch branches, so the JS error reaches the log regardless of whether the downstream catch matches. Grep `PULP_EVAL_THROW` to find the actual error text when "unknown exception" surfaces upstream.

### The web-compat preludes are engine-agnostic — a green V8/macOS test does NOT prove the JSC/iOS path

The `web-compat-*.js` mocks (`web-compat-document-gpu-mock.js` etc.) are loaded identically under QuickJS, JSC, and V8 — so a *logic* bug in a mock behaves the same on every engine, but the surrounding native path may not. A WebGPU-mock `writeBuffer` bug (treating a TypedArray `dataOffset`/`size` as bytes instead of element counts) can pass the macOS V8 headless smoke because that lane does a one-shot Skia **pixel readback**, while the live iOS AUv3 path depends on the presentable swapchain. Two lessons: (1) when you change a `web-compat-*.js` mock, add a focused assertion of the exact code path you touched (the smoke's whole-array `writeBuffer` masked the explicit five-arg element-count form); (2) "green on the V8 headless lane" is necessary but not sufficient for the JSC/iOS live present path — that path must be verified on a device/simulator screenshot, never inferred from the readback test.

### A colour crosses the JS/native boundary as a STRING — resolve CSS Color 4 on the JS side

`web-compat-style-decl-paint.js` hands a gradient to the native side as ONE
string and the stops are parsed *there*, by a colour parser that does not know
`oklab()` / `oklch()` / `lab()` / `lch()` / `color()`. An unrecognised function
name falls back to **white**, so a translucent bloom paints as an opaque white
blob rather than failing visibly.

This is not an exotic case. Chrome serializes every `color-mix(in oklab, …)` to
`oklab(…)`, which is the idiom generated panels use for glows, blooms and
shadows — so the modern-colour path is the *common* path, not the edge. Measured
on one stop: it rendered `(254,254,254)` where the design asked for `#58f0ff` at
48%, with `G - R = 0` at every blur radius (a flat grey/white tell, since the
requested colour is strongly cyan).

`parseCSSColor` on the JS side already resolves all of these correctly, so
resolve to hex *before* the string crosses over. Do NOT grow a second colour
parser on the native side: two parsers for one syntax must then be kept in step
forever, and the failure mode when they drift is a silently wrong colour rather
than an error.

Two traps worth keeping when writing the rewrite pattern:

* **Exclude nested parens.** These colour forms take plain numeric components,
  so matching non-greedily without recursion keeps a `calc()` inside a stop
  *position* from being swallowed by the colour match.
* **Leave anything unparseable exactly as-is.** The native side may still
  understand a form the JS parser does not, and a silent rewrite to a wrong
  colour is worse than passing the original through untouched.

The same boundary explains why `box-shadow` needed a colour-**first** branch:
CSS allows the colour on either side of the offsets and Chrome serializes it
first (`oklab(0.97 0.008 -0.014 / 0.18) 0px 1px 0px 0px inset`). Resolve
function colours to hex *before* any offset split — every offset match divides on
whitespace, and `oklab(…)` carries both spaces and a slash, which tears the
colour apart in either ordering.

**Anchor the offsets regex.** Unanchored, it matches *inside* a colour-first
shadow: given `#f7f3ff2e 0px 1px 3px 0px` it starts at the offsets and takes the
trailing `0px` as the colour, yielding a shadow that parses "successfully" with
garbage in it — strictly worse than not matching, because the colour-first branch
then never runs.

**Do not test this with a round-trip assertion.** Reading `el.style.boxShadow`
back returns whatever string was assigned whether or not it ever parsed, so a
round-trip passes against a shadow that never reached the renderer. Export the
parse helper and assert its decomposition directly.

### A style write that changes nothing must not cross the bridge

`CSSStyleDeclaration._applyProperty` skips a write whose raw string equals the
one already applied for that property. This is not a micro-optimisation: a
stylesheet re-application pass rewrites *every* matched declaration on every
pass, so an interaction that changes no fonts still re-sent every font property
every frame, each one paying `var()` resolution, per-property parsing and a
bridge call to set a value the widget already held.

Four things about the cache are load-bearing, and all four are easy to break:

- **It is keyed on the RAW string, before `var()` resolution — and `var()`
  values are never deduped.** They resolve against live theme tokens, so the
  same raw string can legitimately produce a different applied value with no
  write to observe.
- **A property only trusts its cache while nothing sharing its widget state has
  been applied since.** Several CSS properties reach one piece of widget state
  (`visibility` and `opacity` both drive `setOpacity`; shorthands expand over
  their longhands), so caching properties independently would let a stale entry
  suppress a write the widget needs. `_DEDUP_GROUP` / `_DEDUP_MEMBERS` in
  `web-compat-style-dedup-table.js` partition the properties into groups that
  share state; applying one member drops the cached entries of the others.
  **That table is generated, not hand-written** — `tools/scripts/style_dedup_table.py`
  reads the `_apply*Prop` handlers, records which bridge function and which
  literal sub-key each `case` reaches (`setFlex(id, "margin_top", v)` — the
  sub-key, not the function, is the real granularity), and unions the properties
  that collide. A computed sub-key conflicts with every sub-key of its function.
  Only *writes* count: a `get*` bridge call reads state, and treating one as a
  slot would merge every property that resolves a `var()` into a single group
  and erase the optimisation while still rendering correctly.
  A hand-maintained alias table would have to stay exhaustively correct forever
  and fails *visually and silently* when it does not; a `ctest` re-derives this
  one and fails if it has drifted from the handlers. A property the extractor
  cannot classify is absent from the table, and an absent property is never
  cached **and** drops the whole element's cache when it applies — so being
  missing costs speed, never correctness.
- **A blanket wipe on every apply looks safer and is inert.** That was the first
  implementation: within a single stylesheet pass each property's apply wiped
  the previous property's entry, so the cache retained only the last-written
  property and every write missed on the next pass, self-perpetuating. It
  measured a 1x speedup over three runs — correct, and worth exactly nothing.
  If you change this cache, re-run the `[style][dedup][benchmark]` case; it
  A/Bs the guard inside one binary and prints the apply ratio.
- **Anything that changes widget state outside `el.style` must call
  `__invalidateStyleCache__(el)`.** The cache describes a widget, not an
  Element, so it is wrong the moment the widget is recreated under a surviving
  Element (`_ensureNative`, `_reparentNative`, the `appendChild` /
  `removeChild` / `replaceChild` mount paths, `__forgetWidgetCallbacks__`) or
  another path writes a slot it claims (media-attribute replay, the `hidden`
  setter, dialog `show` / `close`). **If you add a bridge setter call outside
  the `web-compat-style-decl-*` handlers, you are adding one of these.** The
  failure mode is a later identical style write being skipped as redundant
  against a widget that no longer holds that value — which renders wrong with
  every test still green, because `el.style.foo` reads back the assigned string
  whether or not it ever reached the widget. Assert against real `View` state.
