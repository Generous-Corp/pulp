---
name: webview-ui
description: Build or iterate on a Pulp WebView UI using the native WebView bridge, embedded assets, directory-backed dev resources, and focused WebView validation.
---

# WebView UI

Use this skill when working on Pulp's optional WebView layer: HTML/JS panels,
Monaco-style tools, documentation views, or mixed native/WebView windows.

## Core Positioning

Pulp WebView is an opt-in compatibility layer, not the primary UI path.
Default to native Pulp UI unless the task truly benefits from existing web
content.

## Core Runtime Pieces

Main APIs:
- `pulp::view::WebViewPanel`
- `pulp::view::WebViewOptions`
- `pulp::view::WindowHost`
- `pulp::view::AssetManager`

Bridge contract:
- `window.pulp.postMessage(type, payload, id)`
- `window.pulp.on(type, callback)`
- native `set_message_handler(...)`
- native `post_message(...)`
- async `evaluate_js(..., callback)`

## Platform Truth

`PULP_BUILD_WEBVIEW=ON` is now the honest opt-in switch for the common Pulp
WebView layer on:

- macOS via WebKit
- Windows via CHOC's WebView2 path
- Ubuntu/Linux via CHOC's GTK/WebKitGTK path

Required platform notes:
- macOS: no extra package discovery beyond the system WebKit framework
- Windows: normal Visual Studio/CMake toolchain plus a WebView2-capable runtime
- Linux: `gtk+-3.0` and `webkit2gtk-4.1` available through `pkg-config`

Keep claims narrow:
- macOS has the deepest runtime proof today, including Monaco in a native host
- Windows and Ubuntu now have truthful opt-in configure/build/test proof plus
  `pulp-webview-palette` build proof
- do not describe that as full runtime parity unless the host-level proof is
  actually there

## File drag-and-drop (from `choc::ui::WebView`)

CHOC's WebView layer carries file drag-and-drop, exposed to page JS — no Pulp
C++ wiring needed, it rides the `choc/gui/choc_WebView.h` we already wrap in
`core/view/src/web_view.cpp`:

- **Drag a file OUT of the webview:** call `window.chocStartFileDrag(["/abs/path", ...])`
  from a `pointerdown`/`mousedown` handler.
- **Drop files INTO the webview:** listen for the `chocfilesdropped` event;
  `e.detail.paths` is the array of absolute paths.

Platform reality: **macOS (WKWebView) + Linux (WebKitGTK) — both directions.**
**Windows (WebView2) — inbound only** (outbound is a documented no-op: windowed
WebView2 can't start a drag from web content without moving to composition
hosting). The JS call exists on all platforms for parity.

⚠ **Interim source:** this lands via an interim fork pin of CHOC
(`danielraffel/choc` tag `pulp-webview-dnd-poc1`, carrying WebView drag-and-drop
support; Tracktion/choc pull request 64 is the upstream tracker).
When that support is available upstream, the pin reverts to
Tracktion/choc with no API change. The runnable reference lives in the choc repo
(`examples/webview_drag_and_drop.cpp`), not in Pulp.

## Choose A Loading Mode

1. Simple inline proof
- use `set_html(...)`
- append `make_webview_bridge_bootstrap_script()`
- good for the smallest lifecycle/message tests

1.5. Placeholder first paint
- set `WebViewOptions::initial_html` when the final page is dark-themed and the
  platform WebView would otherwise flash white before the real content loads
- keep it lightweight and self-contained so it paints immediately
- combine with `transparent_background = true` when the final page also wants a
  transparent/native host background

2. Embedded bundled assets
- use `pulp_add_binary_data(...)`
- register generated arrays with `AssetManager`
- serve through `make_webview_embedded_resource_fetcher(...)`
- this is the truthful offline/distributable path

3. Directory-backed dev assets
- use `make_webview_directory_resource_fetcher(...)`
- use this for local framework payloads that already exist on disk
- good for Monaco / richer web tooling during development

## Monaco Guidance

Do not chase Monaco's deprecated AMD path.

Use a prebundled ESM workflow:
- bundle one app entry
- bundle the worker files
- point `MonacoEnvironment.getWorkerUrl(...)` or `getWorker(...)` at those generated worker files
- expose `window.monaco` only if the page wants that global convenience

Treat raw Monaco source or raw ESM output as build inputs, not as the final
browser payload. Worker and CSS handling belongs to the bundler.

Current proven local recipe:

```bash
cd /Users/danielraffel/Code/monaco-editor
npm install
npm run build-monaco-editor

cd /path/to/pulp
node examples/webview-monaco/build_monaco_bundle.mjs \
  --monaco-root /Users/danielraffel/Code/monaco-editor \
  --out-dir /path/to/pulp/build/examples/webview-monaco/dist

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_WEBVIEW=ON \
  -DPULP_MONACO_BUNDLE_DIR=/path/to/pulp/build/examples/webview-monaco/dist
cmake --build build --target pulp-webview-monaco -j8
./build/examples/webview-monaco/pulp-webview-monaco
```

Important notes from the working proof:
- use the local build script in `examples/webview-monaco/build_monaco_bundle.mjs`
- prefer `editor.main.js` so Monaco's real CSS/features come through
- worker URLs must point at the built `vs/languages/features/...` layout
- the current Monaco build imports `@vscode/monaco-lsp-client`; for the Monaco example we stub that at bundle time because the example is not doing LSP
- browser preview is useful for cheap layout proof, but `bridge unavailable` is expected there because only the native host injects `window.pulp`
- the native proof should reach `editor ready`

## Window Embedding

WebView backend availability and host embedding availability are separate. A
`WebViewPanel` can exist even when the surrounding `WindowHost` or
`PluginViewHost` cannot accept a native child surface. Always check:
- the panel's `native_handle()`
- the host's native content/view handle where applicable
- the boolean returned by `attach_native_child_view(...)`

Current host truth:
- standalone `WindowHost` child embedding is built in on macOS
- built-in iOS `WindowHost` does not expose standalone child-view embedding
  handles yet
- `PluginViewHost` child embedding is built in on macOS and iOS
- Windows/Linux/Android embedding is factory-backed through
  `WindowHost::Factory` or `PluginViewHost::Factory`

For palette / inspector style UI:
- create the parent `WindowHost`
- create the child/palette `WindowHost`
- use `attach_native_child_view(...)`
- resize with `set_native_child_view_bounds(...)`
- detach on close

For standalone native-child embeds, do not size from startup constants once
the host is live. Read `WindowHost::get_content_size()` for the current
content bounds, attach the child with that size, and keep it in sync via
`WindowHost::set_resize_callback(...)`. `examples/webview-monaco/main.cpp`
is the current reference pattern.

For plugin-editor embedding, use the `view::NativeViewHost` widget — do NOT
hand-roll `plugin_view_host()->attach_native_child_view(...)`:
- add a `NativeViewHost` to the editor tree, size it with flex
  (`host->flex().flex_grow = 1.0f` to fill), and call
  `set_native_child(panel->native_handle(), snapshot_fn)`
- the widget drives attach / bounds / clip / detach itself from its host
  back-reference + Yoga layout, so it works under BOTH a `PluginViewHost`
  (editor) and a `WindowHost` (standalone). It falls back from
  `plugin_view_host()` to `window_host()`
  (`core/view/src/native_view_host.cpp` `try_attach()` / `active_viewport_transform()`)
- it also honors the active design-viewport transform and routes headless
  snapshot capture for free — no `on_view_opened/resized/closed` plumbing
- see `examples/webview-plugin/` for the reference Processor-backed editor

### Native-editor gotchas (G2, learned building `examples/webview-plugin`)

- **Retention: own the `WebViewPanel` on the `Processor`, never in the view
  tree.** `ViewBridge::close()` destroys the whole view tree on editor close
  (`core/format/src/view_bridge.cpp`). A view-owned `WebViewPanel` is recreated
  every close/reopen, discarding the WKWebView's JS heap + page/scroll state. Put
  the panel on the `Processor` (create once, lazily in `create_view()`); the
  `NativeViewHost` only BORROWS `native_handle()`. Rule of thumb: *anything that
  must survive editor close lives on the `Processor`.* Pinned by a pointer-identity
  test across two open/close cycles in `test/test_webview_plugin_example.cpp`.
- **The old `plugin_view_host()`-only attach SILENTLY no-ops in standalone.** In
  standalone the root is hosted by a `WindowHost`, not a `PluginViewHost`
  (`core/format/src/standalone.cpp`), so `plugin_view_host()` is null and a
  hand-rolled attach returns without attaching — the WebView never appears and
  nothing errors. `NativeViewHost`'s `window_host()` fallback is the fix.
- **WKWebView frame scaling REFLOWS, it does not zoom.** CSS px track the frame,
  so a letterbox-scaled native child does not visually match a scaled Pulp
  sibling. `WKWebView.pageZoom = scale` parity is deliberately DEFERRED (not a
  native-editor blocker).
- **Headless `snapshot_png()` returns ZERO bytes even when `is_ready()==true`** —
  WKWebView's `takeSnapshot` needs an on-screen window with painted content.
  Example/headless tests therefore assert the snapshot ROUTE by equality
  (`NativeViewHost::capture_native_overlay_png == panel->snapshot_png()`), not a
  literal non-empty size; the non-empty forwarding guarantee is pinned at the
  widget level in `test/test_native_view_host.cpp`.

For a standalone app that should behave like a single-pane WebView shell:
- use `pulp::format::StandaloneApp::run_with_editor(...)`
- set `StandaloneConfig::show_settings_tab = false` to skip the outer
  standalone `Editor/Settings` chrome entirely
- `examples/webview-plugin/main.cpp` is the current kiosk-style proof
- if the page is dark on first launch, seed `WebViewOptions::initial_html`
  with a dark placeholder so the host opens into the final visual family
- if you want the standalone bundle to ship a custom app icon, keep that in
  the example `CMakeLists.txt` via `pulp_app_icon(...)`; it stays optional and
  should not be wired through the runtime WebView code itself

## Validation Loop

For focused local proof:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_WEBVIEW=ON
cmake --build build --target pulp-test-webview -j8
./build/test/pulp-test-webview --reporter compact
```

If example code changed, also build the relevant example target:

```bash
cmake --build build --target pulp-webview-palette -j8
```

Current proof expectations:
- encode/decode helpers pass
- bridge helpers pass
- resource fetchers pass
- example builds
- Windows/Ubuntu opt-in builds can configure and run the focused test slice
- live callback-ready cases may skip in headless or limited environments

## Screenshot / Preview Workflow

For quick visual inspection, keep a static browser preview page beside the
runtime assets when useful. That gives you a cheap screenshot path without
pretending the browser preview replaces native validation.

For Monaco specifically:
- browser-served proof page is useful to confirm bundling, workers, and CSS
- native-host proof is still required before claiming the bridge is working

### Headless capture of a native WebView overlay

A WebView is an OS-composited native child view (`attach_native_child_view`); it
is NOT painted into the Pulp Skia canvas, so a plain headless capture
(`render_to_png` / a host back-buffer read) of an editor that hosts one sees a
blank where the WebView is. To make a WebView editor self-verifiable headlessly,
the **owning Pulp `View` wrapper** must opt in:

1. call `set_contains_native_overlay(true)` when it attaches the WebView, and
2. override `capture_native_overlay_png()` to forward `WebViewPanel::snapshot_png()`
   (macOS: WKWebView `takeSnapshot`, no screen-recording permission needed).

The smart capture path (`pulp::view::capture_view` / `pulp-screenshot --backend
auto` / standalone `--screenshot`) then returns the in-process WebView snapshot
instead of silently rastering a blank, and **refuses with a reason** only when no
snapshot is available. Without the flag+override, `auto` captures a blank overlay
area silently. See `examples/webview-plugin` `WebViewEditorPane` for the pattern
and the `screenshot` skill for the dispatch contract.

## Canvas2D bridge gotchas (when the editor is on the @pulp/react native path, NOT WebView)

When porting a WebView editor to the native bridge (translating browser `<canvas>` + Canvas2D code to pulp's `canvas*` bridge globals via a JS shim like Spectr's `canvas2d-shim.ts`), several browser idioms silently break. Authoritative reference is the **`import-design` skill's "Canvas2D Bridge Gotchas" section** — see that for the full rule set with example code. Top-of-mind summary:

1. **`ctx.arc()` does not add to a path** — bridge `canvasArc` strokes immediately. Synthesize as ~32 `canvasLineTo` segments so `ctx.fill()` honors the active gradient. Same applies to `arcTo`, `ellipse`, `roundRect`.
2. **Gradient stops must be spread as variadic `(color, pos)` pairs** — passing JSON makes the bridge parse zero stops → silent fall-through to default white fill. `canvasSetLinearGradient(id, x0, y0, x1, y1, c1, p1, c2, p2, ...)`.
3. **Radial gradient bridge takes single outer circle** `(cx, cy, R)`, not the spec's two-circle form. Map `(x0,y0,r0,x1,y1,r1)` → `(x1, y1, r1)` (visually identical when `r0=0`, same center).
4. **Per-canvas `save_layer` isolation** — since pulp v0.74.1, `ctx.clearRect()` on one canvas no longer erases pixels another sibling just painted. Pin SDK `>= 0.74.1` for any multi-canvas editor.
5. **SDK version floor for canvas2d parity** is v0.74.1 (sibling isolation) on top of v0.72.4 (gradient bridge) + v0.72.5 (gradient fills). Anything older has visible gaps.
6. **Always pixel-sample** after rendering — uniform-fallback bugs (every gradient stop resolved to default white) look superficially "filled" at thumbnail scale but are structurally broken.
