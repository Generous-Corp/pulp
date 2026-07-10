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

`Processor`'s editor hooks (`create_view`, `view_size`, `on_view_*`) are
part of the node ABI surface. When adding a new view lifecycle hook, append
the virtual at the tail of `Processor`; never insert, remove, reorder, or
re-signature existing virtuals. `tools/scripts/node_abi_gate.py --mode=report`
is the fast check before pushing. The same rule applies to non-view Processor
virtuals: additive callbacks such as `process_f64(...)` belong after the
existing f32 process surface, and descriptor fields added for new capabilities
should be appended when positional aggregate initializer compatibility matters.

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
the view to a native parent. If attach fails after `open()`, a naive
implementation would fire
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

`run_with_editor()` also opts the host into the platform's native
file-dialog backend by calling `platform::FileDialog::install_native_backend()`
once the processor's editor is confirmed. This is a no-op on macOS (a
native impl is compiled in) and on platforms with no built-in backend
(iOS/Windows/Android), but on Linux it installs the xdg-desktop-portal
FileChooser bridge (`core/platform/.../file_dialog_portal_linux.cpp`,
talking to the portal over the runtime-dlopen `pulp::platform::DBus`
client) so editor file pickers work natively. We install here — not via a
static initializer — because raising a portal dialog blocks, so the
default "no backend → no selection" contract must hold for headless/test
callers until a host explicitly opts in.

Standalone now also keeps the bridge's size in sync with the real host
content area. `run_with_editor()` reads `WindowHost::get_content_size()`
immediately after `notify_attached()`, subtracts any standalone chrome
height, dispatches `ViewBridge::resize(...)`, and re-dispatches on each
`WindowHost::set_resize_callback(...)` event. If you embed a native child
or rely on `Processor::on_view_resized(...)`, do not assume
`editor_size()` is the last word after attach.

Standalone resizes the host window per active settings-chrome tab. The
Audio/MIDI `SettingsPanel` needs far more height than a fixed-size editor
(query `SettingsPanel::preferred_height()` — header + inner Audio/MIDI tab
bar + every Audio-tab row at full size), so a 372px-tall editor window
squishes the device dropdowns to slivers. `run_with_editor()` wires the
outer card-stack `TabPanel::on_tab_change` to drive the SAME design-viewport
path the keyboard resize uses — `set_design_viewport` /
`set_fixed_aspect_ratio` / `request_content_size` to `(editor_w, editor_h)`
on the Editor tab and `(editor_w, SettingsPanel::preferred_height())` on the
Settings tab. The editor tab keeps its exact declared size (no letterbox);
the Settings tab reflows to fill its taller window. Guarded on
`chrome.tab_panel()` so the editor-only chrome (no settings tab) is untouched.

Standalone headless/test launches must not show or activate a native
window. `StandaloneConfig::headless`, `PULP_HEADLESS`, `PULP_TEST_MODE`,
`CI`, or `PULP_SCREENSHOT` route `run_with_editor()` through
`WindowOptions::initially_hidden`; screenshot mode captures
`WindowHost::capture_back_buffer_png()`, not compositor-visible
`capture_png()`. If a CI/test run is headless but has no screenshot path,
`run_with_editor()` fails before creating the host so tests cannot park a
hidden live window forever.

The `--audio-probe-json` / `PULP_AUDIO_PROBE_JSON` path (gated by
`PULP_ENABLE_AUDIO_PROBES`) is a SECOND headless one-shot that reuses the same
`screenshot_frame_delay` counter (`detail::DelayedAction`): after the
delay it writes `output_probe().latest()` as JSON via the pure
`audio::audio_probe_snapshot_to_json()` helper, then closes the host. When a
screenshot is ALSO requested the JSON write rides the screenshot `capture_fn`
(same frame, before the window closes); when only the JSON dump is requested it
drives a dedicated `DelayedAction` so no fake PNG bytes or cleared screenshot
path are involved. So a headless run without a screenshot path is valid as long
as `audio_probe_json_path` is set — don't tighten the "headless requires
screenshot" guard to also reject it.

## AU v3 controller lifecycle — runs on XPC queue, NOT main (Phase 3.5)

The host invokes `-[PulpAUMacViewController createAudioUnitWithComponentDescription:error:]`
(macOS) and `-[PulpAUViewController ...]` (iOS) on
`com.apple.NSXPCConnection.user.endpoint` — the XPC connection's
serial queue, not the main thread. Any AppKit / UIKit call from there
(`setPreferredContentSize:`, `self.view`, the `PluginViewHost`
attach) throws `NSInternalInconsistencyException` and kills the
.appex process. Host reports "Failed to load Audio Unit"; auval
silently passes because it doesn't exercise the controller path.

`au_view_controller_mac.mm` + `au_view_controller_ios.mm` apply a
HARD GUARD at the top of `rebuildEditorIfReady` that bounces the body
to `dispatch_get_main_queue()` if invoked off-main. **Don't guard
only at `setAudioUnit:`** — the compiler can inline the property
setter when `createAudioUnit` assigns to `self.audioUnit`, bypassing
the check. The guard must live in `rebuildEditorIfReady`.

Full recipe (macOS framework + stub .appex + container .app, signing,
notarization, PlugInKit diagnostics) is in
`.agents/skills/auv3/SKILL.md → "macOS AU v3 packaging"`.

### AU v3 teardown must ALSO run on the main thread

The same off-main hazard applies to `-dealloc`. A GPU-backed
`PluginViewHost` owns a `CVDisplayLink` whose idle pump
(`make_scripted_idle_pump`) is `dispatch_async`'d to the **main queue**
and dereferences the `ViewBridge`. If the last controller release lands
on the XPC connection queue (it can — `createAudioUnit…` and the rebuild
bounce arrive off-main), destroying the host + bridge off-main races a
main-queue idle block already past its `alive` liveness check → the pump
touches a freed bridge (SIGSEGV in `display_link_callback`). This is the
same use-after-free the AU v2 Cocoa view fixed by serializing teardown.
Both AU v3 `dealloc`s (`au_view_controller_mac.mm`,
`au_view_controller_ios.mm`) reset `_viewHost` on the main thread first
(`[NSThread isMainThread] ? reset : dispatch_sync(main, reset)`) — that
flips the host's liveness token and stops the display link before any
freeing — then let the remaining ivars destroy in the documented
reverse-declaration order. **Don't** remove the main-thread reset, and
**don't** instead try to clear `idle_callback_` from the off-main
`dealloc`: writing the `std::function` while the main-queue block is
calling it is just a different data race. Only main-thread serialization
closes it. The GPU host's `display_link_callback` block additionally
copies the idle callback locally and **re-checks `alive` after** running
idle (a scripted `poll()` can reentrantly close the editor and free
`self`) — parity with the CPU host's `render_link_callback`.

### macOS AU v3: editor attach is now DEFERRED until a settled host size

`rebuildEditorIfReady` no longer creates the `PluginViewHost` (or calls
`ViewBridge::notify_attached()`) inline. Logic hosts AU v3 out-of-process and
does **not** deliver its restored window size to the extension's view on initial
open, so a `PluginViewHost` built then would paint its first frame at the design
size inside Logic's smaller window (clipped). Instead `rebuildEditorIfReady`
opens the bridge, sets `preferredContentSize`, wires a `setFrameSize:` hook on a
custom `PulpAUMacRootView` (which fills its superview the **frame-based** way —
`autoresizingMask`, NOT Auto Layout: constraint pinning crashed Ableton Live, see
the auv3 SKILL), and marks the host
**pending**; `-createViewHostIfReady` then builds the host **at the view's real
bounds** — and only there fires `notify_attached()`. **Consequence for adapter
authors:** on macOS AU v3, `Processor::on_view_opened` (via `notify_attached`)
fires slightly later — after the host's first real layout, not at controller
build — and `_pendingRoot`/`_viewHostPending` gate the one-shot creation. Don't
move `notify_attached()` back inline; it reintroduces the first-paint clip.
Rationale + the view-config/first-paint root cause are in
`.agents/skills/auv3/SKILL.md → "Logic OOP first-paint clip"`. The same controller also calls `set_design_viewport_top_align(true)` so the AU design anchors to the TOP of a taller host pane (REAPER FX-chain) like CLAP/VST3 instead of centering between bands — see the auv3 SKILL.

**iOS editor sizing differs from macOS — it FILLS the pane.**
`au_view_controller_ios.mm` deliberately does NOT pin a design viewport: it
lays the root out at the actual pane bounds (via `resizeEditorToViewBounds` →
`set_size` + `ViewBridge::resize`) so a responsive flex scene fills edge-to-edge.
Aspect-locked design-viewport scaling letterboxed the pane (dark bars on the
sides) and pushed header text to the edge. The `notify_attached` → `resize`
protocol is unchanged; only the viewport policy differs. A genuinely
fixed-aspect iOS editor that wants letterboxing can call `set_design_viewport`
itself. See the `ios` skill for the responsive-scene + `syncCanvasSize` contract.

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

AU v3 render automation also attaches its block-local
`ParameterEventQueue` to the existing audio `Processor` immediately
before `process()` via `set_param_events(...)`. Treat that pointer like
the MIDI/MPE sidecars: it is valid only during the current process call
and must not be captured by editor/view code.

**Trigger / momentary params and the shared StateStore.** An editor that
raises a *trigger* parameter (`ParamInfo::is_trigger`, or a
`ParamDesignation::Reset` "reset/panic" control) on the shared store is
writing a one-shot: the adapter's render path calls
`StateStore::reset_triggers_rt()` after each `Processor::process` block,
which returns the parameter to its default. Editor/view code therefore
must not assume a trigger it set stays raised across frames — it is
observed for one block and then auto-settles. Read it back from the
store if you need to reflect the resting state in the UI.

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

Remote views attach through `ViewBridge::attach_remote_channel(channel, label)`
as `ViewRole::Remote` secondaries. The bridge does not own URL parsing or socket
creation; callers connect a `MessageChannel` first and hand it to the bridge.

## Parameter type-in round-trips through `ParamInfo` (shared with the editor)

The same `StateStore` every attached view polls (see *Secondary views*) is
also the source of truth for the host's parameter **display strings** and its
**generic type-in field**. As of G5 all four format adapters route the
host-facing text↔value conversion through the *same* `ParamInfo::to_string` /
`from_string` your editor draws from:

| Format | Host callback that now honors `ParamInfo` |
|---|---|
| VST3 | `getParamStringByValue` / `getParamValueByString` |
| AU v2 | `kAudioUnitProperty_ParameterStringFromValue` / `...ValueFromString` (+ `kAudioUnitParameterFlag_ValuesHaveStrings` when a `to_string` exists) |
| CLAP | `params_value_to_text` / `params_text_to_value` |

The CLAP `params_text_to_value` change is the one that lands on the shared
`clap_entry.hpp` (hence this skill is dual-mapped with `clap`): it now tries
`ParamInfo::from_string` **before** the generic locale-independent `strtod`, so
a fully custom rendering ("quality=0.75", an enum label, "12 o'clock")
round-trips instead of being rejected by a bare numeric parse. CLAP values are
plain (`min..max`) — the exact domain `from_string` returns — so no
normalization step is needed; VST3 converts to/from the host's normalized
domain around the same call.

**Editor consequence.** A parameter's on-screen value string (drawn by your
view) and the host's generic type-in field now agree, because both derive from
the same `ParamInfo` converters on the shared store. Two rules for editor
authors:

- **Don't add a parallel text parser in the editor.** Type-in a user enters in
  your own field should call the SAME `ParamInfo::from_string` (or bind through
  the store) so your field and the host's stay consistent.
- **Decline by returning a non-finite value.** A `from_string` that yields
  NaN/±inf is rejected: CLAP falls through to the numeric parse, VST3/AU decline
  to the base class. So a parameter with no meaningful string form should return
  NaN rather than a garbage value.

Tests: `test/test_clap_entry.cpp`, `test/test_vst3_param_display.cpp`,
`test/test_au_v2_param_display.mm`.

## Trackpad / Scroll-Wheel Zoom Event Path

**Searchable keywords**: trackpad zoom, scroll-wheel zoom, mouse wheel, "1.00x zoom" stuck, deltaY missing, deltaY=0, wheel event not firing, FilterBank zoom, Spectr zoom, onWheel, addEventListener('wheel'), registerWheel never called, wheel bubble, ancestor not receiving wheel, canvas-child captures wheel, wheel handler short-circuits.

**Symptom**: in Spectr (and likely any @pulp/react consumer that uses a
canvas child inside a wheel-handling wrap-div), scroll-wheel or
trackpad scroll over the canvas does NOT trigger the wrap-div's zoom
handler. The zoom indicator stays "1.00x" no matter how many wheel
events fire.

**Three independent bugs stacked**, all needed fixing to make zoom work:

### Bug A: `on(id, 'wheel', fn)` never invoked `registerWheel(id)`
The `on()` JS function in `kJSPreamble` mapped event names to native
registrars (click → `registerClick`, pointer events → `registerPointer`,
gesture events → `registerGesture`), but had **no case for `'wheel'`**.
Spectr's editor.js bound a wheel handler via `addEventListener('wheel',
fn)` which routes through `on(id, 'wheel', fn)`. The callback was
stored in `__callbacks__[id + ':wheel']` but the native side was never
told this view wanted wheel events. Result: `registerWheel` ran for 0
views, wheel events had no JS receivers.

**Fix**: add a `wheel` case to `on()` + a `wheel` group to
`__ensureNativeRegistered__()` so `on(id, 'wheel', fn)` calls
`registerWheel(id)` to wire the native dispatch. (`core/view/src/widget_bridge.cpp` `kJSPreamble`.)

### Bug B: bubble loop short-circuited on the wrong handler
`window_host_mac.mm::scrollWheel:` walked from the hit-tested deepest
view up to find the first ancestor with `on_pointer_event` set, then
delivered the wheel event there and returned. But the deepest hit
(typically a Canvas2D child) had `on_pointer_event` registered via
`registerPointer` (for pointerdown/up/move/cancel) — that lambda
short-circuits on `is_wheel` (it's the pointer-only handler). The
bubble therefore delivered the wheel event to a no-op handler and
returned, never reaching the wrap-div ancestor that had registered the
ZOOM handler via `registerWheel`.

**Fix**: change `scrollWheel:` to deliver to EVERY ancestor with
`on_pointer_event` set (not stop at the first). Each handler self-
filters on `me.is_wheel`: `registerPointer`'s lambda short-circuits
when `is_wheel == true`, `registerWheel`'s short-circuits when `false`.
So a view that registered both gets both halves; a view that registered
only one ignores the other. ScrollView ancestor still takes precedence
and stops the walk. (`core/view/platform/mac/window_host_mac.mm`.)

### Bug C: wheel-event payload must include `clientX/clientY/deltaY`
The bridge emits
`{deltaX, deltaY, clientX, clientY}` as an object (not positional args)
so the `@pulp/react` synthetic-event shim's `isPlainObject(rawArgs[0])`
branch can lift the fields. Without this, even after Bugs A+B were
fixed, `e.deltaY` would be undefined and `e.clientX - rect.left` would
read 0.

### How to diagnose if zoom is broken again

1. `fprintf(stderr, ...)` in `scrollWheel:` to confirm the NSView even
   receives the event — if not, accessibility / focus issue.
2. Confirm `registerWheel('<view_id>')` is being called after the
   view's editor.js mounts. If never called, Bug A is back.
3. Confirm the bubble walk reaches the wrap-div, not stopping at the
   canvas child. Print the chain `target → parent → … → root` and
   note which have `on_pointer_event` set.
4. Confirm `__dispatch__(view_id, 'wheel', {deltaX, deltaY, clientX,
   clientY})` is called with non-zero deltas. If `clientX/clientY` are
   0, `me.window_position` was not set in `scrollWheel:`.
5. In Spectr's `native-react/dist/editor.js` bundle, `grep deltaY` —
   expect ≥3 mentions. If 1 or 0, the bundle was built against an old
   `@pulp/react` without the synthetic-event delta fields and the
   bundle needs `npm run build:port`.

### Tooling: drive wheel events programmatically

`cliclick` has no wheel command (`w:N` is WAIT, not WHEEL). Compile a
tiny tool:

```c
// /tmp/scroll-event.c
#include <ApplicationServices/ApplicationServices.h>
int main(int argc, char** argv) {
    int count = argc > 1 ? atoi(argv[1]) : 10;
    int delta = argc > 2 ? atoi(argv[2]) : 5;
    for (int i = 0; i < count; i++) {
        CGEventRef ev = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, delta);
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
        usleep(50000);
    }
}
```

Build: `clang -framework ApplicationServices -o /tmp/scroll-event /tmp/scroll-event.c`. Posts real CGEvent scroll-wheel events that reach `NSView::scrollWheel:`. Use `/tmp/scroll-event 30 5` to inject 30 scroll-up events.

## Stale-Header ABI Mismatch

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
   bounds.** Prefer `pulp_add_plugin(... DESIGN_WIDTH N DESIGN_HEIGHT N)`
   over a per-plugin `view_size()` override for imported-design plugins;
   the macros flow into the default
   `Processor::view_size()` via `format::view_size_from_design(...)`,
   which derives `min = preferred * 2/3` and `max = 2 * preferred`. The
   derived `min > 0` is what makes CLAP's `gui_can_resize` engage.
   Override `view_size()` directly only when the dimensions are computed
   at runtime (rare).
5. **Forgetting that adapters must read `bridge.size_hints().min_*`
   when building the host's `WindowOptions`.** The bridge caches
   `Processor::view_size()` in `size_hints_`, but each adapter is
   responsible for forwarding `min_width`/`min_height` (and
   `preferred_*`) into its window/host options. The standalone
   adapter centralizes this in `detail::make_standalone_window_options`
   so the chrome-height shift is applied consistently. Other
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
   only fires when an unrelated event happens to pump the message loop.
   `ScriptedUiSession::poll()` does this pair on Mac/iOS standalone;
   `core/render/platform/android/gpu_surface_android.cpp::android_render_frame()`
   does the same pair on Android. Tests live under `[issue-1412]` in
   `test/test_widget_bridge.cpp`.
7. **Design-viewport pointTransform block holds raw `this` — clear it
   in the host dtor.** Both `MacPluginViewHost::~` and
   `MacGpuPluginViewHost::~` MUST run `view_.pointTransform = nil` /
   `metal_view_.pointTransform = nil` inside an `@autoreleasepool`
   BEFORE the C++ host frees itself. DAWs routinely retain the
   returned NSView after disposal (attach_to_parent hands it to the
   DAW's view hierarchy); a later `mouseDown:` would otherwise invoke
   the block on freed memory. This is the same ownership shape as the
   deferred-click teardown token. Test: `pulp-test-plugin-view-host-design-viewport`
   has the dtor-clears-pointTransform regression case.
8. **`paint_overlays` MUST run inside the design-viewport transform.**
   ComboBox dropdowns, claimed overlays, and the inspector layer all
   draw in root-view coordinates, and mouse input inverse-maps
   window→root through `pointTransform` before `hit_test`. Painting
   overlays outside the transform puts them at root coords in window
   space → visually misaligned + non-clickable at any host size that
   isn't exactly the design size. Mac plugin host paint paths (CPU
   `drawRect:` and GPU `paint_scene`) call `View::paint_overlays`
   INSIDE the save/translate/scale block, matching the standalone
   `MacGpuWindowHost::paint_scene` overlay-inside-transform rule.
9. **A GPU display-link idle pump can outlive its `ViewBridge` — guard on
   `ViewBridge::alive_token()`, not the host's own `alive_`.** The GPU
   scripted-idle pump (`gpu_host_select.hpp::make_scripted_idle_pump`) is
   dispatched to the main queue by `CVDisplayLink` and captures the bridge by
   raw pointer. Two ways it runs after the bridge is gone: (a) a host reloading
   the embedded view REPLACES the bridge (`_bridge = make_unique<…>`) while the
   host + its idle callback survive, and (b) `CVDisplayLinkStop` does NOT join
   an in-flight callback during teardown. The host's `alive_` token guards the
   HOST, not the bridge, so it does not cover case (a). Fix: `ViewBridge` owns
   its own `std::shared_ptr<std::atomic<bool>> alive_` (`alive_token()`), flipped
   false FIRST in `~ViewBridge` before `close()`; the pump captures a COPY of
   that token and no-ops when it reads false. This crashed the PulpTempoSampler
   AU embedded in Ableton Live 12 (EXC_BAD_ACCESS in `store()`/`scripted_ui()`
   on a freed bridge, v1.6.1). The token makes the no-op DECISION safe — it does
   NOT make a cross-thread deref of the raw bridge pointer safe, so teardown must
   stay on the main thread. General rule: any display-link/cross-thread callback
   capturing a raw pointer to a REPLACEABLE owned object needs a liveness token
   on THAT object, not just its host. Test: `[idle-pump][crash]` in
   `test_view_bridge.cpp` builds the pump, destroys the bridge, calls the pump →
   no-op instead of use-after-free.

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
- `"scripted idle pump no-ops after the ViewBridge is destroyed"`
  (`[idle-pump][crash]`) — the GPU display-link pump liveness-token guard
  (pitfall 9); build the pump, `reset()` the bridge, call the pump → no UAF.

Run with `ctest --test-dir build -R ViewBridge --output-on-failure`.

## Remote views

`ViewBridge::attach_remote_channel(channel, label)` registers a
`RemoteViewSession` driving a `MessageChannel` (WebSocket or in-process
loopback) as a `ViewRole::Remote` secondary. The session speaks the
protocol in `docs/reference/remote-view-protocol.md`:

- `view.hello` + `view.metadata` handshake
- `view.param_set` / `view.param_changed` wire through `StateStore`
- `view.param_get` request/response
- `view.input` (notification)
- `view.close` (either side)

Tests: `test/test_remote_view.cpp` covers handshake, metadata escaping,
parameter sync, input forwarding, close handling, null-channel rejection, and
stale-session detach behavior via MemoryMessageChannel loopback.

### Attaching from an MCP server

An MCP server that runs alongside a Pulp plugin host can open a
`RemoteViewSession` to drive the plugin's view from Claude Code:

1. MCP server can declare a tool (e.g. `view_attach`, `view_param_set`,
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
crashes the host process. Do not reintroduce an explicit close in the
AU dealloc path. The full rationale lives in the `auv2`, `auv3`, and
`ios` skills since those files are dual-/triple-mapped.

### Standalone editor lane DOES need explicit `bridge->close()` before `stop()`

The "never call close() explicitly" rule above applies to AU dealloc
because the bridge and host are RAII-owned and destroyed in reverse
declaration order. The standalone app lane in
`core/format/src/standalone.cpp` is different: `run_with_editor()`
exits when `window->run_event_loop()` returns, and that can happen via
two paths:

1. **Window-close callback path** — the close handler already called
   `bridge->close()` while processor and view were both alive.
2. **Application-quit path** — the event loop returns without ever
   firing the window-close callback, so the bridge never saw `close()`.

If path 2 falls through to `stop()` without an explicit close, the
processor is torn down with a still-attached bridge and the view's
`on_view_closed` either never fires or fires against a freed
processor. The fix is to call `bridge->close()` unconditionally after
the event loop returns and before `stop()` — `close()` is idempotent,
so path 1 stays correct.

When you add a new host lane (CLI, embedded runner, foreign host), do
the same audit: is the bridge owned by an RAII path that guarantees
ordering (AU pattern, no explicit close), or does the lane have a
return path that bypasses the window-close callback (standalone
pattern, explicit close before processor teardown)?

## EditorBridge — JSON message dispatch over the editor lifecycle

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
  `attach_native_runtime(JsRuntime&, "<handler_name>")` for the
  native-JS-runtime import lane. Same handler registrations.
- **Explicit WebView teardown.** `detach_webview(WebViewPanel&)`
  clears the callback installed by `attach_webview`. Use it when the
  bridge and `WebViewPanel` are side-by-side members and you want to
  sever queued WebView messages before native detach or panel destruction.
- **noexcept dispatch.** `dispatch_json(...)` and `dispatch(...)` are
  `noexcept` and always return a well-formed JSON response envelope —
  handler exceptions become `{"ok":false,"error":"internal error"}`.
- **Standard envelope error vocabulary** (substring-compatible with
  Spectr's existing test suite — that compatibility is load-bearing
  for the Spectr cutover acceptance criterion):
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

### registerShortcut focus-guard

`WidgetBridge::forward_key_event` checks `registerShortcut` entries
before falling through to the W3C `__global__` keydown dispatch. To
prevent global bare-key shortcuts (`?` for cheatsheet, etc.) from
firing while the user types into a text input, the loop suppresses any
registered shortcut whose modifier mask has no
`kModCtrl|kModAlt|kModMeta|kModCmd` bit set, IF a text-accepting
widget currently has focus. Shift alone counts as bare (Shift only
picks the upper-case glyph). Modifier chords (`Cmd+S`, `Cmd+,`) always
fire — they are always-global by design and must work even when an
editor has focus.

The focus signal is `View::focused_input_` (the same static slot the
macOS PulpView already maintains for text-input dispatch)
**narrowed** by `View::accepts_text_input()`. The slot is populated
for ANY focusable widget — Knob, Button, ListBox, TextEditor — via the
window-host focus path, so checking just `focused_input_ != nullptr`
would wrongly kill bare-key shortcuts after clicking a knob. The virtual `accepts_text_input()` returns false by
default; only `TextEditor` (and any future text-input widget) overrides
to true.

Prereq for the default-shortcuts pass (`planning/2026-05-16-default-
keyboard-shortcuts.md`), which adds a bare-`?` cheatsheet binding to
imported designs.

### Tests that pin the contract

`test/test_widget_bridge.cpp` — `[contract]` tag:

- "Event contract: W3C MouseEvent.button maps left=0, middle=1, right=2"
- "Event contract: forward_key_event emits W3C UIEvent.key strings + modifier booleans"
- "Event contract: window.addEventListener('keydown', fn) receives __global__ keydown"
- "WidgetBridge focus-guard: bare-key shortcuts suppressed while text input focused"
- "Event contract: __dispatch__ try/catch keeps listeners alive after a handler throws"
- "Event contract: wheel dispatch is an object {deltaX,deltaY,clientX,clientY}"
- "Event contract: registerPointer/registerWheel are idempotent (no lambda-stack growth)"

Run with: `./build/test/pulp-test-widget-bridge "[contract]"`

When adding any new event family or fields, add a corresponding
`[contract]` case AND a row to the payload-shape table above.

### Test and validator runs must not open native editor hosts

Adapter editor creation is guarded by `PULP_DISABLE_PLUGIN_EDITOR`,
`PULP_HEADLESS`, `PULP_TEST_MODE`, and `CI`. Validator and agent-driven
test paths must set the explicit `PULP_*` variables so VST3 returns no
editor view, CLAP hides `CLAP_EXT_GUI`, AU v2 returns no Cocoa view, and
AUv3 avoids constructing `PluginViewHost`. Do not replace this with
post-hoc window cleanup; the contract is that no native editor host is
created in the first place.

## Keyboard-focus host etiquette — never hold the host's first responder when idle

A plugin editor embeds an `NSView` in the DAW's window. If that view returns
`acceptsFirstResponder = YES` unconditionally (or the plugin sets a *focusable
root* via `set_focusable(true)`), then **clicking any control makes the plugin
view the host window's first responder, and every keystroke routes into the
plugin** — stealing the host's keyboard. In Logic this silences **Musical
Typing** on software-instrument tracks the instant you touch a plugin control,
which masquerades perfectly as an audio failure ("adjusting the plugin kills the
instrument until I reopen it" — reopening just refocuses a host view). No crash,
no log; unaffected by any audio/parameter fix; happens only on instrument
tracks (audio tracks don't need keystrokes) and only via the plugin's own GUI
(the host's generic control view never moves focus out of the host). Cost ~2
days to find — see the `au-xpc-shared-process-crash` debug note.

Contract (`core/view/platform/mac/plugin_view_host_mac.mm`, both the CPU
`PulpPluginView` and GPU `PulpGpuPluginView` classes — shared by AU/VST3/CLAP):

- `acceptsFirstResponder` returns true **only while** `View::focused_input_`
  is set (a widget is in active text input), not unconditionally.
- After every `mouseDown:`/`mouseUp:`/`keyDown:`, call `syncKeyFocus`:
  `makeFirstResponder:self` when a widget wants keys; the instant it doesn't,
  **restore the host's PRIOR first responder** (saved at claim time), not
  nil — handing nil leaves Logic's key routing dead (Musical Typing stays
  silent after a type-in commit until the user resets the track). The saved
  pointer is never dereferenced: it is re-found *by identity* in the window's
  live view tree (`pulp_plugin_live_prior_responder`), so a freed responder
  degrades to nil instead of a dangling send (the file builds without ARC).
- **The host's grab wins**: `resignFirstResponder` must end the focused
  widget's text input (`pulp_plugin_end_text_input`: clear the slot, then
  `on_focus_changed(false)` so the widget commits/closes its type-in).
  Otherwise a type-in left open when the user clicks a host control keeps
  `acceptsFirstResponder` true and re-steals the keyboard on the next event.
  Widgets with type-in UIs should override `on_focus_changed(false)` to
  commit, exactly like a click-away inside the plugin.
- **Scope every focus decision to THIS editor's root.** `View::focused_input_`
  is a process-GLOBAL static, so with two plugin editors open in one host
  (two instances of the same plug-in is the common case) it can point into a
  *different* editor's tree. `acceptsFirstResponder`, `syncKeyFocus`, the
  keyDown dispatch, and the resignFirstResponder text-input teardown must all
  gate on `pulp_focus_under_root(self.rootView)` (focused view is `root` or a
  descendant — walk `View::parent()`), never on the bare global. Otherwise
  editor B accepts/steals the keyboard, or routes keys into editor A, while A
  holds a focused field. All four contracts (claim/restore, host-grab ends
  input, freed-prior-responder safety, per-editor scoping) are pinned by
  `test_plugin_view_host_key_focus.mm` (real `NSWindow` + responder dance,
  single- and two-editor, CPU host).
- Editors must NOT set a focusable ROOT. Claim focus per-field:
  `claim_input_focus()` in `enter_typein()`, `release_input_focus()` in
  `commit_typein()`/`cancel_typein()`. This is the JUCE default
  (`wantsKeyboardFocus = false`; grab only for an active text field).

## GPU view host auto-selection — never hardcode `use_gpu=false`

The format adapters (AU v2 / VST3 / CLAP / iOS AUv3) must NOT hand-set
`PluginViewHost::Options::use_gpu`. They call the shared decision helper
`pulp::format::decide_gpu_host(bridge)` (`core/format/include/pulp/format/gpu_host_select.hpp`),
which returns `use_gpu` = `bridge.uses_script_ui() || bridge.view()->requires_gpu_host()`,
honoring the `PULP_DISABLE_PLUGIN_GPU` runtime opt-out. Hardcoding
`use_gpu=false` is the exact trap that made a Skia/Dawn editor silently fall
back to the AutoUi CPU path (the GPU-plugin-view-host work, 2026-05).

Rules when touching an adapter's editor-attach path:
- A custom `Processor::create_view()` that owns a `ScriptedUiSession` MUST
  override `Processor::active_scripted_ui()` so `ViewBridge` reports
  `uses_script_ui()`, adapters log `mode=scripted`, select the GPU host, and
  `make_scripted_idle_pump` can poll that session. Chainer-style generated
  processors use this path.
- A custom non-scripted GPU view (WebGPU/Three.js canvas, hand-built Skia view)
  MUST call `view->set_requires_gpu_host(true)` on its root, or
  `decide_gpu_host` returns `mode=autoui` and it gets CPU. The framework
  scripted-UI root (`editor_ui.hpp`) already sets it.
- After `PluginViewHost::create(...)`, call
  `format::warn_if_unexpected_cpu_fallback(decision, host.get())` — it screams
  (`runtime::log_error`) if GPU was requested but the host fell back to CPU.
- Wire the per-vsync scripted pump: `host->set_idle_callback(make_scripted_idle_pump(bridge))`.
  Without it a JS UI paints its first frame but `requestAnimationFrame` /
  timers / async results never fire.
- `make_scripted_idle_pump` captures the `ViewBridge` by raw pointer, so the
  host MUST be destroyed before the bridge. In AU/VST3/CLAP the bridge is
  declared before the host (reverse-destruction → host first); keep that order.
- AU v2 has no host resize callback — use `host->set_resize_callback(...)` to
  forward native-NSView frame changes to `bridge->resize()`. VST3/CLAP drive
  resize through their own host size callbacks and don't need it.
- **Embedded plugin views route their own mouse input.** The mac plugin views
  (`PulpGpuPluginView` GPU + `PulpPluginView` CPU, in `plugin_view_host_mac.mm`)
  implement `mouseDown/Dragged/Up/scrollWheel` → `hit_test` → `on_mouse_event` +
  `on_mouse_down/drag/up` with W3C pointer/drag bubbling to ancestors and
  drag-target liveness, reusing the standalone window host's `mac_geometry`
  helpers. Without these the editor paints but swallows every click. They also
  set flexible `autoresizingMask` so they follow host editor-container resizes.
- **AU specifically also needs the Cocoa view advertised** (`kAudioUnitProperty_CocoaUI`)
  or the host shows its own generic param view regardless of the host wiring —
  see the `auv2` skill's "AU v2 MUST advertise its Cocoa view" gotcha.

See `planning/2026-05-22-gpu-view-host-in-plugins.md` and its `qa/` doc.

## Proportional resize with aspect lock — design viewport (2026-05)

`PluginViewHost` now mirrors `WindowHost`'s design-viewport contract
so DAW-embedded editors can corner-drag
proportionally without re-laying out. Three new virtuals on the host
(no-op defaults; full impl on the mac CPU + GPU hosts today):

- `set_design_viewport(design_w, design_h)` — pin root at design size;
  paint applies an aspect-correct scale + letterbox translate to fit
  the current host bounds.
- `set_fixed_aspect_ratio(ratio)` — API parity only; the host doesn't
  own the OS window, so DAWs enforce the aspect via per-format hints.
- `window_to_root_point(pt)` — inverse-map a host-space point into
  root coords; called by native event handlers (mouse hit-test on
  resized windows) AND tests.

Per-format wiring:

- **CLAP** — `gui_can_resize=true`; `gui_get_resize_hints` sets
  `preserve_aspect_ratio=true` and `aspect_ratio_{w,h}=design_{w,h}`;
  `gui_adjust_size` snaps to the design aspect, then clamps to
  plugin min/max; `gui_create` calls `host->set_design_viewport(...)`
  + `host->set_fixed_aspect_ratio(...)`.
- **VST3** — `canResize=kResultTrue`; `checkSizeConstraint` snaps to
  the design aspect; `onSize`'s existing `host->set_size(...)` path
  resizes surfaces; `attached()` (or first `onSize`) calls
  `host->set_design_viewport(...)`.
- **macOS AUv3** — no CLAP/VST3-style drag constraint callback.
  `PulpAUMacViewController` must create its initial root view at the
  compile-time design size when `PULP_PLUGIN_DESIGN_W/H` are available,
  then call `host->set_design_viewport(...)` after `ViewBridge` opens.
  `supportedViewConfigurations:` should return aspect-correct host
  configs first, but only if they are large enough for the design;
  wrong-aspect "large enough" configs are fallbacks. Undersized fixed-
  design configs should be rejected so CoreAudioKit can choose the
  largest available configuration. REAPER's in-process AUv3 path can
  still shrink the first live layout after attach, so the macOS
  controller has a one-shot initial size sync that expands the host
  window by the view delta and reapplies the design size before normal
  `viewDidLayout` resize takes over.
- **AU v2** — cannot offer this; the DAW resizes the returned NSView
  directly with no host-side resize-hint analogue.

Pitfall to remember: when wiring `gui_set_size` / `onSize`, do NOT
re-layout the view at the host window size when a design viewport is
active — the host's paint already applies the design transform, and a
window-sized layout would briefly flash before the next paint reset.

See `test_plugin_view_host_design_viewport.mm` for the wiring proof.

## GpuSurface Plumbing Into WidgetBridge

Adapter editor-attach paths that wire a scripted UI (`ScriptedUiSession`)
MUST hand the host's live `render::GpuSurface*` to the bridge so the
JS-side `navigator.gpu` / `canvas.getContext('webgpu')` shim routes
through Pulp's real Dawn instance. Without it, those shims fall through
to mocks and any JS-rendered WebGPU output (Three.js, raw WebGPU) is
black.

The order is fixed by the adapter editor lifecycle: `ViewBridge::open()`
constructs the `ScriptedUiSession` (and its `WidgetBridge`) BEFORE
`PluginViewHost::create()` allocates the `GpuSurface`. Two new API
points close that gap:

- `pulp::view::WidgetBridge::attach_gpu_surface(GpuSurface*)` — idempotent
  late-attach; nullable. Stores the pointer + lazily allocates the
  native GPU bridge state. Pair with `gpu_surface()` /
  `has_native_gpu_bridge()` for introspection / tests.
- `pulp::view::ScriptedUiSession::attach_gpu_surface(GpuSurface*)` —
  forwards to the active bridge AND stashes the pointer so any later
  hot-reload rebuild constructs the next bridge with the same surface.

Adapters call this after the host is built. Reference wiring in:

- `core/format/src/au_view_controller_ios.mm` (iOS AUv3)
- `core/format/src/au_view_controller_mac.mm` (macOS AUv3)
- `core/format/src/vst3_plug_view.cpp` (VST3)
- `core/format/src/au_v2_cocoa_view.mm` (AU v2)
- `core/format/include/pulp/format/clap_entry.hpp` (CLAP)

The expected log line on success:

```
[plugin-gpu-host] GpuSurface attached to WidgetBridge via ScriptedUiSession (<format>)
```

`PluginViewHost::gpu_surface()` is a new virtual mirroring
`WindowHost::gpu_surface()` (CPU hosts inherit the nullptr default; GPU
hosts on iOS/macOS override). Future Windows/Linux factory hosts that
want WebGPU JS plumbing must override the virtual too.

Coverage: `pulp-test-widget-bridge "[gpu-surface-plumbing]"` (ctor +
late-attach + idempotence + detach) and `pulp-test-scripted-ui
"[gpu-surface-plumbing]"` (session-level forwarding). A live-Dawn
counterpart should use the `[jsc][navigator-gpu][live]` test path when it lands.

See `planning/2026-05-29-ios-d3b-threejs-webgpu-program.md`
for the full rationale.

**Why this matters:** Without this plumbing, format adapters silently route
Three.js + WebGPU canvas calls to mock adapters in production plug-ins. Do not
bypass it — if you're writing a new format adapter or plugin host, route your
live `GpuSurface*` through `ScriptedUiSession::attach_gpu_surface()` (or
`WidgetBridge::attach_gpu_surface()` if you don't have a session yet).

**`presentable` flag**: `WidgetBridge`'s `__gpuCanvasConfigureImpl` and
`__gpuCanvasDescribeCurrentTextureImpl` both expose a `presentable` boolean to
JS. `true` iff `gpu_surface_->has_surface()` is true (i.e., the surface has a
real swapchain, not just an offscreen texture). Three.js draws to a
`presentable=false` canvas land in a silent offscreen render that's not
composited to the visible AUv3 editor. Always check this flag in any new GPU
bridge code path.

## References

- `core/format/include/pulp/format/view_bridge.hpp` — public API
- `core/format/include/pulp/format/gpu_host_select.hpp` — GPU host auto-select helper
- `core/format/src/view_bridge.cpp` — implementation
- `core/format/include/pulp/format/processor.hpp` — `create_view`,
  `view_size`, `on_view_*`
- `core/format/include/pulp/format/remote_view_session.hpp` — remote session API
- `core/view/include/pulp/view/editor_bridge.hpp` — EditorBridge API
- `core/view/src/editor_bridge.cpp` — EditorBridge implementation
- `docs/guides/view-bridge.md` — user-facing guide
- `docs/reference/editor-bridge.md` — EditorBridge reference
- `docs/reference/remote-view-protocol.md` — remote-view wire format
- `examples/view-bridge-demo/main.cpp` — runnable headless demo
- `test/test_remote_view.cpp` — loopback tests for the remote protocol
- `test/test_editor_bridge.cpp` — EditorBridge unit tests
- `planning/next-features-plan.md` § Feature 1 — historical planning context

## Plugin-contributed settings sections

`Processor::settings_sections()` lets a plugin surface its own Settings tabs (e.g. a model
picker) that the **host composes** alongside its host-owned Audio/MIDI tabs — keep device
selection a host concern (a plugin can't pick the audio device in a DAW; the host owns it),
while still giving one unified Settings panel. The standalone chrome
(`make_standalone_editor_chrome`) calls `processor.settings_sections()` and appends each via
`SettingsPanel::add_section(title, view)` after the Audio/MIDI tabs. Gotchas:

- The `Settings` tab (with Audio/MIDI) only exists when `StandaloneConfig::show_settings_tab`
  is true; that's also the gate for composing plugin sections. A plugin that wants its
  settings visible in the standalone must leave it on.
- Do NOT pull host audio settings down into the plugin editor — invert it: contribute the
  plugin's sections up. In a DAW the same `settings_sections()` show with no Audio/MIDI tab,
  automatically correct.
- `make_standalone_editor_chrome` accesses `StandaloneEditorChrome`'s private members via a
  `friend` declaration — if you change its signature, update the friend decl to match or it
  silently loses friendship and fails to compile on the private-member access.

## Headless screenshot captures native overlays

`StandaloneApp::run_with_editor`'s `--screenshot` path normally reads the host's
Skia back buffer (`capture_back_buffer_png`), which can't see an OS-composited
**native overlay** (a WebView child view). When the editor view hosts a native
overlay (`View::contains_native_overlay()`), the standalone now routes through
`pulp::view::capture_view()`, which calls the overlay's
`capture_native_overlay_png()` (e.g. `WebViewPanel::snapshot_png()` → WKWebView
takeSnapshot) so WebView editors are self-verifiable headlessly. A plain Skia UI
falls through to the back-buffer capture unchanged. See the `screenshot` skill.

## Standalone audio callback: size for the device's MAX block, NOT the nominal buffer

`StandaloneApp::start()` reads `audio_device_->buffer_size()` into
`config_.buffer_size` and it is tempting to size the processor + every scratch
buffer to that. **Don't.** The device's nominal buffer is not the largest block
the render callback can deliver: when the hardware sample rate differs from the
app's configured rate, CoreAudio (and other backends) splice in a resampler
(`AudioConverter…fillComplexBuffer` in the crash stack) that pulls the callback
in *variable, larger-than-nominal* blocks — up to the host's
`MaximumFramesPerSlice` (4096 by default on macOS). A user simply selecting an
output device at a different rate is enough to trigger it.

The 2026-06 symptom: a standalone SIGABRT on `com.apple.audio.IOThread.client`,
~85 min in, in `RealtimePitchTimeProcessor::process` →
`assert(num_samples <= config_.max_block)`. The processor had been
`prepare()`d for the nominal buffer; an oversized resampler pull blew the
assert (and, in NDEBUG, would have overflowed the pre-allocated scratch).

The contract (`core/format/src/standalone.cpp`, `start()` + the audio lambda):

- Compute `max_callback_block_ = std::max(config_.buffer_size, 4096)` once, and
  use it for `PrepareContext::max_buffer_size`, `test_buffer_`,
  `silence_buffer_`, and `output_probe_.prepare(...)` — every buffer the
  callback can write, sized to the MAX, not the nominal.
- Guard the callback head: if `ctx.buffer_size > max_callback_block_`, fill the
  output with silence, `log_error` once, and `return` — never let an
  over-max block reach `process()`. A backend that ignores
  `MaximumFramesPerSlice` degrades to a one-time warning, not a crash on a real
  user's machine.
- The silence early-return must STILL advance the transport clock
  (`transport_position_samples_.fetch_add(ctx.buffer_size, ...)` when
  `transport_playing`) before returning. The normal path advances after
  `process()`; an early `return` that skips it lags the transport position —
  and the MIDI timeline derived from it — by exactly the dropped frames.
- The member lives in `standalone.hpp` (`int max_callback_block_`). The audio
  device's `CallbackContext::buffer_size` is the *actual* frames this block
  (it can exceed the nominal), so the guard compares against it, not
  `config_.buffer_size`.

Rule of thumb for any RT host you write: prepare for the worst-case block the
backend may hand you, then assert/guard against anything larger. Nominal buffer
size is a hint, not a ceiling.

## Standalone audio-callback probe tap (Phase 5 observability harness)

`StandaloneApp`'s audio callback carries an optional realtime output-boundary
probe, gated behind the `PULP_ENABLE_AUDIO_PROBES` CMake option. The default is
ON for dev/example builds; release and SDK build paths pass
`-DPULP_ENABLE_AUDIO_PROBES=OFF` so shipped standalone artifacts do not export
the dev probe surface unless a developer opts in. When ON, `start()`
`prepare()`s `output_probe_` (the only place it allocates) and the callback
calls `output_probe_.analyze_output(...)` immediately after
`processor_->process(...)` — the "standalone processor-output boundary." This
is the first wired probe stage for "UI works, no sound" reports. Gotchas:

- It is NOT the input meter bridge. `input_meter_bridge_` is input-oriented and
  lacks the snapshot's stage/sequence/NaN/clip/silence fields. Don't conflate.
- The tap is fully `#if PULP_ENABLE_AUDIO_PROBES`-guarded; an OFF build links no
  `AudioProbe` symbols into `standalone.cpp.o` (verified by `nm`). If you touch
  the callback, keep the probe strictly inside that guard so OFF builds pay $0.
- The probe is RT-safe (scalar-only, no FFT/alloc/locks) — `pulp::audio::AudioProbe`.
  Do NOT model audio-thread work on `pulp::view::VisualizationBridge`, which runs
  STFT and returns `std::vector` in its callback (explicitly quarantined).

### Phase 6 — Audio Inspector tool window wired into the host

`run_with_editor()` opens a `pulp::view::AudioInspectorWindow` (a SEPARATE
floating window, sibling of the layout inspector — not a tab) that observes
`output_probe_`. It is created only when a real `WindowHost` exists, behind the
same `#if PULP_ENABLE_AUDIO_PROBES` guard. Wiring gotchas:

- **Key routing must not clobber the layout inspector.** The audio inspector's
  toggle (Cmd/Ctrl+Shift+A) dispatches through a shell-owned `CommandRegistry`
  via `route_global_keys(window_root, registry)`, which writes
  `window_root.on_global_key`. The layout inspector (Cmd+I) uses
  `on_global_click` (`install_inspector_hooks`). Distinct hooks → they coexist;
  do not move either onto the other's hook.
- **Compose the idle callback, don't replace it.** `poll()` must run each tick
  to refresh meters; capture the existing `pre_screenshot_idle` and call it
  first, then `audio_inspector_->poll()` — clobbering `set_idle_callback` drops
  the scripted-ui / settings / overlay paint.
- **Open + live waveform are env-gated.** `PULP_AUDIO_INSPECTOR` shows the
  window AND makes `start()` size `output_probe_`'s capture ring (to
  `AudioWaveformView::kCapacity`); without it the probe stays summary-only (no
  waveform allocation). Headless `--screenshot` also writes the inspector's own
  surface to `<stem>.audio-inspector.png` when it is open.
- **Teardown order:** destroy the window before the `CommandRegistry` (its RAII
  handler removal targets a live registry) and before `output_probe_`.
- **Panel colors:** `canvas::Color` channels are floats in `[0,1]` — build panel
  colors with `Color::rgba8(r,g,b,a)`, NOT `Color{26,26,32,255}` brace-init,
  which Skia clamps to opaque white (the white-on-white panel bug).
- **The editor idle pump drains the param store (automation → widgets).**
  `make_scripted_idle_pump(bridge)` (in `gpu_host_select.hpp`, set as every
  format's `set_idle_callback`) calls `bridge.store().pump_listeners()` each
  vsync. This is what makes `bind_parameter`-bound widgets follow host
  automation playback / host-side edits: `ListenerThread::Main` store changes
  are queued (the adapter writes the store from the audio thread) and ONLY
  fire on `pump_listeners()` on the UI thread. Without this drain, bound
  widgets never move during playback. The idle callback also keeps the GPU
  host's frame loop alive (`has_idle`), so the pump runs even when nothing
  else is animating. A custom view that reads the store directly each frame
  (not via `bind_parameter`) instead needs `View::set_continuous_repaint(true)`.
- **A native `create_view()` sizes the host window from its own layout bounds.**
  In `ViewBridge::open`, when the editor is native (not a scripted UI) and the
  view reports non-zero `bounds()`, those bounds override the processor's
  `view_size()` hints (`preferred_width`/`preferred_height`). A native editor
  that lays itself out — but doesn't declare `DESIGN_WIDTH`/`DESIGN_HEIGHT` —
  would otherwise get the default window size, which can be narrower than the
  laid-out content, so the right column + edge padding fall off the window's
  right/bottom edge. Scripted UIs keep the processor-declared `view_size()`
  (their layout is driven from JS/Yoga, not C++ `bounds()`). This is SDK-level:
  every native editor is sized correctly without per-plugin hardcoding — do not
  reintroduce per-plugin window-size constants to work around clipped editors.

## In-DAW scripted-UI hot reload is opt-in (dev only)

The DAW editor paths (`au_view_controller_mac.mm`, `au_v2_cocoa_view.mm`,
`au_view_controller_ios.mm`) build the `ViewBridge` with
`.enable_hot_reload = pulp::format::dev_editor_hot_reload_enabled()` — a
header-inline helper in `view_bridge.hpp` that returns true only when the host's
environment sets `PULP_DEV_HOT_RELOAD=1` (or `t`/`T`/`y`/`Y`). Default is OFF: a
shipping plugin must never watch + reload scripted UI / `theme.json` from disk
inside a host. The standalone app (`standalone.cpp`) always enables it — it is
the dev tool. Editor polling runs every tick regardless; the flag only decides
whether the watcher acts. To use the in-DAW edit→see-it loop, export
`PULP_DEV_HOT_RELOAD=1` in the DAW's environment before launching it. (Live-swap
plan item 1.3.)

## Scripted UI joins a unified live-swap transaction (SwapUnit)

`ScriptedUiSwapUnit` (`core/format/include/pulp/format/reload/scripted_ui_swap_unit.hpp`)
adapts a `ScriptedUiSession` to the `SwapUnit` contract so a UI swap composes with
a DSP swap (`DspReloadSwapUnit`) under `apply_live_swap` — a content pack that
carries both UI and DSP applies as ONE transaction (item 1.8b/2.5b). Notes:
- The adapter is **path-based**: `to_stage()` captures `session.script_path()`,
  `apply` = `reload_from(new_script)`, `rollback` = `reload_from(prev_script)`.
  It reuses the public `reload_from` (last-good, state-preserving) — no new view
  API. The UX unit is ordered FIRST (cheap to re-apply); a later DSP failure rolls
  it back to the previous bundle.
- It lives in the FORMAT layer (format→view is the allowed direction; view never
  links format), in its own header so DSP-only users don't pull in view.
- Value-perfect widget-state restore across a rollback is a refinement (rollback
  re-runs the previous script; `reload_from`'s own snapshot/restore preserves
  matching widget values across the rebuild).

## Custom widgets carry their own reload state (item 1.4b)

`WidgetBridge` snapshots/restores BUILT-IN widget state by type across a scripted-
UI reload (knob/fader/range/toggle/checkbox/togglebutton scalar; combo/segmented
selection index; xy pad — `widget_bridge.cpp` `snapshot_values`/`restore_values`).
A CUSTOM widget (a `View` subclass) opts into carrying its OWN state by overriding
the `View` virtuals `save_reload_state(std::string&)` / `restore_reload_state(std::string_view)`
(default return false → existing widgets unaffected). The bridge calls them for
every widget in `widgets_` and stores the opaque blob in
`WidgetReloadSnapshot::custom_state` keyed by script id; restore hands the blob
back to the widget still living under that id (id/type change → no match, fail-
safe). Note: `widgets_` is populated by the JS `createX` registrars (built-ins
only today), so end-to-end coverage through a JS-created custom widget awaits a
custom-widget registration path — the `View` hook + bridge wiring are in place for
when one exists.

## Live editor reload (in-place rebuild on hot-swap) — 1.9

A hot-reload plugin whose logic hot-swaps its `create_view()` needs the OPEN
editor to rebuild in place — otherwise the DSP swaps live but the panel only
updates when the DAW re-instantiates the plugin (the symptom that surfaced this).
The mechanism is format-agnostic and driven by the shared idle pump:

- **Signal, don't marshal.** `Processor::supports_editor_reload()` +
  `editor_reload_generation()` (additive virtuals, defaults false/0).
  `ReloadableShell` overrides them; an atomic counter bumps on each successful
  swap (both `reload_now()` and the watcher). The editor **polls** the generation
  on its UI-thread tick — the reload fires on the control/watcher thread, so
  polling avoids cross-thread UI mutation. Don't wire `set_on_reloaded` straight
  into UI work.
- **Preserve the root object identity.** `PluginViewHost` captures `View& root`
  at `create()` — there is no replace-root API. So `ViewBridge::rebuild_primary_view()`
  TRANSPLANTS the fresh `create_view()` output (children + background) INTO the
  same root `View` the host still references, rather than swapping `view_`. A
  logic whose `create_view()` returns a custom `View` **subclass** with root-level
  paint gets children+bg refreshed but not the subclass identity (fine for the
  common container-root editor).
- **Repaint after rebuild.** The CPU (CoreGraphics) mac host only repaints on
  `setNeedsDisplay`, so `make_scripted_idle_pump` calls `View::request_repaint()`
  after a rebuild. Mutating the tree alone does NOT repaint on CPU.
- **One wiring point.** `make_scripted_idle_pump` (gpu_host_select.hpp) covers AU
  v2/v3, VST3, CLAP, and both the CPU CVDisplayLink tick and GPU display link.
  `set_idle_callback` reads "GPU only" in the base header, but the mac CPU host
  (plugin_view_host_mac.mm) runs it via its own CVDisplayLink — so the pump does
  tick on CPU editors.

Test: `test_view_bridge.cpp` `[reload]` cases — a reloadable stub rebuilds into
the same root object with new content/bg, is idempotent, and is inert for a normal
processor. `examples/hot-reload-morph` exercises it end-to-end.

## Standalone is a transport — it must derive the same playhead change flags

`StandaloneApp` synthesizes transport (tempo, time signature, `position_beats`
from the rolling sample clock, `is_playing` from the user's play/stop toggle), so
it *is* a host for the Processor running inside it. It must therefore populate the
same derived `ProcessContext` fields the VST3 / AU / CLAP adapters do:

```cpp
detail::derive_bar_from_beats(proc_ctx);
detail::compute_playhead_changes(proc_ctx, playhead_prev_);   // member snapshot
```

It previously hand-rolled the `bar` derivation and never computed the change
flags at all, so `tempo_changed` / `time_sig_changed` / `transport_changed` /
`transport_started` / `transport_jump` were permanently `false` in the standalone
build. A tempo-synced generator therefore behaved differently in standalone than
in the plugin builds — and standalone is exactly where such generators get
developed. `playhead_prev_` is touched only from the audio callback.

## `transport_started` fires on the play edge; `should_reset_dsp_state()` does not

`ProcessContext::should_reset_dsp_state()` is `reset_requested || transport_jump`.
Pressing play at a **parked** position does not move the playhead, so
`compute_playhead_changes()` correctly reports no jump — and therefore
`should_reset_dsp_state()` returns `false` on the block where the transport starts.

That is correct for delay/reverb tails (a start is not a discontinuity, and tails
must survive it) and catastrophic for anything with run-relative phase. A
tempo-synced clock, LFO, or step sequencer that keys its phase reset off
`should_reset_dsp_state()` resumes a stale free-running phase and emits every
backlogged event on the first block of playback — a **pulse burst on play**.

Use `ProcessContext::transport_started` instead. It is deliberately *not* folded
into `should_reset_dsp_state()`, because the two answer different questions:
"did the timeline jump?" versus "did a run begin?"

Two subtleties:
- A processor instantiated **while the transport is already rolling** has no
  previous block to diff against (`!snapshot.has_previous`), so
  `transport_started` is set from `is_playing` on that first block. Without this a
  clock sits dead until the user stops and restarts the transport.
- Better still, derive event positions from `position_beats` outright rather than
  from an accumulator. Then a start, a seek, and a loop wrap are the same case,
  and none of them can produce a burst.

## The StateStore must outlive the Processor

`Processor::state()` dereferences a pointer the host installs. A Processor may
follow it for its whole lifetime — from `process()`, from its destructor, and from
any worker thread that destructor is about to `join()`. So the host has to keep the
store alive until the Processor is gone.

In practice that is one rule about member order: **declare the `state::StateStore`
before the `std::unique_ptr<Processor>`.** Members are destroyed in reverse
declaration order, so the store then dies last. Every host in `core/format` had it
backwards until 2026-07; the effect is nothing at all for a Processor with no
threads, and a use-after-free on plug-in close for one with a background thread that
reads `state().get_value()` while the destructor walks to its `join()`.

It crashes only on close, only sometimes, and the DAW gets the blame. The regression
test is `test/test_store_lifetime.cpp`; it observes the store's destruction through a
sentinel owned by a parameter's `to_string` closure rather than reading freed memory
and hoping the result looks wrong.

A Processor should not *rely* on this either: a worker thread that reads the store on
every tick is one host away from the same crash. Publish what the thread needs to
atomics from `process()` instead.
