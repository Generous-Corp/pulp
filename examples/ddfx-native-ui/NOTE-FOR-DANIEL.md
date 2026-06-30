# Native-view embed — what we're building & the blocker

**From:** Matthew (Dream Date Designs) — **Date:** 2026-06-30

Hi Daniel — pushing this to our fork so you can see the work and weigh in on
one SDK gap that's blocking us. TL;DR at the bottom if you're short on time.

---

## What we're building

A second version of our shipping JUCE plugin **Dream Date FX** where **Pulp owns
the graphics and JUCE/YSS stays in the backend**:

- **Backend = unchanged JUCE/YSS:** the real `yss::effects::EffectChain` DSP, and
  `yss::state::StateController` / `YsspReader` for state + presets. This gives us
  bit-identical sound and lets v2 load both **DAW host state** and the JUCE
  version's **`.dddp` presets** for free (same serialization).
- **Graphics = native Pulp:** a `pulp::view::DesignFrameView` subclass,
  `DdfxEditorView`, that we've built into a faithful, fully-interactive port of
  the DDFX editor. It coexists with the shipping plugin via a separate identity
  (`DDFp` / `dreamdatefx-pulp`), and the long-term goal is an identity-swap
  drop-in once it's proven.

### What the native view already does (in this folder, `src/view.cpp`)

A snapshot of our sandbox (`knob-playground`). It renders + drags **natively**
via Skia/Dawn, no JS:

- Module rack (8 slots: LFO + 6 FX + MASTER), seamless layout, section dividers.
- Advanced effect pages with the 6×2 control grid (knobs / toggles / radios /
  GR meters), input/output level faders, close/remove.
- **LFO advanced page**: Wave/Sync/Host Sync/Rate/Random control grid, the 6
  modulation **depth range-sliders** (dual half-circle thumbs, ±100%), a
  waveform visualiser; meters suppressed and remove dimmed (pinned modulator).
- iPhone-style **edit mode**: hover an icon → animated "EDIT", click → wiggle,
  click a control → **assignment popup** (port of our `EffectModuleRackParamPicker`)
  with triangle pointer, Set-Default / Swap (carousel anim), close-X.
- **Drag-to-reorder** with lift/drop easing + landing-glow settle; empty-slot
  "+" / "Add Effect" affordances; preset browser; bypass dimming.

Build/repro (as a standalone Pulp-SDK consumer):
```
cmake --build build           # in the knob-playground project
FX_OPEN=0 FX_CTRL=1 FX_FX=LFO ./build/KnobPlayground.app/Contents/MacOS/KnobPlayground \
    --screenshot /tmp/lfo.png # headless render of the LFO advanced page
```

---

## The blocker

We want this native `DdfxEditorView` to be the UI **inside the JUCE plugin**,
through the embed path (`pulp-embed-juce` → `pulp-view-embed`). It can't be, today.

**1. The embed C ABI only mounts generated designs, never a native `View`.**
`pulp-view-embed/include/pulp_view_embed.h` exposes:
- `pulp_embed_create_from_design_json(...)`        (L242)
- `pulp_embed_create_from_design_json_str(...)`     (L248)
- `pulp_embed_create_from_ui_bundle(...)`           (L268, needs `ui.js`)
- `pulp_embed_create_offscreen(...)`                (L292)

There is no `pulp_embed_create_from_view` / factory / registered-view entry. In
`pulp-view-embed/src/embed_processors.hpp`, `EmbedProcessor` (L40) and
`EmbedScriptedProcessor` (L187) are both `final` and built internally from the
JSON/bundle inputs — a foreign `pulp::view::View` subclass has no way in.
`pulp-embed-juce` follows suit: `PulpEmbedComponent` takes a `juce::File&` source
and calls only those two create fns (`src/PulpEmbedComponent.cpp:123-127`).

**2. The host-param bridge is keyed to importer-generated control keys.**
The bridge itself is great and exactly what we want — `PulpEmbedHostCallbacks`
(`pulp_view_embed.h:184-196`): `set_param/get_param/begin_gesture/end_gesture`
by `const char* key` (normalized 0..1), plus `pulp_embed_param_changed(view,key,v)`
(L486) for host→UI. On the JUCE side `PulpEmbedComponent::resolveParameterBindings`
(`PulpEmbedComponent.cpp:162-192`) matches each design control **key == APVTS
`AudioProcessorParameterWithID::paramID`** string. But those keys come from the
importer harvesting `DesignFrameElement.source_node_id`
(`embed_processors.hpp:76-94`). A hand-built native view has no ABI-exposed
key table, so even if it could be mounted, its controls couldn't reach the bridge.

**3. Our native view binds nothing yet, by design — it's waiting on this.**
`DdfxEditorView` (`src/view.cpp:642`) holds all control values as local state
(`element_value`/`set_element_value`); it never assigns `on_element_changed` /
`on_gesture_*`. The code comment (`view.cpp:3556`) flags "APVTS param binding is
the next step." `pulp/view/parameter_binding.hpp::bind_parameter()` exists but
targets stock `Knob`/`Fader`/`Toggle` widgets keyed by **integer** `state::ParamID`,
not `DesignFrameView` elements and not the string-key embed bridge.

So the two integration models are mutually exclusive right now: the embed bridge
(string-key → APVTS) can't host a native `View`, and the native-`View` host
(`pulp::format` `create_view()`) doesn't go through the string-key host bridge.

---

## What would unblock us (proposals, your call)

Either of these lets a hand-built `DesignFrameView` ride the existing host bridge:

- **(A) A "create from native view" embed entry** — e.g.
  `pulp_embed_create_from_view(desc, view_factory_fn, out_view)` (and a matching
  `PulpEmbedComponent` ctor that takes a factory instead of a `juce::File`), so the
  embed wraps our compiled `View` the way `EmbedProcessor` wraps a design tree.
- **(B) A native-view-facing host-param surface** — let `DesignFrameElement`s (or
  any control) carry a `param_key` and call the host `set_param/get_param/
  begin/end_gesture`, and receive `pulp_embed_param_changed`. Harvest those keys
  the same way `faithful_element_keys()` harvests `source_node_id`. With this our
  view binds with zero glue — each control just declares its APVTS id string.

(B) on top of (A) would be ideal: mount the native view, and have its elements
expose param keys so the string-key↔APVTS bridge "just works."

If you'd rather we go the other direction (drive everything from a DesignIR doc
the importer generates and keep interactions in `@pulp/react`), say so — but we'd
lose the hand-tuned native interactions (wiggle/assign, reorder, advanced-page
navigation) that the static import doesn't capture, which is why we built them
natively in the first place.

---

## TL;DR

We built a fully-interactive native `pulp::view::DesignFrameView` port of our
plugin editor and want it to be the UI of the JUCE plugin via `pulp-embed-juce`.
**Blocker:** the embed ABI can only mount importer-generated designs (`ui.js` /
DesignIR JSON), not a compiled native `View`, and the string-key→APVTS host-param
bridge is only wired to importer-harvested control keys. **Ask:** a way to mount
a native `View` in the embed (factory entry) and let its `DesignFrameView`
elements expose `param_key`s to the existing `PulpEmbedHostCallbacks` bridge.

Snapshot of the view is in this folder (`src/`, `CMakeLists.txt`). Living copy is
our `knob-playground` sandbox. Happy to jump on a call.
