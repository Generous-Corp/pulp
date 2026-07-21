# Putting a Pulp UI in a JUCE plugin

You can give a JUCE plugin a native Pulp GPU editor while keeping your JUCE DSP
exactly as it is. The editor *is* a Pulp view; your `AudioProcessor` is
untouched. Drag a Pulp control and it writes host automation; move a parameter
and the control follows.

This page is the map. There are **three repositories** and it matters which one
you start from — they are not interchangeable.

## Which repo do I start from?

| Your situation | Start here | What it is |
|---|---|---|
| I have an **existing** JUCE plugin and want to move its UI to Pulp | **[`pulp-import-juce`](https://github.com/danielraffel/pulp-import-juce)** → `--emit hybrid-ui` | A private-beta **importer** that reads your plugin and generates a Pulp editor swap over your unchanged DSP, wiring every APVTS parameter for you. With beta access, run the current spike as `python3 spike/emit/emit.py … --emit hybrid-ui` in a `pulp-import-juce` checkout; a released `pulp import` command lands later. |
| I'm **starting a new** plugin and want a Pulp UI | **[`pulp-embed-juce-template`](https://github.com/danielraffel/pulp-embed-juce-template)** | A GitHub **template** — clone it and you have a working plugin with one knob bound end-to-end; hand-build the rest |
| I want to **understand or extend the bridge itself** | **[`pulp-embed-juce`](https://github.com/danielraffel/pulp-embed-juce)** | The **adapter library** (`PulpEmbedComponent`) that both of the above depend on |

The one thing to get straight: **the template does not import anything.** It is a
from-scratch starter for a *new* plugin. If you already have a plugin, you want
the importer, not the template. Both ultimately depend on the same adapter
library.

```
                         pulp-import-juce  (import an existing plugin)
                              \
                               \        pulp-embed-juce-template  (start a new plugin)
                                \        /
                          pulp-embed-juce   (the adapter library — PulpEmbedComponent)
                                |
                          pulp-view-embed   (the flat C ABI over Pulp's view host)
                                |
                             Pulp SDK        (find_package(Pulp))
```

## The binding contract

However you get there, the whole editor↔host binding is one string per control:

```
DesignFrameElement.param_key == "gain"   ⇄   APVTS ParameterID { "gain", 1 }
```

Tag a design control's `param_key` with your parameter ID; `PulpEmbedComponent`
binds it bidirectionally (drag writes automation with begin/set/end gestures;
host automation / preset recall pushes values back, polled at ~30 Hz). Controls
whose key matches no parameter stay visual-only — never guessed.

Dynamic and paged controls (effect racks, tab groups) work too: re-key a control
at runtime with `DesignFrameView::set_element_param_key`, and it resolves against
the host's live parameter set through the runtime host-param surface
(`has_param` / `param_display_text`) — no editor remount. Structural commands
(insert/remove/reorder a slot, load a preset) flow the other way through
`PulpEmbedComponent::onHostAction`.

## Build

Every repo consumes an **installed Pulp SDK** via `find_package(Pulp)`, plus the
adapter/ABI sources. Two ways to wire the dependencies:

- **Prebuilt** (no Pulp checkout needed) — the Pulp release ships a
  `find_package`-able developer SDK tarball (`pulp-sdk-darwin-arm64.tar.gz` and
  friends: static libs + `lib/cmake/Pulp/PulpConfig.cmake`), and
  `pulp-view-embed` / `pulp-embed-juce` come from their coordinated `v0.1.0`
  tags via FetchContent. This is what the template defaults to when no sibling
  checkouts are present.

  ```bash
  # download + unpack the SDK dev tarball, then point CMAKE_PREFIX_PATH at it
  curl -L -o pulp-sdk.tgz \
    https://github.com/Generous-Corp/pulp/releases/latest/download/pulp-sdk-darwin-arm64.tar.gz
  tar xzf pulp-sdk.tgz            # -> ./pulp-sdk/lib/cmake/Pulp/...
  cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/pulp-sdk"
  cmake --build build -j
  ```

  (The tarball is a static-lib developer SDK for building against — not a
  code-signed end-user bundle; you sign your own plugin at *your* ship step.)

- **Siblings** — local checkouts of `pulp-view-embed` and `pulp-embed-juce` next
  to your project, against an SDK you build from source. The bridge-development
  path:

  ```bash
  git clone https://github.com/Generous-Corp/pulp && cd pulp
  export SKIA_DIR=/path/to/external/skia-build
  cmake -S . -B build-gpu -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=ON \
        -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX=/path/to/pulp-sdk-install
  cmake --build build-gpu -j && cmake --install build-gpu
  # then, in your plugin (or the template):
  cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install \
        -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed
  ```

The single `-DPULP_VIEW_EMBED_DIR` flag is enough when you build a plugin (or
`pulp-embed-juce`) directly. The **template's** siblings mode wants both
adapter checkouts next to it — `../pulp-view-embed` and `../pulp-embed-juce`
(or pass `-DPULP_EMBED_JUCE_DIR` too); its `cmake/ResolvePulpEmbed.cmake`
auto-detects them and otherwise falls back to prebuilt, so a fresh clone "just
builds" either way. Coordinated versions are tracked in `pulp-view-embed`'s
`COMPAT.md` (SDK version × embed ABI × adapter tag).

## What you get, mapped to JUCE concepts

- **Editor** → a `pulp::view::DesignFrameView` (or an imported design) mounted by
  `PulpEmbedComponent` — a `juce::Component` you return from `createEditor()`.
- **APVTS parameters** → bound by `param_key`; display strings come from
  `AudioProcessorParameter::getText`.
- **`LookAndFeel`** → a CSS theme + the SDK's faithful-port primitives (hover /
  bypass-dim, SVG fragment restyle, anchored popover, drag-to-reorder container,
  paint-space control painters, transient-animation glue). See
  [widgets reference](../reference/widgets.md).
- **Resizable editor** → `PulpEmbedComponent::configureResizableEditor(editor)`
  (aspect-locked, size-on-open; the embed does the letterbox math internally).

## Faithful reference

- The adapter's `examples/synthetic-rack/` is the canonical end-to-end example:
  static + paged param binding, a host action, a resizable editor, and a headless
  self-check — in one small plugin.
- `pulp-embed-juce/docs/porting-a-juce-plugin-ui.md` is the step-by-step porting
  walkthrough (decision tree, 5-step recipe, the Debug/Release trap,
  troubleshooting).

## Structured state for importers

Importers should use the same provider-neutral shape regardless of source
framework:

- owned node records → `StateTree` nodes and children;
- array/dictionary/custom leaf payloads → `PropertyValue` Array/Object;
- primitive leaves → scalar `PropertyValue` alternatives.

This covers JUCE-style var arrays/objects, ValueTree-like record hierarchies,
and iPlug2/custom structured state without baking source-framework names into
Pulp. Node-as-PropertyValue is intentionally staged as an ownership/cycle design
choice; importer support should represent nodes with `StateTree` children today.

## iPlug2

The same model exists for iPlug2:
[`pulp-embed-iplug2`](https://github.com/danielraffel/pulp-embed-iplug2) (adapter)
and [`pulp-import-iplug`](https://github.com/danielraffel/pulp-import-iplug)
(importer).

## See also

* [Foreign-framework coexistence](foreign-framework-coexistence.md) — the
  *forward* direction: a foreign framework (JUCE, a host's own UI) hosting a
  Pulp view in-process. Covers the `PluginViewHost`-vs-`WindowHost` rule and
  the run-loop / timer risks of a shared process.
* [Coming from JUCE](coming-from-juce.md) — migrating the DSP itself to a Pulp
  `Processor` (the other direction: rewrite, don't embed).
* [Importing designs](importing-designs.md) — the design-import pipeline the
  hybrid-UI emit builds on.
