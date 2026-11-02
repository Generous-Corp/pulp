# Editor-host contract

`pulp/format/editor_host.hpp` is how a host opens, sizes, feeds and tears down a
processor's editor without knowing which UI toolkit draws it. It is one header
in `pulp::format-core`, it names no view, canvas, render or GPU type, and it
adds nothing to a processor that does not use it.

The existing editor path (`Processor::has_editor()`, `create_view()` and
`ViewBridge`, described in [ViewBridge](view-bridge.md)) is unchanged. The
format adapters do not consume the contract yet; a processor that provides a
factory today is ready for them, and keeps working through the existing path
until they do.

## The pieces

| Type | Role |
|---|---|
| `EditorFactory` | Builds an `Editor` for `(Processor&, EditorContext&, EditorRole)`, and can `prewarm()` its caches on a background thread before the first open. A processor returns it from `Processor::editor_factory()`; null (the default) means no factory. |
| `Editor` | One editor in one native window: `attach` / `detach`, `size` / `set_size` / `size_hints` / `adjust_size`, `idle`, `visibility_changed`, `focus_changed`, `offer_key`, `parameter_highlight`, `automation`, and `invalidate`. |
| `EditorContext` | What the host gives the editor: `params()`, `visuals()`, `request_resize()`, `host_action()`, `host_kind()`. |
| `ParamPort` | Parameter info, `get`, `begin` / `set` / `end`, normalized helpers and listeners. |
| `StateStoreParamPort` | The default `ParamPort`, over the processor's `StateStore`. |
| `VisualPort` | Read access to the lock-free values and meters the audio thread publishes. |
| `EditorAutomation` | Optional `capture_png` / `inject_input` / `observe` provider, so tooling can see and drive any editor. |

Roles are `editor`, `inspector` and `settings`; several editors, of one role or
several, can be alive at once over one processor.

## Threading

Every call runs on the host's main (UI) thread, including creating and
destroying editors. `ParamPort::get()` and `ParamPort::info()` may be called
from any thread. A `VisualPort` has one reader thread, normally the editor's
frame tick; the audio thread publishes without locks.

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
