# Optional RmlUi editor bridge

## Status

This document records the production boundary for importing an existing
RmlUi/RCSS/Lua editor into a JUCE-free Pulp plugin. The current Pulp tree has
no RmlUi or Lua runtime dependency. Existing scripted editors use
`ScriptedUiSession`, QuickJS, `WidgetBridge`, and the Skia/Dawn view host. The
Nodal importer currently proves the unchanged RmlUi product behavior in a
separate product-backed probe; that probe is not the installed AU/CLAP editor.

## Design boundary

RmlUi must be an optional editor backend. It must not be linked into
`pulp-view-core`, the format adapters, or every plugin by default. A plugin
that opts in gets a separate `pulp-view-rmlui` target and declares its RmlUi,
Lua, and asset licenses and exact dependency pins. A plugin that does not opt
in keeps the existing native and JavaScript scripted paths byte-for-byte
unchanged.

The backend owns one `Rml::Context` per editor instance and implements:

* `Rml::RenderInterface` by lowering draw commands to Pulp Canvas/Skia/Dawn;
* `Rml::SystemInterface`, `FileInterface`, and `FontEngineInterface` through
  the plugin asset provider, with normalized roots and fail-closed missing-file
  diagnostics;
* RmlUi input, wheel, focus, resize, and frame-clock events from
  `PluginViewHost` on the UI thread;
* Lua document loading and callbacks inside a bounded per-editor Lua state;
* parameter, gesture, browser, and product actions through the existing
  `HostParamSurface` and `HostActionSurface` contracts.

The format adapters remain unchanged. `ViewBridge` continues to own
open/attach/resize/close ordering, `notify_attached()` is sent only after the
host parent exists, and editor teardown releases open gestures before the RmlUi
context and Lua state are destroyed.

## Implementation slices

1. **Backend contract.** Add `pulp-view-rmlui` interfaces and a headless test
   double for render, file, font, input, Lua, and action boundaries. No host or
   default target changes.
2. **CPU render proof.** Pin RmlUi and Lua, implement the Canvas lowering, and
   prove deterministic frame output and context isolation without a DAW.
3. **Product adapter.** Add a generated/imported Nodal model namespace and
   action bridge. Existing RML/RCSS/Lua files remain external or embedded by
   the importer; no hand-edited skin coordinates are accepted.
4. **GPU and host proof.** Attach the backend through `ViewBridge` and
   `PluginViewHost`, then validate arm64 AUv2 and CLAP in REAPER and Logic with
   pointer gestures, automation, playback, state recall, browser navigation,
   and nonzero audio.
5. **Packaging.** Add dependency notices, ROM-free payload scanning, strict
   signatures, notarization, and installed-consumer receipts. Repeat on Windows
   and Linux only after the macOS row is complete.

## Promotion gates

The RmlUi path is not promoted from a parser or screenshot alone. A product
row needs exact source and dependency hashes, unchanged asset/font resolution,
Lua callback evidence, deterministic render and hit-region checks, parameter
and action readback, AU/CLAP discovery, real host interaction, automation and
state round-trip, and audio evidence. Missing Lua APIs or assets fail closed
with a typed residual. The native editor remains the rollback path until every
gate passes.

## Nodal handoff

The importer-side plan and product probe are authoritative for Nodal behavior:
`RMLUI_LUA_COMPAT_PLAN.md` in `pulp-import-juce-nodal-current-20261022`.
That worktree currently proves the unchanged Trancy skin in the product-backed
probe, while the installed package still uses the native Pulp editor. This
Pulp document is the integration contract for the next slice; it does not
claim that production RmlUi support has landed.
