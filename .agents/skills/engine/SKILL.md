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
| `Element.nodeType` (=1) / `nodeName` (=tagName) | `web-compat-element.js` | React reconciler walks every node; bails before first commit without these (pulp #468) |
| `Element.ELEMENT_NODE` / `TEXT_NODE` / `COMMENT_NODE` constants | `web-compat-element.js` | `node.ELEMENT_NODE === 1` fast-paths in React |
| `createTextNode` → nodeType=3 + nodeName='#text' + `data`/`nodeValue` mirrors | `web-compat-document.js` | DOM Level 1 text-node spec; React's text-update path |
| `createComment` → nodeType=8, `createDocumentFragment` → nodeType=11 | `web-compat-document.js` | React portal sentinels + batched commits |
| `MutationObserver` / `IntersectionObserver` / `ResizeObserver` / `PerformanceObserver` no-ops | `web-compat-observers.js` | `typeof X === 'function'` feature-detects pass; React skips because no events ever fire |
| `XMLHttpRequest` no-op + spec readyState constants | `web-compat-observers.js` | React dev-mode error-stack lookup probes XHR |
| `Element.scrollTop`/`scrollLeft`/`scrollWidth`/`scrollHeight` (returns 0) | `web-compat-observers.js` | React dev focus warnings |
| `queueMicrotask` (Promise-based shim) | `web-compat-scheduler.js` | React 18 concurrent scheduler |
| `MessageChannel` + `MessagePort` (microtask-deferred postMessage) | `web-compat-scheduler.js` | React 18 scheduler prefers MC; falls back to setTimeout if missing (perf cliff, not a blocker) |
| `URLSearchParams` polyfill | `web-compat-scheduler.js` | React error-source URL parsing |
| `requestAnimationFrame` / `cancelAnimationFrame` (driven by native `__requestFrame__`) | `web-compat-scheduler.js` | Bundled-React frameworks reference the standard names; without this each consumer ships a ~80-line `shim.js` (pulp #915) |
| `setTimeout` / `clearTimeout` / `setInterval` / `clearInterval` (driven by native `__scheduleTimer__` deadline tracker) | `web-compat-scheduler.js` | React's scheduler yield path + plugin code; setTimeout(fn, 0) drains via microtask, positive delays drain in `service_frame_callbacks()` (pulp #915) |
| `performance.now()` (driven by native `__performanceNow__`) | `web-compat-scheduler.js` | Bundled-React modules read `performance.now` at module-eval time before the legacy `window.performance` shim is reachable (pulp #915) |
| Mirror block onto `window` (rAF/cAF/sT/cT/sI/cI/MC/qM/perf) | `web-compat-scheduler.js` | React 18's scheduler reads `window.setTimeout` / `window.requestAnimationFrame` specifically; the global must be reachable through both names (pulp #915) |

When adding new framework support, check the engine capability
comparison before assuming a polyfill is missing — the entry above is
exhaustive for React 18 dev. Add new files to `core/view/CMakeLists.txt`'s
`PULP_JS_PRELUDES` list AND to the `eval_or_throw` block in
`core/view/src/widget_bridge.cpp` (`embed_js.cmake` only embeds the
constants; the bridge constructor evaluates them).

### Canvas2D surface coverage (issue-916, issue-964)

`web-compat-canvas.js` exposes `CanvasRenderingContext2D.prototype`
with the standard methods plus the gap-list closures from issue-916
and the FilterBank-parity additions from issue-964:

| Method | Notes |
|--------|-------|
| `measureText(text)` | Returns a full HTML5 `TextMetrics` object — `width` + `actualBoundingBox{Left,Right,Ascent,Descent}` + `fontBoundingBox{Ascent,Descent}`. Routed through `canvasMeasureText` which calls `SkiaCanvas::measure_text_with_font` for surface-less metrics. |
| `drawImage(img, …)` | 3 / 5 / 9-arg signatures supported; the 9-arg `(sx,sy,sw,sh,dx,dy,dw,dh)` form currently ignores the source rect — file a follow-up if a plugin needs sprite-sheet slicing. |
| `setLineDash([…])` / `getLineDash()` | Even-length patterns are taken verbatim; odd-length patterns are duplicated per the HTML5 spec. Phase comes from `lineDashOffset`. |
| `getImageData(x,y,w,h)` | Returns `{data: Uint8ClampedArray, width, height}`. The bridge currently returns zero-filled pixels (no live surface handle from JS-call context); consumers that need real pixels should round-trip through a render-host integration. |
| `putImageData(img, dx, dy)` | Decodes the typed array to base64 across the bridge and applies via `Canvas::write_pixels` on backends that implement it (Skia today). |
| `save()` / `restore()` (#964) | Forward to `canvasSave` / `canvasRestore`. JS-side caches of last-pushed text/line/global state are invalidated on save so the next draw re-pushes — the bridge captures the matching state on the C++ side via SkCanvas::save. |
| `translate` / `scale` / `rotate` / `setTransform` / `resetTransform` / `transform` (#964) | Forward to `canvasTranslate` / `canvasScale` / `canvasRotate` / `canvasSetTransform`. `transform` is best-effort: pure translation forwards to `canvasTranslate`; other matrices are silently dropped (the bridge has no concat primitive). `setTransform` accepts the (a,b,c,d,e,f) form and the single-DOMMatrix form. |
| `arc(cx,cy,r,a0,a1,ccw)` / `ellipse` (#964) | Approximated as cubic-Bezier segments (4-segment unit-circle scaling) so the path participates in `fill()` / `stroke()` / `clip()`. `arcTo` is a conservative two-segment lineTo approximation — sufficient for rounded marquee corners, not fidelity-critical. |
| `bezierCurveTo` / `quadraticCurveTo` / `rect` / `roundRect` (#964) | Forward to `canvasCubicTo` / `canvasQuadTo` / repeated `canvasLineTo`. `rect` emits an explicit closing lineTo back to the start so the resulting subpath is closed. `roundRect` honours the uniform-radius case; non-uniform `radii[]` falls back to `radii[0]`. |
| `clip(fillRule)` (#964) | Calls `canvasClip` (issue-896 path-based clip). The fill rule is dropped — Pulp's bridge currently ignores even-odd vs nonzero, matching SkCanvas defaults. |
| `fillText(text,x,y)` / `strokeText` (#964) | `fillText` syncs global / text state and forwards to `canvasFillText` with the active fillStyle's colour (or first gradient stop). `strokeText` falls back to `fillText` with the strokeStyle colour — Pulp's bridge has no stroke-text command. |
| `createLinearGradient` / `createRadialGradient` / `createConicGradient` (#964) | Return a `CanvasGradient` object with `_kind`, `_params`, `_stops`, and an `addColorStop(offset, color)` method. Gradients are NOT pushed to the bridge until they're assigned to `fillStyle` / `strokeStyle` AND a draw fires — `_applyFillStyle()` flushes via `canvasSetLinearGradient` / `canvasSetRadialGradient`. Conic gradients return an empty linear placeholder (Skia conic-gradient plumbing not yet wired). |
| `fillStyle` / `strokeStyle` (#964) | Plain fields. `_applyFillStyle()` runs before every fill draw and flushes either a string colour (via `canvasSetFillColor`) OR a gradient (via `canvasSetLinearGradient` / `canvasSetRadialGradient`), tracking `_activeFillKind` so a subsequent string assignment first calls `canvasClearGradient`. Stroke gradients fall back to the first colour stop (no stroke-gradient bridge today). |
| `globalAlpha` / `globalCompositeOperation` / `font` / `textAlign` / `textBaseline` / `lineCap` / `lineJoin` (#964) | Plain fields. Pushed to the bridge via `_syncGlobalState` / `_syncTextState` / `_syncLineState` lazy helpers — they only emit a `canvas*` call when the value differs from the last-sent cache, and the cache is invalidated on `save()` / `restore()`. |
| `createPattern` (#964) | Returns `null` per spec when patterns aren't available. Pulp's bridge has no pattern primitive yet; revisit when a plugin actually needs one. |

#### Why the shim must export every Canvas2D method (#964)

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
   cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
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
- `v8`: V8 on desktop/Android via the pinned sealed `libv8` (fetched into `external/v8-build/`). Build fails if not fetched; iOS errors out (no JIT). See "V8 provider library" below.

The engine choice is a **build-time** CMake option. Changing it requires reconfigure + rebuild. The abstraction ensures all JS bridge code works identically across engines — the switch is invisible to UI scripts.

## V8 provider library (how V8 is obtained)

Pulp does **not** build V8 from source, and (since 2026-06) does **not**
use a developer's Homebrew `libnode`. The provider is a **pinned, sealed
prebuilt `libv8`** from the
[danielraffel/v8-builder](https://github.com/danielraffel/v8-builder)
fork — the same pin/fetch/Find pattern as Skia:

1. **Pin:** the `V8` entry in `tools/deps/manifest.json`
   (`determinism.release_assets`, per-platform URL + sha256, tag `v8-15.1.27`).
2. **Fetch:** `python3 tools/scripts/fetch_v8_for_release.py <platform>`
   downloads + sha256-verifies + unpacks to `external/v8-build/<platform>/`
   (`include/` + `lib/`). Platforms: `darwin-arm64`, `darwin-x64`,
   `linux-x64`, `linux-arm64`, `windows-x64`, `windows-arm64`,
   `android-arm64`, `ios-simulator-arm64` (headers-only — iOS is JSC).
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

**Two behavior rules (Codex review, 2026-06):**
- `PULP_JS_ENGINE=auto` **never** pulls in V8 — V8 is strictly opt-in via
  `=v8`. (Previously `auto` + `V8_INCLUDE_DIR` silently enabled it.)
- `PULP_JS_ENGINE=v8` on **iOS** is a configure-time `FATAL_ERROR` — V8
  needs JIT, forbidden in iOS apps / AUv3 extensions. iOS uses JSC.

**Why the sealed build (the ICU caveat):** the v8-builder `libv8` exports
only the `v8::`/`cppgc::` API and keeps its bundled ICU/zlib/Abseil
internal, so they don't collide with Skia's bundled-but-flat-named
ICU/HarfBuzz at the final link. Confirm any provider is seal-safe with
`nm -gU <lib> | c++filt | grep -cE 'icu_[0-9]+::|absl::'` — only
`v8::internal::` functions whose *signatures* mention those types should
appear, never re-exported `icu_NN::`/`absl::` library symbols. V8 is still
confined to `js_v8_engine.cpp` and never shares a TU with a Skia/Dawn
header.

**Cross-platform rollout + ship/sign/package** (Windows MSVC `/MT` ABI,
macOS nested-dylib re-signing, Android ABI gating, golden-VM bake) are
tracked in `planning/2026-06-06-v8-sealed-libv8-provider-migration-plan.md`.
macOS arm64 is the proven lane; other platforms are pinned but pending
per-platform verification.

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
`FATAL_ERROR` since V8 needs JIT).

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

### Web-API global registration is hybrid native+JS by design (#915)

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
threejs-bridge skill for the full #3217 chronology.

### setTimeout(fn, 0) takes the microtask path, not the timer queue (#915)

`setTimeout(fn, 0)` deliberately bypasses `__scheduleTimer__` and routes through `Promise.resolve().then(...)` so it drains on the next `pump_message_loop()` call. This matches React's scheduler expectations and makes tests deterministic (no host frame loop needed). Positive-delay timeouts go through the native deadline tracker and only fire when `service_frame_callbacks()` runs.

If a consumer reports "my setTimeout(fn, 1) never fires", check that the host is actually calling `service_frame_callbacks()` from its frame loop — that's the drain hook for non-zero delays.

### `display: flex` defaults to `flex-direction: row` (CSS web-compat, #1147)

Pulp's underlying widgets default to `FlexDirection::column` (RN convention). The CSS web platform default for `display: flex` is `flex-direction: row` — children lay out horizontally. Imported / extracted designs assume the web default, so `web-compat-style-decl.js` explicitly emits `setFlex(id, 'direction', 'row')` whenever a `CSSStyleDeclaration` resolves `display: flex` and the consumer has NOT also declared `flexDirection`, `flex-direction`, or a `flexFlow` shorthand that includes a direction token.

Order independence is intentional. Both of these end up column:
```js
el.style.flexDirection = 'column'; el.style.display = 'flex';
el.style.display = 'flex'; el.style.flexDirection = 'column';
```
The setter trap stores into `_props` BEFORE `_applyProperty` runs, so the display handler can see a previously-declared direction and skip the row default. A later explicit `flexDirection` overrides the row default through the normal handler.

`flexFlow` is content-aware: `flexFlow: 'wrap'` does NOT block the row default (CSS shorthand semantics — omitted `flex-direction` defaults to row), but `flexFlow: 'column wrap'` does. The check uses a `\b(row|column)\b` regex against `_props.flexFlow`.

Not changed by this fix: `createCol` / `createRow` / `createPanel` C++ paths preserve their explicit direction; typed React props in `pulp-react/prop-applier.ts` route directly through bridge setters and don't touch `style`.

### CSS-shim gap fills — translator vs. bridge contract (#1434)

Three classes of "silent drop" recur in `web-compat-style-decl.js`. When
adding a CSS property, walk all three before declaring done:

1. **Missing `case "X":`** — the property is nowhere in the switch, so
   `el.style.X = ...` writes to `_props[X]` and never reaches the
   bridge. Harness verdict: NOT-IMPL. Examples: `backdropFilter`
   (pulp #1434 batch 3 — bridge `setBackdropFilter` was wired by
   pulp #1366, the JS route was the only missing piece).

2. **Coalesced shorthand only** — the shorthand routes (e.g.
   `textDecoration`) but the longhands (`textDecorationLine` /
   `-Color` / `-Style`) silently no-op. Per-attribute longhands MUST
   route to per-attribute bridge setters so a previously-set sibling
   isn't clobbered — the same pattern as the per-side border fix
   from PR #1166 finding #4. Don't try to coalesce three independent
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

This pattern landed for shadow* (#1434), miter / image-smoothing
(#1434 bridge-thin), and direction / filter (#1520). Copy the same
shape for the next canvas2d catalog setter.

### String-valued custom CSS properties (`var(--mono)` etc., #1899)

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

## ESM support per engine (iOS-D.3b 2026-05-29)

| Engine | Public ESM API | Status |
|---|---|---|
| **V8** (`libnode` on macOS, version-suffixed) | `import.meta`, dynamic `import()`, `setModuleLoaderDelegate`-equivalent | ✅ Full ESM. Used by `examples/threejs-native-demo/main.cpp:221` for macOS Three.js. |
| **JSC** (system framework on iOS) | NO public ESM module loader on iOS | ❌ `JSScript.h` + `JSModuleLoaderDelegate` are private. Shipping them in an `.appex` risks App Store rejection. |
| **JSC** (system framework on macOS) | `JSScript` exposed via framework headers | 🟡 Available, but typically unused since macOS uses V8. |
| **QuickJS** (vendored under `external/quickjs/`) | Full ESM via `JS_NewModule*` | ✅ Used as a fallback when V8 is unavailable + jitless V8 isn't an option. |
| **Hermes** | Limited (no full ES module support) | ❌ Not viable for Three.js. |

**iOS Three.js workaround**: Bundle the ESM source into a self-contained IIFE that registers exports on `globalThis.THREE`, then JSC `evaluate()` it. The bundler is at `tools/scripts/bundle_threejs_for_jsc.mjs`; the iOS NSBundle loader at `core/view/src/threejs_resources_apple.mm`; the .appex build-time wiring at `tools/cmake/PulpAuv3.cmake`. Pattern is general — any ESM library can be bundled this way for the JSC iOS lane.

**Bundler implementation note**: The bundler delegates to esbuild (pinned in `tools/scripts/package.json` at 0.25.10). The earlier regex-only pass could not resolve sibling `import { ... } from "./three.core.js"` statements that Three.js's webgpu entry depends on, which caused JSC parse errors at runtime ("expecting `(`"). esbuild handles ESM resolution + http: import stripping out of the box. The bundler auto-installs esbuild on first run via `npm install --prefix tools/scripts/` when `node_modules/esbuild` is missing — no manual setup needed.

See `planning/2026-05-29-ios-d3b-threejs-webgpu-program.md` for the full 6-slice bring-up and `.agents/skills/{ios,threejs-bridge}/SKILL.md` for the runtime contract.

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

### The web-compat preludes are engine-agnostic — a green V8/macOS test does NOT prove the JSC/iOS path (#3217)

The `web-compat-*.js` mocks (`web-compat-document-gpu-mock.js` etc.) are loaded identically under QuickJS, JSC, and V8 — so a *logic* bug in a mock behaves the same on every engine, but the surrounding native path may not. A WebGPU-mock `writeBuffer` bug (treating a TypedArray `dataOffset`/`size` as bytes instead of element counts) shipped undetected because the macOS V8 `[threejs][gpu][phase13]` smoke does a one-shot Skia **pixel readback** and renders green, while the live iOS AUv3 path renders black for an *unrelated* reason (`SkiaSurface::begin_frame()` silently falling back to the offscreen surface — see #3217). Two lessons: (1) when you change a `web-compat-*.js` mock, add a focused assertion of the exact code path you touched (the smoke's whole-array `writeBuffer` masked the explicit five-arg element-count form); (2) "green on the V8 headless lane" is necessary but not sufficient for the JSC/iOS live present path — that path has no CI coverage and must be verified on a device/simulator screenshot, never inferred from the readback test.
