---
name: view-bridge
description: Editor lifecycle and multi-view attach for Pulp plugins — when to override Processor::create_view(), the open → notify_attached → resize → close protocol, release_view() ownership rules, and secondary-view roles.
---

# ViewBridge skill

**TL;DR.** Every Pulp plugin format adapter (VST3, AU v2, AU v3, CLAP,
Standalone) opens its editor through
`pulp::format::ViewBridge`. You only touch the bridge when you override
`Processor::create_view()` or write a new adapter. This skill captures
the invariants the API enforces and the pitfalls that bit us enough
times to be worth remembering.

## When to use ViewBridge

| Task | Touch ViewBridge? |
|---|---|
| Add a knob to the auto-generated editor | No — just `define_parameters` |
| Return a hand-built `view::View` tree from a processor | Yes — override `Processor::create_view()` |
| React when the host actually shows / resizes / closes the editor | Yes — override `on_view_opened / on_view_closed / on_view_resized` |
| Add a new format adapter | Yes — construct a `ViewBridge` and follow the lifecycle protocol |
| Hand the built view to an external container (TabPanel, WindowHost) | Yes — call `ViewBridge::release_view()` |
| Ship a paint-only overlay (inspector) | No — but `attach_secondary_view(…, ViewRole::Inspector)` is the primitive you'll eventually use |

## Lifecycle protocol — adapter author side

```
bridge.open(&err)                 // builds the view; does NOT fire on_view_opened
  |
  +-- host.attach_to_parent(window)  // platform-specific
  |
bridge.notify_attached()          // fires Processor::on_view_opened
  |
bridge.resize(w, h)               // on each host resize; fires on_view_resized
  |
bridge.close()                    // fires Processor::on_view_closed iff attached
```

**Do not** fire `on_view_opened` before the host has actually attached
the view to a native parent. That was PR #140's Codex P2 finding — if
attach fails after `open()`, a naive implementation would fire
`on_view_opened` then destroy without a matching `on_view_closed` and
the plugin would leak host-window-dependent resources. `ViewBridge`
guarantees balance **only when the adapter honors the two-step
protocol** (`open()` then `notify_attached()`).

### Per-format attach point

| Format | `open()` is called here | `notify_attached()` is called here |
|---|---|---|
| VST3 | `CPluginView::attached()` before `attach_to_parent` | After `CPluginView::attached` returns `kResultTrue` |
| CLAP | `gui_create` | Inside `gui_set_parent` on the matched window API |
| AU v2 | `uiViewForAudioUnit` — after fetching context via `kPulpEditorContextProperty` | After `PluginViewHost::create` succeeds |
| AU v3 | `viewDidLoad` | After `PluginViewHost::attach_to_parent` in `viewDidLoad` |
| Standalone | Before `WindowHost::create` | After `WindowHost::create` succeeds |

## `release_view()` — for containers that own the view

`TabPanel::add_tab` and similar widgets take `std::unique_ptr<view::View>`.
Call `bridge.release_view()` to hand ownership over. The bridge keeps a
raw pointer so `notify_attached`, `resize`, and `close` continue to
dispatch `Processor::on_view_*` on the same view instance.

**Contract:** the caller must keep the released view alive until
`bridge.close()` runs (or the bridge is destroyed). The standalone
adapter handles this by calling `bridge.close()` explicitly after
`run_event_loop()` returns, before the `TabPanel` falls out of scope.

Standalone also has an editor-only path now: set
`StandaloneConfig::show_settings_tab = false` and `run_with_editor()`
will host the released editor root directly in `WindowHost` instead of
wrapping it in the outer `Editor/Settings` `TabPanel`. The same
ownership rule still applies: close the bridge before the released
root view is destroyed.

Standalone now also keeps the bridge's size in sync with the real host
content area. `run_with_editor()` reads `WindowHost::get_content_size()`
immediately after `notify_attached()`, subtracts any standalone chrome
height, dispatches `ViewBridge::resize(...)`, and re-dispatches on each
`WindowHost::set_resize_callback(...)` event. If you embed a native child
or rely on `Processor::on_view_resized(...)`, do not assume
`editor_size()` is the last word after attach.

## AU v2 dual-Processor gotcha (fixed)

Pre-ViewBridge, the AU v2 Cocoa view factory called
`format::registered_factory()` and built a **second** `Processor` +
`StateStore` for the UI, syncing parameters one-shot at construction.
Any parameter change on the audio thread drifted from the UI.

Post-fix: `au_v2_adapter.{hpp,cpp}` exposes the host `Processor*` +
`StateStore*` via a private AU property `kPulpEditorContextProperty`
(`'PuEd'`). The view factory fetches it with `AudioUnitGetProperty`
and drives a `ViewBridge` against the host's single Processor.

AU v3 does the same thing via the ObjC accessors
`-[PulpAudioUnit pulpProcessor]` and `-[PulpAudioUnit pulpStore]`.

**If you add another AU-like adapter, expose the same pair.** Never
spin up a second Processor for the UI.

## Secondary views

```cpp
view::View* ViewBridge::attach_secondary_view(std::unique_ptr<view::View>, ViewRole);
bool        ViewBridge::detach_secondary_view(view::View*);
size_t      ViewBridge::view_count()                const;
view::View* ViewBridge::view_at(size_t index);
ViewRole    ViewBridge::role_at(size_t index)       const;  // Editor, Inspector, Remote
```

Roles are opaque — the bridge only uses them for introspection.
Parameter bindings propagate automatically because every attached view
polls the same `StateStore`; there is no explicit broadcast step.

Phase 4's `attach_remote_view(url)` (WebSocket-backed) will land as a
`ViewRole::Remote` secondary view.

## Common pitfalls

1. **Forgetting `notify_attached()` after a successful attach.** The
   host will show the view but `Processor::on_view_opened` never fires,
   so any lifecycle-tied setup (listener registration, meter startup)
   silently skips.
2. **Calling `close()` after destroying the released view.** Lifecycle
   dispatch then runs on a dangling pointer. Always close the bridge
   first, then let the caller-owned view destruct.
3. **Creating a second Processor in the editor path.** Fetch the host
   Processor via the format-specific accessor (AU v2 property, AU v3
   ObjC selector, CLAP / VST3 constructor param).
4. **Hard-coding `editor_size()` when you actually want resize
   bounds.** Override `view_size()` and return a `ViewSize` with
   real min/max; hosts use those to constrain user-drag resize.
5. **Forgetting that adapters must read `bridge.size_hints().min_*`
   when building the host's `WindowOptions`.** The bridge caches
   `Processor::view_size()` in `size_hints_`, but each adapter is
   responsible for forwarding `min_width`/`min_height` (and
   `preferred_*`) into its window/host options. The standalone
   adapter centralizes this in `detail::make_standalone_window_options`
   so the chrome-height shift is applied consistently (#1362). Other
   adapters wiring an OS window (e.g. host apps registering a
   `WindowHost::Factory`) need the same propagation; reading only
   `preferred_*` leaves the OS host with a zero minimum and lets
   plugins shrink below their declared floor.
6. **Idle-pump must drain timers + frames + async results — not just
   frames.** The platform host idle entry point (Mac CVDisplayLink,
   iOS CADisplayLink, Android AChoreographer) is the only thing that
   drives `WidgetBridge` per vsync when no input event fires. There
   are TWO bridge methods that drain different queues:
   - `poll_async_results()`: async-shell results (`execAsync` callbacks)
     + rAF callbacks (`__flushFrames__`). Does NOT pump message loop
     or drain timers.
   - `service_frame_callbacks()`: pumps engine message loop + drains
     native-tracked `setTimeout` / `setInterval` (`__flushTimers__`)
     + rAF callbacks. Does NOT drain async-shell results.

   Host idle paths must call BOTH (`poll_async_results()` first, then
   `service_frame_callbacks()`). Calling only the first drops timer
   callbacks on the floor — `setTimeout(fn, 100)` queues forever and
   only fires when an unrelated event happens to pump the message
   loop (pulp #1412, regression of PRs #1400/#1404/#1405).
   `ScriptedUiSession::poll()` does this pair on Mac/iOS standalone;
   `core/render/platform/android/gpu_surface_android.cpp::android_render_frame()`
   does the same pair on Android. Tests live under `[issue-1412]` in
   `test/test_widget_bridge.cpp`.

## Tests

`test/test_view_bridge.cpp` is the canonical reference:
- `"ViewBridge falls back to AutoUi when create_view returns nullptr"`
- `"ViewBridge honors custom create_view()"`
- `"ViewBridge supports secondary views"`
- `"ViewBridge defers on_view_opened until notify_attached"`
- `"ViewBridge close without attach does not fire on_view_closed"`
- `"ViewBridge destructor closes view"`
- `"ViewBridge cross-format lifecycle invariants"` (VST3 / CLAP / AU v2 /
  AU v3 / Standalone / failed-attach replay)

7 cases, 67 assertions. Run with
`ctest --test-dir build -R ViewBridge --output-on-failure`.

## Remote views (Phase 4)

`ViewBridge::attach_remote_channel(channel, label)` registers a
`RemoteViewSession` driving a `MessageChannel` (WebSocket or in-process
loopback) as a `ViewRole::Remote` secondary. The session speaks the
protocol in `docs/reference/remote-view-protocol.md`:

- `view.hello` + `view.metadata` handshake
- `view.param_set` / `view.param_changed` wire through `StateStore`
- `view.param_get` request/response
- `view.input` (notification)
- `view.close` (either side)

Tests: `test/test_remote_view.cpp` — 4 Catch2 cases / 23 assertions via
MemoryMessageChannel loopback.

### Attaching from an MCP server

An MCP server that runs alongside a Pulp plugin host can open a
`RemoteViewSession` to drive the plugin's view from Claude Code:

1. MCP server declares a tool (e.g. `view_attach`, `view_param_set`,
   `view_param_get`) backed by `pulp::runtime::WebSocketChannel::connect(...)`.
2. Tool handler calls `bridge->attach_remote_channel(std::move(ws), "mcp")`
   where `bridge` is the host's ViewBridge (same process) — or opens
   the socket *to* a separate Pulp host process that listens via
   `WebSocketChannel::accept`.
3. Subsequent MCP tool calls drive `RemoteViewSession::set_parameter`
   / `get_parameter` / `send_input`.

This is the pattern. A concrete MCP-tool wrapper bundled with Pulp is
a small follow-up on top of `tools/mcp/pulp_mcp.cpp`.

### Paint-op streaming

Not yet wired. Current MVP: the remote renderer is expected to mirror
its own view hierarchy informed by `view.metadata`. Canvas-command
streaming is the next increment — see the "Not yet wired" section of
the protocol doc.

### AU editor `dealloc` ordering — never call `bridge->close()` explicitly

When a `ViewBridge` and a `PluginViewHost` are owned by the same C++
scope (a struct, an Obj-C class's ivars), C++ destroys members in
REVERSE declaration order. AU's editor wrappers
(`PulpAUEditorOwnership` for AU v2 Cocoa view, `PulpAUViewController`
for AUv3 iOS) declare the bridge first and the host second so the
host (which holds `View& root_`) is torn down BEFORE the bridge that
owns the View. The host's destructor can then safely call
`root_.set_plugin_view_host(nullptr)` to clear the back-pointer; then
`~ViewBridge` fires `Processor::on_view_closed` and resets the View.

Calling `bridge->close()` explicitly inside dealloc reverses that
order — the View dies first, the host's `~PluginViewHost` then
dereferences a dangling `root_` reference, and AU editor close
crashes the host process. Codex P1 review on PR #653 caught the
crash; the fix was to remove the explicit close (PR #667 / pulp
`fix/au-editor-uaf-on-close`). Don't reintroduce it. The full
rationale lives in the `auv2`, `auv3`, and `ios` skills since those
files are dual-/triple-mapped.

## EditorBridge — JSON message dispatch over the editor lifecycle (pulp #709)

`pulp::format::ViewBridge` (this skill) handles **when** the editor
exists. `pulp::view::EditorBridge` handles **what messages flow
between it and the processor** while it does. Use both:

```cpp
class MyEditor : public pulp::view::View {
    void wire(pulp::view::WebViewPanel& panel) {
        bridge_.add_handler("set_value", [this](const auto& payload) {
            const auto v = pulp::view::EditorBridge::get_float(payload, "value", 0.0f);
            // ... apply to processor ...
            return pulp::view::EditorBridge::ok_response();
        });
        bridge_.attach_webview(panel);          // routes WebView postMessage → handlers
    }
    void unwire(pulp::view::WebViewPanel& panel) {
        bridge_.detach_webview(panel);          // clears the installed callback before teardown
    }
private:
    pulp::view::EditorBridge bridge_;
};
```

Key invariants:

- **Renderer-agnostic.** `attach_webview(WebViewPanel&)` today;
  `attach_native_runtime(JsRuntime&, "<handler_name>")` for the pulp
  #468 native-JS-runtime import lane. Same handler registrations.
- **Explicit WebView teardown.** `detach_webview(WebViewPanel&)`
  clears the callback installed by `attach_webview`. Use it when the
  bridge and `WebViewPanel` are side-by-side members and you want to
  sever queued WebView messages before native detach or panel
  destruction (#726).
- **noexcept dispatch.** `dispatch_json(...)` and `dispatch(...)` are
  `noexcept` and always return a well-formed JSON response envelope —
  handler exceptions become `{"ok":false,"error":"internal error"}`.
- **Standard envelope error vocabulary** (substring-compatible with
  Spectr's existing test suite — that compatibility is load-bearing
  for the Spectr cutover acceptance criterion in #709):
  - `malformed_json` — JSON parse failed / root not object
  - `unknown_type` — no handler registered
  - `missing_field` — envelope missing/empty/non-string `type`
  - `wrong_type` — handler-emitted via `err_response("...")`
  - `internal_error` — handler threw
- **Plugin-specific drag/edit state stays in the handler closure** —
  the framework explicitly does NOT carry `EditorBridgeState`-style
  per-session state. Capture it on `[this]` instead.

When you change `core/view/src/editor_bridge.cpp` or its header, the
skill-sync gate requires updates to either *this* skill or the
`import-design` skill (or both). The path map maps both to the file.

## References

- `core/format/include/pulp/format/view_bridge.hpp` — public API
- `core/format/src/view_bridge.cpp` — implementation
- `core/format/include/pulp/format/processor.hpp` — `create_view`,
  `view_size`, `on_view_*`
- `core/format/include/pulp/format/remote_view_session.hpp` — Phase 4
- `core/view/include/pulp/view/editor_bridge.hpp` — EditorBridge API (#709)
- `core/view/src/editor_bridge.cpp` — EditorBridge implementation
- `docs/guides/view-bridge.md` — user-facing guide
- `docs/reference/editor-bridge.md` — EditorBridge reference (#709)
- `docs/reference/remote-view-protocol.md` — Phase 4 wire format
- `examples/view-bridge-demo/main.cpp` — runnable headless demo
- `test/test_remote_view.cpp` — loopback tests for the remote protocol
- `test/test_editor_bridge.cpp` — EditorBridge unit tests (#709)
- `planning/next-features-plan.md` § Feature 1 — phase tracking
