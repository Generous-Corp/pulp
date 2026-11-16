# Native-surface editors (bring your own renderer)

`integrations/pulp-native` is for a plug-in editor that already has its own
renderer, such as an existing Metal engine. It gives that renderer a place to
draw in the host's window without Pulp's view layer, Skia, Dawn, Vellum or
RmlUi. It is not the default way to build an editor: for that, see
"Choosing a UI stack" in the [editor-host contract](editor-host-contract.md).

The package is on by default on macOS (`PULP_ENABLE_PULP_NATIVE`) because it
needs only system frameworks. Nothing in Pulp core links it. macOS is the only
platform so far.

## The pieces

| Piece | Header | What it does |
|---|---|---|
| `MetalSurface` | `pulp/native/metal_surface.hpp` | A child `NSView` backed by a `CAMetalLayer`, and the system `MTLDevice` it uses. Hands out raw drawables from any thread; the client encodes and presents. |
| `FramePump` | `pulp/native/frame_pump.hpp` | An optional display-paced driver: one-shot frames, continuous frames, a rate cap and an off-screen pause, delivered on the main thread or on a render thread it owns. |
| Input | `pulp/native/input.hpp` | Neutral pointer, wheel, key and cursor types, and the translation from `NSEvent`. |
| Host extension | `pulp/format/native_surface_host.hpp` | `dev.pulp.native_surface/1`: what the host knows about the window (the scale it set, whether it shows the window), offered by every format adapter's editor session. |

An editor is a `UI` package (`pulp_add_plugin(... UI <package>)`) whose
`Editor::attach()` creates a `MetalSurface` in the parent view.
`integrations/pulp-native/examples/metal-probe` is the worked example: a gain
plug-in whose clear colour follows the gain and whose triangle follows the
pointer. A vertical drag edits the gain as one host gesture.

## Rules

- **The editor owns its GPU device.** It never also uses the shared GPU
  runtime: one editor, one device family.
- **The render thread presents, nothing more.** It may acquire and present
  drawables and read `ParamPort::get`. It never calls `EditorContext`, the host
  extension, or `ParamPort::begin` / `set` / `end`.
- **Teardown is two-phase.** `detach()` and `invalidate()` call the release
  handler on the main thread first. The handler stops the render thread before
  it returns, and only then does the view leave its parent.
- **The host keeps the keyboard** unless the client holds text focus
  (`set_text_focus(true)`), for example while a text field is being edited.
  Giving focus back restores the host's own responder.
- **Unhandled input goes on to the host.** A surface with no wheel handler
  passes wheel events to the host.

## Using another renderer

The surface takes no position on how a frame is drawn. A client can:

- use the surface's command queue, or create its own on `device()`;
- set `DrawableSizing::client` and size drawables itself;
- skip `FramePump` and pace frames from its own thread or timer, in which case
  it creates no display link at all.

An RmlUi renderer, or any other renderer that takes a `MTLDevice` and a target
texture, draws into the drawable's texture the same way the metal-probe
renderer does.

## RmlUi on Metal

The opt-in `PULP_ENABLE_RMLUI_ADAPTER` package adds
`pulp::rmlui-native-metal` when the Vellum main SDK and the optional
`vellum-rmlui-sdk-<version>-darwin-arm64` prefix export
`VellumRmlUiNative::Native` and `VellumRmlUiNative::Metal`. Put both prefixes in
`CMAKE_PREFIX_PATH` (the RmlUi prefix alongside the main Vellum prefix).
Construct
`vellum::rmlui::MetalRenderInterface` with the device returned by the surface,
then pass `metal_renderer_hooks(renderer)` to `RmlUiSurface`. The hooks give
Vellum the drawable's whole viewport, its backing scale, and a clear request on
every frame. The drawable is BGRA8Unorm and the surface's device must be the
renderer's device. The `rmlui-metal-demo` example is a small plug-in using this
path; it remains unavailable when the adapter option or the Vellum Metal
component is off.

The surface context is sized in drawable pixels, the Metal frame scale is 1,
and the density ratio is the backing scale. Documents that need
density-scaled text should use `font-size` in `dp` (the example does this)
rather than `px`.

### Keys and window focus

An RmlUi editor does not decide for itself whether a text field wants a key a
host offers before its own accelerators. `pulp/rmlui_native/editor_keys.hpp`
(with `pulp::format-core`) gives the two contract members as one call each:

```cpp
KeyDisposition offer_key(const EditorKeyEvent& e) override {
    return pulp::rmlui_native::offer_editor_key(*ui_, e);
}
void focus_changed(bool focused) override {
    pulp::rmlui_native::forward_editor_focus(*ui_, focused);
}
```

`offer_key` returns `consumed` only while a text field has focus and takes the
key: RmlUi consumed it, or it produced text, which is typed into the field. A
command or control shortcut and any key with no field focused go
back to the host, so Space still reaches the transport. A release follows its
press: it is the field's only if the field took the press, wherever focus has
moved since. `focus_changed(false)` blurs a focused text field and hands the
host's keyboard back; `focus_changed(true)` focuses it again, found by its
`id` (a field without one is blurred but not restored). Pointer input queued behind a frame is applied before the
decision, so a click on a field followed at once by Space types the space.

### Threading

A frame holds one process-wide lock, `rmlui_mutex()`, for RmlUi's update and
render, because RmlUi's state is shared and not thread-safe. The main thread
does not wait for it to deliver pointer or wheel input or to run `idle()`:
when a frame is in flight, pointer and wheel events are queued (consecutive
moves, and wheel deltas with the same modifiers, merge) and applied in order on
the main thread within about a millisecond of the frame ending. Element
listeners therefore still run on the main thread, a moment after the event.
`with_context()`, key and text input apply the queue first and wait for a
frame; call `flush_input()` where a caller needs earlier events applied now,
as a test does after a click. `offer_key()` applies the queue only when input
is queued, so with nothing pending a key offered with no field focused still
returns at once without touching RmlUi.

## Proof

- `pulp-test-native-metal-surface`, `-frame-pump`, `-input` and
  `-surface-input` cover the package on the real Metal device in offscreen
  windows. They skip, saying so, without one.
- `pulp-test-native-metal-probe` drives the example through the editor-host
  contract, the path every format adapter takes.
- `native-metal-probe-clap-host` and `native-metal-probe-au-host` load the
  built plug-in in-process as CLAP and AU hosts do. They install nothing, so
  they run headless.
- `native-metal-probe-symbol-census` checks the built plug-in carries no Skia,
  Dawn, Vellum, RmlUi or `pulp::view` code. Its control runs the same census on
  Pulp's view library.

macOS behaviours that shaped these tests, such as visibility events in an
inactive app, headless display links and `auval` over SSH, are in the
`view-bridge` and `auv2` agent skills.
