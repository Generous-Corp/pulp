# Native-surface editors (bring your own renderer)

`integrations/pulp-native` is for a plug-in editor that already has its own
renderer, such as an existing Metal engine. It gives that renderer a place to
draw in the host's window without Pulp's view layer, Skia, Dawn, Vellum or
RmlUi. It is not the default way to build an editor: for that, see
"Choosing a UI stack" in the [editor-host contract](editor-host-contract.md).

The package is on by default on macOS, Windows and Linux
(`PULP_ENABLE_PULP_NATIVE`) because it needs only system libraries, and it ships
in every desktop SDK as the `native` component (`find_package(Pulp COMPONENTS
native)`, target `Pulp::native`). Nothing in Pulp core links it.

| Platform | Surface |
|---|---|
| macOS | `MetalSurface`, `FramePump`, AppKit input, and an offscreen `GlSurface` for tests |
| Windows | `GlSurface` over a child window with a WGL context |
| Linux | `GlSurface` over a child window of the host's X11 window with a GLX context, and X11 input |

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

## OpenGL on Windows and X11

`GlSurface` (`pulp/native/gl_surface.hpp`) is the OpenGL counterpart of
`MetalSurface`: it owns a child window of the host's window and that window's GL
context, hands the context to the client and leaves drawing and buffer swaps to
it. `attach` takes an `HWND`, or an X `Window` id for a host that offers
`NativeParentKind::x11_window`. The client lists the contexts it accepts, in
order (`GlSurfaceOptions::requests`); a request the driver cannot honour falls
through to the next. `GlContext::report()` says what it got, including
`software` for a software rasterizer such as llvmpipe, which a client may prefer
to replace with its CPU renderer.

The context belongs to the client's render thread: `make_current` creates it on
first use on the thread that calls it, so the host's UI thread never binds it.
Teardown is two-phase, as for `MetalSurface`: `make_current` and `swap_buffers`
refuse, then the release handler runs on the UI thread and returns once the
client has stopped using the context, and only then is it destroyed.

### X11

The surface opens its own connection to the X server for the window and its
events, and the context uses a second one opened on the render thread, because
a plug-in cannot call `XInitThreads` before the host's first Xlib call. It
starts no thread. Something on the UI thread has to pump it:

```cpp
// From the editor's idle tick:
surface->dispatch_events();
// Or register surface->event_fd() with the host's run loop (VST3 IRunLoop
// fd handler, CLAP posix-fd-support) and call dispatch_events() when readable.
```

Until it is pumped nothing follows the host window's size and no input arrives.
`dispatch_events` never blocks.

- **Size.** The child fills the host window and follows its resizes. Each change
  bumps `geometry().generation` and emits `geometry_changed`. The drawable is
  the window's size in pixels.
- **Scale.** `GlSurfaceOptions::scale` is the scale the host passes; 0 detects it
  as `Xft.dpi` over 96 (1 when the server sets none). `set_scale` replaces it and
  emits `geometry_changed`. Positions in input events are logical points: pixels
  over the scale.
- **Input.** `set_input_handler` takes the same `InputHandler` as the Metal
  surface; pointer (with click counts, and drags that keep arriving after the
  pointer leaves the window), wheel, key, text and focus are delivered. The host
  keeps the keyboard unless the client holds it (`set_keyboard_focus`); a key the
  client declines, and a wheel event with no wheel handler, is sent on to the
  host window. Text covers ASCII and Latin-1 only, because a plug-in cannot set
  the locale an input method needs. Drops are not provided.
- **Cursor.** `set_cursor` takes the same `Cursor` set as `MetalSurface` and maps it to the X cursor font (`x11_cursor_shape`); the cursor stays until set again and is applied again on re-attach.
- **Errors.** A window that vanishes under the surface, or a context request the
  server rejects, raises an X error. The surface contains the errors of its two
  connections and leaves every other connection's errors to the handler that was
  installed before, so a host window closing first never ends the host process.

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
`VellumRmlUi::Native` and `VellumRmlUi::Metal`. The package is found as
`VellumRmlUiNative`, but its targets keep the `VellumRmlUi::` namespace. Put
both prefixes in `CMAKE_PREFIX_PATH` (the RmlUi prefix alongside the main
Vellum prefix).
Construct
`vellum::rmlui::MetalRenderInterface` with the device returned by the surface,
then pass `metal_renderer_hooks(renderer)` to `RmlUiSurface`. The hooks give
Vellum the drawable's whole viewport, its backing scale, and a clear request on
every frame. The drawable is BGRA8Unorm and the surface's device must be the
renderer's device. The `rmlui-metal-demo` example is a small plug-in using this
path; it remains unavailable when the adapter option or the Vellum Metal
component is off.

`RmlUiSurface::attach()` calls `NativeContext::warm()` after the document loads,
so textures, geometry and glyphs are ready before the first frame is requested.
The SDK is required to provide it: configure fails when it does not (see the
SDK requirement below). `warm()` renders once outside a frame, which is safe for
Vellum's Metal and CPU renderers but not for a renderer that draws on every call
(RmlUi's GL3 backend): a custom `Rml::RenderInterface` must draw nothing between
frames.

The surface context is sized in drawable pixels, the Metal frame scale is 1,
and the density ratio is the backing scale. Every `px` length, not only text,
is a device pixel: a `100px` box is 100 device pixels (50 points at 2x), while
pointer positions arrive in points and are scaled to device pixels, so hit
testing agrees with layout in either unit. A `dp` length is one point
(`density_ratio()` device pixels). Size anything that must keep its physical
size across displays in `dp` (the example does this).

The adapter needs a Vellum RmlUi SDK from Vellum commit `37777f3` on, which
defines `VELLUM_RMLUI_NATIVE_API_VERSION` and the feature bits for the density
ratio, `NativeContext::warm()` and the renderer `trim()` in `native.hpp`. The
configure checks the macros and fails with a message when the SDK is older, so
`dp` can never silently draw at 1/scale of its size. The `vellum-rmlui-sdk` pin
in `tools/deps/manifest.json` names the commit the CI lane builds.

### Hiding the editor

A host that hides an editor without detaching it (CLAP `gui_hide`, an AU view
leaving its window) reaches the editor as `visibility_changed(false)`. The
contract session then stops calling the editor's `idle()` until the editor is
shown again. It keeps draining the parameter change queue, because the audio
thread keeps writing and the queue is bounded, and showing ticks once at once.
An RmlUi editor forwards the call to `RmlUiSurface::visibility_changed()`:
frames stop at once, including one already waiting for the frame lock, RmlUi
releases its compiled geometry and textures through the renderer, and
`RendererHooks::trim` is called so the renderer can free the layer, filter and
atlas textures it caches. `metal_renderer_hooks()` fills it from
`MetalRenderInterface::trim()`. The trim waits until no frame of the surface is
being encoded or submitted and its last committed command buffer has completed
on the GPU (about a frame at most), so nothing released is still in use.
Showing resumes frames and draws one, which recompiles everything that was
released, so the first visible frame is complete. The release calls are
renderer-wide, so a renderer serves one attached surface at a time:
`attach()` fails while another attached surface uses it.

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
back to the host, so Space still reaches the transport. Repeats and releases
keep the initial press disposition, even when focus moves while the key is
held. `focus_changed(false)` blurs a focused text field and hands the
host's keyboard back; `focus_changed(true)` focuses it again, found by its
`id` (a field without one is blurred but not restored). Pointer input queued behind a frame is applied before the
decision, so a click on a field followed at once by Space types the space.

### Threading

A frame holds one process-wide lock, `rmlui_mutex()`, for RmlUi's update and
render, because RmlUi's state is shared and not thread-safe. The main thread
does not wait for it to deliver pointer or wheel input or to run `idle()`:
when a frame is in flight, pointer and wheel events are queued (consecutive
moves, and wheel deltas with the same modifiers, merge) and applied in order on
the main thread after the frame ends. If another editor holds the shared lock
when delivery runs, one coalesced delayed retry remains until the input is
applied; it needs no further frame or host idle tick. Element
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
- `pulp-test-native-gl-surface-x11` covers the X11 backend under a virtual
  server (`xvfb-run`) on a software renderer: child lifecycle and resize
  following, error containment, request fall-through, two-phase teardown, no
  leaked connections or windows, and input from synthesized X events and the X
  test extension. `pulp-test-native-x11-translate` covers the keysym, modifier
  and button tables on every platform.
- `pulp-test-rmlui-native-surface` and `pulp-test-rmlui-native-metal` cover the
  RmlUi adapter, including the first-frame preparation. They need an installed
  Vellum RmlUi SDK. The advisory `rmlui-native` lane builds that SDK from
  Vellum's `rmlui/` project at a pinned commit (Vellum publishes no release
  artifact for it yet) and runs the surface suite
  (`tools/ci/rmlui_native_lane.sh`); the Metal suite is run locally.
- `pulp-test-native-metal-probe` drives the example through the editor-host
  contract, the path every format adapter takes.
- `pulp-test-native-surface-clap`, `-vst3`, `-au-v2` and `-au-v3` open the
  probe through each format's own adapter (the CLAP contract GUI, the VST3
  contract view, the AU v2 contract view and the macOS AU v3 view controller)
  in an offscreen window. Each asserts the same lifecycle through
  `support/native_surface_format_checks.hpp`: the editor attaches at its size,
  the first frame carries the gain colour at the drawable's size, a host resize
  moves the drawable and back, and a close then reopen starts clean.
- `native-metal-probe-clap-host` and `native-metal-probe-au-host` load the
  built plug-in in-process as CLAP and AU hosts do. They install nothing, so
  they run headless.
- `native-metal-probe-symbol-census` checks the built plug-in carries no Skia,
  Dawn, Vellum, RmlUi or `pulp::view` code. Its control runs the same census on
  Pulp's view library.

macOS behaviours that shaped these tests, such as visibility events in an
inactive app, headless display links and `auval` over SSH, are in the
`view-bridge` and `auv2` agent skills.
