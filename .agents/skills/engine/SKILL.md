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

When adding new framework support, check the gap matrix in pulp #468
before assuming a polyfill is missing — the entry above is exhaustive
for React 18 dev. Add new files to `core/view/CMakeLists.txt`'s
`PULP_JS_PRELUDES` list AND to the `eval_or_throw` block in
`core/view/src/widget_bridge.cpp` (`embed_js.cmake` only embeds the
constants; the bridge constructor evaluates them).

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

- `auto` (default): QuickJS everywhere. Backward compatible. Safe.
- `quickjs`: Explicit QuickJS. Same as auto today.
- `jsc`: JavaScriptCore on Apple. Build fails on non-Apple.
- `v8`: V8 on desktop. Requires V8 headers/libs. Build fails without them.

The engine choice is a **build-time** CMake option. Changing it requires reconfigure + rebuild. The abstraction ensures all JS bridge code works identically across engines — the switch is invisible to UI scripts.

## Gotchas

### Web-API global registration is hybrid native+JS by design (#915)

CHOC's `NativeFunction` signature can only carry `choc::value::Value` arguments — JS function values don't round-trip through it. So even though `requestAnimationFrame` / `setTimeout` / `setInterval` look like they "should" be C++-only bindings, the callbacks themselves have to live in a JS-side registry (`__frameCallbacks__`, `__timerCallbacks__`).

What is C++-side: id allocation in `WidgetBridge::__scheduleTimer__`, deadline tracking in `pending_timers_`, and the flush driver invoked from `service_frame_callbacks()`. What is JS-side: the registry table and the `setTimeout` / `requestAnimationFrame` global wrappers (in `web-compat-scheduler.js`) that allocate ids, stash callbacks, and call into the natives.

Don't refactor this into a "pure native" shape — there's no way to do it without copying the entire JS engine's value type into Pulp's bridge layer.

### setTimeout(fn, 0) takes the microtask path, not the timer queue (#915)

`setTimeout(fn, 0)` deliberately bypasses `__scheduleTimer__` and routes through `Promise.resolve().then(...)` so it drains on the next `pump_message_loop()` call. This matches React's scheduler expectations and makes tests deterministic (no host frame loop needed). Positive-delay timeouts go through the native deadline tracker and only fire when `service_frame_callbacks()` runs.

If a consumer reports "my setTimeout(fn, 1) never fires", check that the host is actually calling `service_frame_callbacks()` from its frame loop — that's the drain hook for non-zero delays.
