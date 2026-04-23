# WebView Guide

Pulp's WebView support is an optional compatibility layer for embedded HTML/JS content such as Monaco, documentation panels, or existing web-based tools. It is not the primary UI path. New plugin UIs should still prefer Pulp's native view/canvas/render stack.

## Scope

Current WebView support is built on `choc::ui::WebView` and stays opt-in behind `PULP_BUILD_WEBVIEW`.

What it supports now:
- native OS WebView backends through CHOC
- direct HTML loading with `set_html()`
- URL navigation with `navigate()`
- async JS evaluation with callback results
- readiness queries with `is_ready()`
- structured native/web JSON messaging
- embedded bundled HTML/CSS/JS/assets through either:
  - offline `data:` URI rewriting helpers
  - CHOC `fetchResource` + custom scheme serving
- debug-mode / inspector toggles where the native backend supports them

What it is not:
- a Chromium/CEF/Electron stack
- the default plugin UI model
- a promoted replacement for Pulp's native GPU-rendered UI system

## Current Platform Truth

`PULP_BUILD_WEBVIEW=ON` is now the truthful opt-in switch for the common
WebView layer across the platforms we have focused proof for:

- macOS: system WebKit backend, focused native runtime proof, palette example,
  Monaco example, and `pulp-test-webview`
- Windows: CHOC WebView2 backend, focused configure/build/test proof, and
  `pulp-webview-palette` build proof
- Ubuntu/Linux: CHOC GTK/WebKitGTK backend, focused configure/build/test proof,
  and `pulp-webview-palette` build proof

This is enough to claim native backend availability behind
`PULP_BUILD_WEBVIEW=ON` on those hosts when the required dependencies are
installed. It is not yet a claim that every higher-level WebView workflow has
identical runtime polish on all three platforms.

## Build

Enable the optional WebView layer:

```bash
cmake -S . -B build -DPULP_BUILD_WEBVIEW=ON
cmake --build build -j8
```

Platform requirements:
- macOS: system `WebKit.framework`
- Windows: CHOC's WebView2 path plus the normal Visual Studio/CMake toolchain
- Linux: `gtk+-3.0` and `webkit2gtk-4.1` discoverable through `pkg-config`

The focused validation target is:

```bash
cmake --build build --target pulp-test-webview -j8
./build/test/pulp-test-webview --reporter compact
```

If example code changed, also build the palette example:

```bash
cmake --build build --target pulp-webview-palette -j8
```

## Core API

Main entry points live in `core/view/include/pulp/view/web_view.hpp`.

Important pieces:
- `WebViewPanel::create(options)`
- `is_ready()`
- `set_ready_handler(...)`
- `navigate(url)`
- `set_html(html)`
- `evaluate_js(js, callback)`
- `set_message_handler(...)`
- `post_message(...)`
- `make_webview_bridge_bootstrap_script()`

For native host integration, `WindowHost` now exposes:
- `native_window_handle()`
- `native_content_view_handle()`
- `attach_native_child_view(...)`
- `set_native_child_view_bounds(...)`
- `detach_native_child_view(...)`

That seam is the current bridge from Pulp's multi-window host layer into an
embedded native WebView. It is intentionally small: enough to prove palette /
inspector style embedding without pretending that Phase 7 is already a full
window-docking system.

## Simple HTML

For a simple embedded page:

```cpp
using namespace pulp::view;

auto panel = WebViewPanel::create();
panel->set_message_handler([](const WebViewMessage& message) -> std::string {
    return R"({"ok":true})";
});

panel->set_html(R"HTML(
<!doctype html>
<html>
<body>
<script>
)HTML" + make_webview_bridge_bootstrap_script() + R"HTML(
window.pulp.on('ready', function(message) {
  console.log(message.payload);
});
</script>
</body>
</html>
)HTML");
```

If the backend becomes ready asynchronously, install a ready handler instead of
guessing about timing:

```cpp
panel->set_ready_handler([panel = panel.get()] {
    panel->navigate("pulp://app");
});
```

For dark-themed editors that would otherwise flash white before the real page
loads, provide a lightweight placeholder page up front:

```cpp
WebViewOptions options;
options.initial_html =
    "<!doctype html><html><body style=\"margin:0;background:#0f172a;\"></body></html>";

auto panel = WebViewPanel::create(options);
panel->set_ready_handler([panel = panel.get()] {
    panel->set_html(real_editor_html);
});
```

`initial_html` is opt-in and backward-compatible: callers that leave it empty
keep the previous white-first-paint behavior.

## Offline Bundled Assets

Pulp already ships a CMake helper for embedded web assets:

```cmake
pulp_add_binary_data(MyWebAssets
    SOURCES
        web/index.html
        web/app.js
        web/styles.css
    NAMESPACE my_web_assets
)

target_link_libraries(my-app PRIVATE MyWebAssets)
```

At startup, register those generated arrays with `AssetManager`:

```cpp
#include "MyWebAssets_data.hpp"

auto& assets = AssetManager::instance();
assets.register_embedded("app_html", my_web_assets::index_html, my_web_assets::index_html_size);
assets.register_embedded("app_js", my_web_assets::app_js, my_web_assets::app_js_size);
assets.register_embedded("app_css", my_web_assets::styles_css, my_web_assets::styles_css_size);
```

If you already have embedded assets registered through `AssetManager`, you can inline a simple page for offline loading:

```cpp
const auto html = make_webview_offline_html_from_embedded(
    "page_html",
    {
        { "app.js", "page_js", "" },
        { "styles.css", "page_css", "" },
    });

panel->set_html(html);
```

This rewrites simple `src=` and `href=` references to `data:` URIs. JSON `data:` URIs use `application/json;charset=utf-8`.

## Framework-Style Resource Serving

For richer pages that should keep their own relative resource structure, use CHOC's native resource-serving path through `WebViewOptions`:

```cpp
WebViewOptions options;
options.enable_debug = true;
options.enable_debug_inspector = true;
options.fetch_resource = make_webview_embedded_resource_fetcher(
    "index_html",
    {
        { "app.js", "app_js", "" },
        { "styles.css", "styles_css", "" },
        { "assets/config.json", "config_json", "" },
    });
options.custom_scheme_uri = "pulp://app";

auto panel = WebViewPanel::create(options);
panel->navigate("");
```

Behavior:
- `"/"` or `"index.html"` serves the embedded HTML root
- relative assets are served from embedded resources
- the bridge bootstrap is injected as an init script so the same `window.pulp` contract is available on served pages
- `is_ready()` lets callers wait until the native backend can safely navigate/evaluate JavaScript

For development-only assets that live on disk instead of in the final bundle, use `make_webview_directory_resource_fetcher(...)`. This is the honest path for richer tools such as a locally built Monaco distribution when you want a truthful native-WebView proof without pretending Pulp vendors Monaco itself.

## Monaco / Rich Web Tools

For Monaco-class tooling, treat the browser assets as a prebundled payload.
The truthful Phase 7 model is:

- bundle Monaco's ESM entrypoint and worker files with a supported bundler
  (webpack / parcel / vite / esbuild)
- serve the generated files through either:
  - `make_webview_directory_resource_fetcher(...)` for local development proof
  - `pulp_add_binary_data(...)` + `AssetManager` for a fully embedded distributable bundle
- keep the native side simple: `WebViewPanel`, `window.pulp` messaging, and a
  custom scheme such as `pulp://editor`

Do not assume raw Monaco source files are directly browser-usable inside Pulp.
Upstream has deprecated the older AMD path, and the raw ESM tree still expects
bundler handling for workers and CSS imports. The supported route is to
prebundle and then load the resulting app/worker assets inside the native
WebView host.

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

Notes from the working proof:
- the local bundle script uses `editor.main.js` so Monaco's real CSS and editor features come through
- worker URLs target the built `vs/languages/features/...` tree, not the older docs/examples layout
- the current Monaco build imports `@vscode/monaco-lsp-client`; the Phase 7 example stubs that import at bundle time because the example does not use LSP
- browser preview is valid for layout/bundle proof, but `bridge unavailable` there is expected
- native host proof is the real bridge proof

The strongest native proof so far is still macOS, because that is where we
captured the Monaco-in-host runtime screenshot and editor-ready bridge state.
Windows and Ubuntu now have truthful opt-in backend build/test proof on the
same Phase 7 API surface, but not the same depth of rich-tool runtime coverage
yet.

## Messaging Model

Pulp uses a small structured JSON envelope:

```json
{
  "type": "theme.update",
  "payload": { "mode": "dark" },
  "id": "msg-1"
}
```

Helpers:
- `encode_webview_message_json(...)`
- `decode_webview_message_json(...)`
- `window.pulp.postMessage(type, payload, id)`
- `window.pulp.on(type, callback)`

This is intentionally simpler than a full RPC layer.

## Current Validation Shape

The WebView test target currently proves:
- message envelope encode/decode
- bridge bootstrap shape
- async eval wrapper shape
- offline asset rewriting helpers
- embedded resource fetcher behavior
- directory-backed resource fetcher behavior
- a live HTML page bridge round-trip when the environment makes the callback path available
- host attach / resize / detach through `WindowHost` when the platform exposes
  native child-view embedding handles
- a small floating `pulp-webview-palette` example builds against the same host
  seam and real `pulp_add_binary_data` bundled-resource path
- exact-SHA opt-in configure/build/test proof on Windows and Ubuntu with
  `PULP_BUILD_WEBVIEW=ON`
- `pulp-webview-palette` build proof on both Windows and Ubuntu

On some local/headless environments, the live callback readiness probe may skip rather than fail. That is treated as an environment limitation, not a blanket pass for the runtime.

That is the current bar for claiming cross-platform backend availability. Do
not overstate this as full runtime parity until the platform-specific host
polish work is actually landed.

## Future Skill

Pulp now ships its own `webview-ui` skill for the current Phase 7 workflow. The
useful pieces pulled forward from the iPlug2 audit are:
- framework-selection guidance
- resource-loading and bundling guidance
- bridge/messaging guidance
- devtools and hot-reload workflow guidance
