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
and the density ratio is the backing scale. Every `px` length, not only text,
is a device pixel: a `100px` box is 100 device pixels (50 points at 2x), while
pointer positions arrive in points and are scaled to device pixels, so hit
testing agrees with layout in either unit. A `dp` length is one point
(`density_ratio()` device pixels). Size anything that must keep its physical
size across displays in `dp` (the example does this).

The density ratio needs a Vellum RmlUi SDK that exports
`NativeContext::set_density_ratio`. Without it `density_ratio_supported()` is
false, `dp` equals `px`, and a `dp` document renders at 1/scale of its intended
size on a 2x display. Configure warns about it, and every `attach()` reports it
through the diagnostic sink (standard error by default; `set_diagnostic_sink`
redirects it).

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
