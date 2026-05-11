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

## ⚠️ STALE-HEADER ABI MISMATCH — silent crash at first paint (2026-05-10)

**Searchable keywords**: silent crash, "Standalone: editor window open"
then exit, segfault inside `WidgetBridge::register_api()::$_NNN`,
`byte read Translation fault` at `x9=0` during Promise reaction, JS
`__dispatch__` from `pump_message_loop`, Spectr exits after CoreAudio
init, `pointer_registered_`, `wheel_registered_`, link-swap, install
prefix, `/tmp/pulp-sdk-gpu-latest`, `pulp upgrade --install`.

**The trap**: any PR that adds/removes/reorders **member variables**
on `WidgetBridge` (or any other class whose layout consumers compile
against) changes the C++ struct layout. If the **installed SDK
header** under the consumer's `Pulp_DIR` is OLDER than the
**installed SDK static library**, the consumer's translation units
compute member offsets from the old (smaller) layout while the lib's
own translation units (lambdas registered in `register_api()`) use the
new (larger) layout. Member access from a lambda points into the
wrong byte — typically NULL or garbage — and the first access
SIGSEGVs.

This presents as a "silent crash" because:
- macOS shows the editor window briefly
- The first paint frame logs `[gpu-host] first frame: …`
- Then a Promise reaction (React's commit phase) fires a registered
  C++ callback that does a member load → segfault → process dies
- The standalone parent (zsh / shell) reports nothing useful

The macOS crash report tells you everything: open
`~/Library/Logs/DiagnosticReports/<App>-<date>.ips`, look at thread 0's
faulting frame. If it's `WidgetBridge::register_api()::$_NNN + <offset>`
with `Exception Subtype: KERN_INVALID_ADDRESS at 0x…` and `x9=0` (or
some other register pointing into the bridge struct), you have an ABI
mismatch.

**Fix**:
```bash
# Re-install the header to the SDK consumer's install prefix
cp core/view/include/pulp/view/widget_bridge.hpp \
   "$PULP_SDK_INSTALL/include/pulp/view/widget_bridge.hpp"

# Wipe the consumer's stale .o files (they were compiled with old offsets)
rm -rf "$CONSUMER/build/CMakeFiles/<your-target>.dir"

# Rebuild the consumer from clean
cmake --build "$CONSUMER/build" --target <your-target> -j
```

When you `pulp upgrade --install` this is automatic because the SDK
release lays down headers + libs together from one build. The trap
only fires when you **link-swap a fresh `libpulp-view.a` into an SDK
install whose header is stale**, which is the failure mode for local
SDK-side iteration outside the `pulp upgrade` flow.

**Belt-and-suspenders mitigation** (TODO followup): consider adding a
build-time assertion in widget_bridge.hpp using `_Static_assert(sizeof(WidgetBridge) == EXPECTED, …)`,
where `EXPECTED` is generated at SDK release time from the actual
library build. A stale header would then fail to compile against the
fresh lib instead of segfaulting at first paint.

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

## Event-bridge dispatch payload contract — the @pulp/react surface

WidgetBridge talks to JS by calling `__dispatch__(id, eventName, ...rawArgs)`.
The `@pulp/react` synthetic-event shim (`packages/pulp-react/src/synthetic-event.ts`)
inflates those raw args into a React-DOM-compatible event. **Both sides
of this contract must move together or JSX handlers silently break.**

The shim's `makeSyntheticEvent` only lifts fields off the first arg when
`isPlainObject(rawArgs[0])` is true. Positional args (e.g. `wheel(dx, dy)`)
fall through to the empty default object and `e.deltaY` is `undefined`.
That class of regression is what cost us multiple PRs on the Spectr
band-drawing / trackpad-zoom / Escape-modal fixes.

### Required payload shapes

| Event family | Raw shape (object literal) | Why each field |
|---|---|---|
| `pointerdown` / `pointerup` / `pointercancel` | `{clientX, clientY, offsetX, offsetY, pointerId, pointerType, isPrimary, pressure, altitudeAngle, azimuthAngle, button (W3C: 0=left, 1=middle, 2=right), ctrlKey, shiftKey, altKey, metaKey}` | JSX reads `e.clientX - rect.left`, hit-tests by `e.button`, and gates UI on modifier booleans |
| `pointermove` | `{clientX, clientY, offsetX, offsetY, pointerId, pointerType, isPrimary}` | dragged from `on_drag(local pos)`; `clientX/Y` MUST be window-relative — walk parent-chain `bounds()` to compute |
| `wheel` | `{deltaX, deltaY, clientX, clientY}` **as an object, not positional args** | The synthetic-event shim's plain-object branch is the only one that lifts wheel deltas |
| `keydown` / `keyup` | `{key (W3C UIEvent.key string: 'Escape', 'ArrowLeft', 'F1', 'a', ' ', etc.), keyCode (raw int), ctrlKey, shiftKey, altKey, metaKey, mods}` | JSX compares `e.key === 'Escape'`; the raw int alone is unusable |
| `gesturestart` / `gesturechange` / `gestureend` | `{scale, rotation, clientX, clientY}` | matches Safari GestureEvent |
| `change` / `input` (text) | `rawArgs[0]` is the string value, not an object | the synthetic-event shim treats `typeof === 'string'` as a change event |

### Required JS preamble shape

In `widget_bridge.cpp` the `kJSPreamble` and `kWindowListenerShim`
strings MUST guarantee:

- `__dispatch__(id, eventName, ...args)` wraps every callback in
  `try/catch` and surfaces failures via `__dispatchError__` if defined.
  Otherwise a single throwing handler kills the rAF self-rescheduling
  loop and the whole animation pipeline dies until the next event
  restarts it.
- When `id === '__global__'`, fan the dispatch out to
  `window._listeners[eventName]` so `window.addEventListener('keydown',
  fn)` works without the full web-compat bundle.
- `window` exposes `addEventListener` / `removeEventListener` /
  `dispatchEvent` and `_listeners` via the minimal shim — install AFTER
  all preludes so `var window = {...}` in `web-compat-document.js` does
  not clobber the shim.

### Native-side registrars MUST be idempotent

`registerPointer(id)` / `registerWheel(id)` (and any future
`registerX(id)`) wrap the previous `on_pointer_event` lambda. If a React
re-render re-issues the registration, each call stacks a new wrapper —
N renders → N firings per event. Always gate the registration with a
per-id set (e.g. `pointer_registered_`, `wheel_registered_`) and
early-return on duplicates.

### macOS host-side bubbling

`core/view/platform/mac/window_host_mac.mm` is the dispatch source for
mouse / pointer / wheel on macOS. Every dispatcher MUST:

- Set `me.window_position = pt` for wheel events (clientX/Y derives from
  this). Without it JSX `e.clientX - rect.left` for anchor-frequency
  zoom gives the wrong anchor.
- Bubble `on_pointer_event` up the parent chain (W3C bubbling) so a
  wrap-div with `registerPointer` subscribed gets events from canvas
  children that win `hit_test`. The Spectr FilterBank band-drawer is
  this exact pattern.
- Leave `on_mouse_down` / `on_mouse_drag` / `on_mouse_up` deepest-wins
  (those are the JUCE-style click channel, not the W3C bubbling channel).

### Tests that pin the contract

`test/test_widget_bridge.cpp` — `[contract]` tag:

- "Event contract: W3C MouseEvent.button maps left=0, middle=1, right=2"
- "Event contract: forward_key_event emits W3C UIEvent.key strings + modifier booleans"
- "Event contract: window.addEventListener('keydown', fn) receives __global__ keydown"
- "Event contract: __dispatch__ try/catch keeps listeners alive after a handler throws"
- "Event contract: wheel dispatch is an object {deltaX,deltaY,clientX,clientY}"
- "Event contract: registerPointer/registerWheel are idempotent (no lambda-stack growth)"

Run with: `./build/test/pulp-test-widget-bridge "[contract]"`

When adding any new event family or fields, add a corresponding
`[contract]` case AND a row to the payload-shape table above.

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
