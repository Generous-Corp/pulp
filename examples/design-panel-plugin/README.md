# design-panel-plugin

A plugin whose editor **is** an imported design, built for every format.

The shape is deliberately the one Forge's C++ export emits: the DesignIR
travels *with* the plugin as an embedded string, and `create_view()` parses it
and materializes it natively. A takeaway plugin has no importer, no browser and
no `ui.js` around it at runtime, so anything the editor needs has to be in the
binary.

This exists so that path is exercised by a real multi-format build rather than
only by the importer's own harness — the substitution that lets an importer
look correct while the shipped plugin draws something else.

## Build it with your own panel

The IR is supplied per checkout (it is a project artifact, often large, and
belongs to whoever generated it). Without one, this example configures itself
out — no error.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_DESIGN_PANEL_IR=/path/to/build.design.pulp.json
cmake --build build --target PulpDesignPanel_CLAP -j8
```

Formats: `VST3`, `CLAP`, `AU`, `AUv3`, `Standalone` (whichever SDKs are present).

## See what the editor draws, headlessly

Both of these call `Processor::create_view()` — the same call every format
adapter makes — and neither opens a window or an audio device:

```
build/examples/design-panel-plugin/pulp-design-panel-render-editor \
  --output editor.png --width 900 --height 602 --scale 2

build/examples/design-panel-plugin/PulpDesignPanel.app/Contents/MacOS/PulpDesignPanel \
  --screenshot editor.png --width 900 --height 602 --scale 2
```

`--screenshot` returns before the audio engine starts. A screenshot mode that
started it would make every headless verification audible on whatever machine
ran it, which is precisely where nobody expects sound.

A third renderer goes through `ViewBridge` — the editor-lifecycle layer CLAP,
VST3, AU and AUv3 **all** use to obtain an editor — rather than calling
`create_view()` directly:

```
build/examples/design-panel-plugin/pulp-design-panel-render-adapter-editor \
  --output editor.png --width 900 --height 602 --scale 2
```

This is the one that answers "does the adapter draw the DesignIR, or a scripted
`ui.js`?". `ViewBridge` reaches its scripted-UI branch only when a processor
supplies neither a native `create_view()` nor a tree, so a regression that lands
BETWEEN the adapter and `create_view()` fails here and passes the two above.
`open()` is not followed by `notify_attached()`: attachment is the host's
native-window step, and the editor's content is fully built without it.

`create_view()` returns `nullptr` rather than falling back to a generated UI
when the embedded document is empty: a dropped design must not be able to look
like a design choice.
