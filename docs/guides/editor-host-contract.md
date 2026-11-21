# Editor-host contract

`pulp/format/editor_host.hpp` is how a host opens, sizes, feeds and tears down a
processor's editor without knowing which UI toolkit draws it. It is one header
in `pulp::format-core`, it names no view, canvas, render or GPU type, and it
adds nothing to a processor that does not use it.

The existing editor path (`Processor::has_editor()`, `create_view()` and
`ViewBridge`, described in [ViewBridge](view-bridge.md)) is unchanged. The VST3
(`ContractPlugView`), CLAP (the contract `clap_plugin_gui`), AU v2
(`au_v2_contract_view.mm`), AU v3 (macOS and iOS, `au_v3_contract_view.mm`) and
standalone adapters open a processor's editor through the contract whenever it
returns a factory from `editor_factory()`, and keep the existing path for a
processor that does not. On iOS the AU v3 path is compile-checked only. AU v2 chooses at build time (a `UI <package>` plug-in links the contract view,
and a `create_view()` plug-in keeps its ViewBridge view); the other adapters
choose at run time.

## The pieces

| Type | Role |
|---|---|
| `EditorFactory` | Builds an `Editor` for `(Processor&, EditorContext&, EditorRole)`, and can `prewarm()` its caches on a background thread before the first open. A processor returns it from `Processor::editor_factory()`; null (the default) means no factory. |
| `Editor` | One editor in one native window: `attach` / `detach`, `size` / `set_size` / `size_hints` / `adjust_size`, `idle`, `visibility_changed`, `focus_changed`, `offer_key`, `parameter_highlight`, `automation`, and `invalidate`. |
| `EditorContext` | What the host gives the editor: `params()`, `visuals()`, `request_resize()`, `host_action()`, `host_kind()`, and `query_extension()` for capabilities added later. |
| `ParamPort` | Parameter info, `get`, `begin` / `set` / `end`, normalized helpers and listeners. |
| `StateStoreParamPort` | The default `ParamPort`, over the processor's `StateStore`. |
| `VisualPort` | Read access to the lock-free values and meters the audio thread publishes. |
| `EditorAutomation` | Optional `capture_png` / `inject_input` / `observe` provider, so tooling can see and drive any editor. |

Roles are `editor`, `inspector` and `settings`; several editors, of one role or
several, can be alive at once over one processor.

## Audio/MIDI settings in the standalone

A standalone host has no DAW to own audio and MIDI routing, so it owns a
settings surface. An editor asks for it with
`EditorContext::host_action({HostAction::open_settings})`; a plug-in host (VST3,
CLAP, AU) returns false, because the DAW does that job. The editor should hide
its settings control when the request is declined.

`pulp::standalone-contract` (the standalone of a UI package) opens a small
native window on macOS, from the editor's request or from the app menu item
"Audio/MIDI Settings…" (Cmd+,). It lists the output device, the audio input
(an on/off choice over the system default input device, hidden when the
processor has no audio input; the backends open one device, so there is no
per-input picker), the sample rate
(`StandaloneConfig::allowed_sample_rates` when set), the buffer size
(`allowed_buffer_sizes`), the MIDI input (with "None") and the output latency.
Apply restarts audio through `StandaloneApp::apply_config()`, which saves the
selection for the next launch. A failed Apply puts the last working config back so audio keeps running; the window, the SDK's `apply_config()` and a later Apply never report success while audio is stopped. A device or port that has disappeared is not re-applied: each row's value comes from the row's own selection. Device lists come from the audio and MIDI
systems, so `PULP_AUDIO_DEVICE=null` shows the null device.

An app with its own settings UI replaces the opener:

```cpp
app.set_settings_window_opener([](pulp::format::StandaloneApp& app) {
    return my_settings_ui.show(app);   // true when a window is showing
});
```

The rules behind both the native window and the view `SettingsPanel` are in the
view-free `StandaloneSettingsModel` (`standalone_settings_model.hpp`): what each
list offers, the input-hidden rule, how selections fold into a
`StandaloneConfig`. A UI of your own can build on it. `StandaloneConfig::kNoMidiInput`
is the `midi_input_id` that opens no MIDI input (an empty id opens the first port).

## Choosing a UI stack

The contract is toolkit-neutral, but Pulp has one default answer:

| Stack | When | Cost to a build that does not use it |
|---|---|---|
| **Vellum on Skia/Dawn** (default) | Every new editor, imported design and generated UI, unless one of the rows below applies. | — |
| **RmlUi** (opt-in Vellum component, its `rmlui/` subproject) | A product that already ships RML/RCSS skins or Lua skin scripts, or a developer who asks for it. | None: off unless enabled, and it needs neither Skia nor Dawn. |
| **Native surface** (bring your own renderer) | An existing Metal renderer that draws into the editor's window: `integrations/pulp-native` on macOS ([guide](native-surface-editors.md)). | None: it needs system frameworks only. |

The rows are alternatives, not rivals. Do not mix two stacks in one editor
without a stated reason, and never put a Dawn device and a privately owned GPU
device in the same editor. The `rmlui` agent skill
(`.agents/skills/rmlui/SKILL.md`) says where RmlUi work lives.

## How a plug-in gets its default editor

A plug-in's editor is wired per plug-in, never by a process-wide registration.
`pulp_add_plugin(... UI <package>)` generates a hook in that plug-in's own
binary that returns the named UI package's `EditorFactory`, the way a VST3
plug-in supplies its own view from `createView()`. Several plug-ins loaded into
one host process therefore never share or override each other's editor choice.
A processor can also provide a factory by overriding
`Processor::editor_factory()`.

### Vellum editor roots

`pulp::vellum::EditorFactory<Root>` constructs a Vellum root with
`(EditorEnv&, format::Processor&)` when that constructor is available and with
`(EditorEnv&)` otherwise. A root can return a `format::EditorSize` from
`preferred_editor_size()` to set the initial window and root bounds. The default
is 400×300. Use `env.request_resize(size)` to ask the host for a different
size. The host applies accepted sizes through `Editor::set_size()`.

A root authored at a fixed design size returns a `format::ViewSize` from
`editor_view_size()` instead. Its preferred size opens the editor, its minimum,
maximum and aspect ratio become the editor's size hints, and every host size is
clamped to them. When it declares `design_width` and `design_height`, the root
is laid out once at that size and the plug-in view host scales its paint and
maps input into design coordinates; the window size never re-lays it out.

The host shows `Processor::editor_background()` before the first frame and
wherever the root does not paint. On AU v2 the view container is backed with
the same colour, so the DAW never flashes the SDK default.

Optional `editor_opened()` and `editor_closing()` methods run after successful
native attachment and before detachment or invalidation, respectively. Use
`editor_idle()` to poll a deferred scripted session. A root that publishes a
session through processor state should check that the published session still
belongs to that root before clearing it.

For a root-owned `vellum::view::ScriptedUiSession`, construct the session with
`env.params` and register the existing JSON dispatcher with
`session.attach_native_message_handler(name, env.native_message_handler(dispatch))`.
Link `Vellum::MirrorView_script` for this SDK API. Register before the first
script load to serve messages during evaluation. Registrations stop dispatching
when the editor closes. Detach them with
`session.detach_native_message_handler(name)` before destroying their captures,
and register fresh handlers when reopening the same root.

## Extending the contract

`Editor` and `EditorContext` are implemented in different binaries (a UI
package, a format adapter), so a virtual appended to either breaks a prebuilt
one. New host capabilities go through `EditorContext::query_extension(id)`
instead: the id is a stable, versioned string naming an interface, the host
returns a pointer to it (owned by the host, valid while the context lives) or
null when it does not offer it, and the default returns null. The GPU device is
not one of these: an editor acquires it from the shared GPU runtime with a
role tag. `tools/scripts/node_abi_gate.py` holds `Editor`, `EditorContext` and
`EditorFactory` frozen.

### Native surfaces

`dev.pulp.native_surface/1` (`pulp/format/native_surface_host.hpp`) is for an
editor that brings its own renderer and presents into a native layer. Look it
up with `native_surface_host(context)`. It reports what the host knows about the
editor's window: the display, the backing scale the host set, a nominal refresh
rate, and whether the host shows or hid the window. Each field reads as unknown
until the host says otherwise, and an observer hears every change on the main
thread. `ContractEditorSession` offers it to every editor, fed from `set_scale()`
and `visibility_changed()`; an adapter that knows the display publishes it
through `ContractEditorSession::native_surface_host()`.

Two rules come with it:

- **The editor owns its GPU device.** This is the one exception to the
  shared-runtime rule above, and such an editor never uses the shared runtime
  as well.
- **A render thread presents, nothing more.** It may acquire and present its own
  drawables, but it never calls the context, the extension, or
  `ParamPort::begin` / `set` / `end`. The editor stops it before `detach()` or
  `invalidate()` returns.

## Threading

Every call runs on the host's main (UI) thread, including creating and
destroying editors. `ParamPort::get()` and `ParamPort::info()` may be called
from any thread. A `VisualPort` has one reader thread, normally the editor's
frame tick; the audio thread publishes without locks.

### Main-thread work on Linux

An editor that posts work to the main thread (`platform::MainThreadScope`) or
runs a timer (`platform::MainThreadTimer`) runs it on the host's UI thread
through the host's own run loop, with no thread or X connection of its own. The
adapters lend the loop to the platform layer while an editor can run:

- VST3: the view's `IPlugFrame` must offer `Linux::IRunLoop`. `setFrame()` lends
  it (timers through `registerTimer`, wake-ups through `registerEventHandler`)
  and `setFrame(nullptr)` takes it back, stopping everything made under it.
- CLAP: `gui_create` lends the host's `timer-support`, and `posix-fd-support`
  when the host has it; a host without descriptor watches is polled on a short
  timer instead, but only for scopes an editor makes itself.

Without a loop (no editor open, or a host that offers none) `post` is refused
and timers never fire, so a caller sees the refusal rather than a task that
never runs. Destroy an editor's scopes before the editor's window goes; a
loop that goes first stops them.

## Parameters

Values are in the parameter's plain domain. A continuous edit is
`begin(id)`, one or more `set(id, v)`, then `end(id)`; the host records it as
one automation pass and one undo step. A `set` outside an open gesture is a
complete one-shot edit, bracketed by the port.

`StateStoreParamPort` writes through `StateStore::set_value` and opens gestures
with the store's shared leases (`acquire_gesture` / `release_gesture`). The
format adapters already report that path to the host
(`HostParameterEditBridge`), so a host sees, for a drag to 0.75:

```
begin(gain)  value(gain, 0.75)  value(gain, 0.75)  end(gain)
```

The final value is repeated before the end, so a host that latches on end
records where the control landed.

Several editors can share one port. Overlapping gestures on one parameter open
the host gesture once and close it when the last editor lets go. Giving each
editor its own port over the same store also isolates teardown: destroying a
port, or calling `release_all()`, closes only the gestures that port opened, so
closing one window mid-drag never ends another window's gesture.

Listeners see every change, whoever made it (an editor, host automation, a
state restore), on the main thread with the plain value. Writes from the audio
thread are coalesced to their latest value.

```cpp
pulp::format::StateStoreParamPort port(processor.state());
auto subscription = port.add_listener([](pulp::state::ParamID id, float v) {
    // update the control bound to id
});
port.begin(kGain);
port.set(kGain, 0.75f);
port.end(kGain);
```

## Sizing

`Editor::size_hints()` is how an editor tells every format what the host may do
with its window. The session applies it the same way in VST3 (`canResize`,
`checkSizeConstraint`), CLAP (`can_resize`, `get_resize_hints`, `adjust_size`)
and AU v2 (the container's frame):

- **Fixed size** is the default. An editor is resizable only when both minimums
  are set and the bounds leave room (not `min == max` on both axes). An editor
  that declares only a preferred size is fixed, as a ViewBridge editor is.
- **Locked aspect.** With `aspect_ratio > 0` a proposed size snaps to the
  largest box at that ratio that fits the request, then moves inside min/max
  without leaving the ratio.
- **Free resize.** With `aspect_ratio == 0` each axis is clamped to its own
  min/max.
- `Editor::adjust_size()` runs first; the hints still hold for an editor that
  does not override it. `detail::constrain_to_hints()` is the same arithmetic as
  a pure function, so a test or a widget can check a size without a host.

A host can also set a size without asking first (VST3 `onSize` without
`checkSizeConstraint`, CLAP `set_size` without `adjust_size`). The editor then
gets the constrained size, and the adapter asks the host for that size back
(`IPlugFrame::resizeView`, `clap_host_gui::request_resize`) so the window does
not keep a margin the editor never paints. A fixed-size editor takes no
unsolicited host size in any format: it is told nothing, and the host is asked for the editor's
own size. The macOS standalone window holds a live drag to the same
limits and aspect, and opens inside the screen's usable frame
(`detail::fit_size_to_area`).

AU v2 has no size negotiation, so a host that resizes the container by hand goes
through the same call: during a drag the frame stays at the host's size and the
editor gets the constrained size at once, and the frame moves there later
through the debounced request. A fixed-size editor's frame returns to
the editor's size and the editor is told nothing.

### Editor-origin resize acknowledgments

A fixed-size editor still may request a new layout through
`EditorContext::request_resize`. Fixed sizing prevents unsolicited host resizes;
it does not discard the host's acknowledgment of an editor request. The session
recognizes that acknowledgment whether it arrives inside the request hook or
later. A new editor request supersedes any pending debounced host correction.
The standalone host delivers the accepted size to the editor before moving the
window, returns failure if the editor refuses it, and suppresses the resulting
native frame echo.

## Keys

Hosts offer keys synchronously before acting on them (VST3 `onKeyDown`, the
macOS responder chain under AU). `Editor::offer_key()` returns `consumed` only
when the editor needs the key, such as a focused text field; otherwise
`forward`, so the host keeps its shortcuts. Space with no focused field must be
forwarded, or the host loses transport.

## Lifecycle

A factory creates an editor unattached. The size members may be called before
the first `attach()`: CLAP sets the scale and asks for the size before it hands
over a parent window, and AU v3 attaches only once the host's size settles.

## Teardown

`detach()` is the orderly path: the parent window is still valid and the editor
removes what it added. `invalidate()` is the host-driven path: the window is
already gone or about to be destroyed by the host, so the editor drops every
reference without touching it and makes no further host calls.

## Native parents

`NativeParent` carries a `kind` and a handle: `cocoa_view` (`NSView*`),
`uikit_view` (`UIView*`), `win32_window` (`HWND`), `x11_window` (an X11
`Window` id in `window_id`), or `none` for an offscreen editor. A host asks
`supports_parent()` before choosing a windowing API.

## ABI

`Processor::editor_factory()` is appended at the end of `Processor`'s vtable,
as the node-ABI gate requires. Enumerations in the contract keep explicit
values and only ever gain new values at the end.
