# Editor-host slice

One processor (a gain parameter and a peak meter) built three ways, to prove a
plug-in can take its editor from an installed Vellum SDK through
`pulp/format/editor_host.hpp` while Pulp's own `core/canvas`, `core/render`
and `core/view` stay out of the link.

| Executable | Editor | Links |
|---|---|---|
| `pulp-slice-headless` | none | `pulp::format-core` |
| `pulp-slice-vellum` | mirrored `pulp::view::Knob` from the Vellum SDK | `pulp::format-core` + `pulp-slice-vellum-ui` (a shared library built against Vellum only) |
| `pulp-slice-rmlui` | an RmlUi document through `VellumRmlUi::SoftRenderer` | `pulp::format-core` + `VellumRmlUi` |

All three link the same processor library. Each defines its own
`slice_editor_factory()`, which stands in for the per-plug-in hook that
`pulp_add_plugin(... UI <package>)` generates.

## What the tests check

- `pulp-editor-host-slice-<kind>`: renders one second of fixed stimulus. With an
  editor, it then drives the contract in-process against a real (never shown)
  NSWindow:
  - open, size before attach, then attach;
  - a knob drag reaches the host as one begin, the values, and one end;
  - capture, resize (and an undersized resize is refused);
  - Space is forwarded to the host;
  - close, which leaves no subview, no live UI and no open gesture;
  - reopen: the value survives, and a host-side set reaches the editor without
    being echoed back;
  - an editor and an inspector open together, and each follows the other's
    edits;
  - host-driven invalidation.

  It then renders again with an editor attached and ticking, and requires the
  same bytes as the first render. The RmlUi editor's drag lands on exactly 0.75,
  its frame equals the RmlUi module's golden image byte for byte, and its LED
  follows a host-side set of 0.2 within one frame.
- `pulp-editor-host-slice-audio-identical`: the three renders are
  byte-identical.
- `pulp-editor-host-slice-symbols` (`check_symbols.py`) reads the link maps and
  the binaries. No executable loads an object from Pulp's canvas, render or
  view, or defines or references a `pulp::view`, `pulp::canvas` or
  `pulp::render` name. The headless binary carries no Vellum, RmlUi, Skia or
  Dawn symbol. The Vellum-side library links nothing from Pulp's tree. No
  Objective-C class is defined twice. Each absence check is paired with a
  control that must find something.

## Running it (macOS)

```bash
# An installed Vellum SDK with the mirror and the RmlUi module:
cmake -S <vellum> -B vellum-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVELLUM_ENABLE_GPU=ON -DVELLUM_REQUIRE_GPU=ON -DVELLUM_SKIA_ARCHIVE=<locked zip> \
  -DVELLUM_ENABLE_MIRROR=ON -DVELLUM_ENABLE_RMLUI=ON -DVELLUM_BUILD_TESTS=OFF
tools/ci/governed-build.sh cmake --build vellum-build
cmake --install vellum-build --prefix <vellum-sdk>

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPULP_EDITOR_HOST_SLICE_VELLUM_PREFIX=<vellum-sdk>
tools/ci/governed-build.sh cmake --build build --target pulp-editor-host-slice
ctest --test-dir build -L editor-host-slice --output-on-failure
```

With `PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX` empty (the default), nothing here
is configured.

## Temporary shims

- Vellum's mirrored units still use `pulp::` namespaces, so the Vellum-side
  code lives in its own shared library behind a std-only boundary
  (`vellum_ui/knob_panel.hpp`), and macOS two-level namespaces keep its
  references bound to Vellum's libraries. The `pulp::` → `vellum::` rename
  removes the need for that separation.
- The installed SDK packages no foreign-window host for a mirrored view tree
  (`PluginViewHost::create` is in the unpackaged view-tooling unit). The Vellum
  editor therefore takes frames from `render_to_rgba` and shows them in a
  layer-backed NSView (`layer_view_mac.mm`), and so does the RmlUi editor.
- `PeakMeterVisualPort` stands in for the data-plane channel set.
- The RmlUi editor maps a drag on the knob's document box to a gesture itself.
  The real binding layer reads the element's box from the document.
- `knob_led.rml`, `empty.rml` and `knob_led.golden.png` are copies of the
  RmlUi module's test fixtures.
- `rmlui_vellum_renderer_main.cpp` (acceptance item 3c) draws the knob document
  through `VellumRmlUi::VellumRenderer` on an offscreen `SkiaDawnSurface` and
  requires at most 1% of pixels to differ from the soft renderer by more than 16
  in any channel; the empty document and the knob at gain 0.2 must fail that
  rule. It builds only when the installed SDK carries the `VellumRenderer`
  component; otherwise a marker test records its absence and 3c is SKIP.
