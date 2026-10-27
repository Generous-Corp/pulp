---
breaking = false
summary = "Document the optional RmlUi editor bridge boundary"
---

# Optional RmlUi editor bridge

## Status

This document records the production boundary for importing an existing
RmlUi/RCSS/Lua editor into a JUCE-free Pulp plugin. The current Pulp tree has
an opt-in, core-only RmlUi dependency and no Lua runtime dependency. Existing scripted editors use
`ScriptedUiSession`, QuickJS, `WidgetBridge`, and the Skia/Dawn view host. The
Nodal importer currently proves the unchanged RmlUi product behavior in a
separate product-backed probe; that probe is not the installed AU/CLAP editor.

The admitted core slice is pinned to RmlUi commit
`58c751531f4c1a1eb2dc56f409b8e8a323a0c9ad`, with samples, debugger, Lua
bindings, font engines, SVG/Lottie, HarfBuzz sample, and Tracy disabled. The
`pulp-test-rmlui-backend` suite now proves core initialization, document
parsing, update, render, and teardown through the optional backend. Its RmlUi
render adapter lowers compiled geometry to bounded Pulp draw commands, and its
file adapter resolves document resources through the rooted provider. This is
a CPU recording seam, not yet a Skia/Dawn production renderer.
The optional backend also exposes a rooted, SHA-256-checked asset provider;
its tests cover traversal, missing-manifest entries, tampering, size limits,
and symlink escapes.
This is dependency and lifecycle evidence only; it is not production editor
or unchanged-skin support.

## Design boundary

RmlUi must be an optional editor backend. It must not be linked into
`pulp-view-core`, the format adapters, or every plugin by default. A plugin
that opts in gets a separate `pulp-view-rmlui` target. A plugin that does not
opt in keeps the existing native and JavaScript scripted paths byte-for-byte
unchanged.

Admission is a repository change, not a CMake URL: RmlUi, the selected Lua
runtime or binding, and every transitive dependency need an exact upstream URL,
commit or release, archive/tree SHA-256, license, notice text, and export
policy in `tools/deps/manifest.json`, `DEPENDENCIES.md`, and `NOTICE.md`.
`tools/deps/audit.py --strict --check-upstream` and a default-off configure
test must prove that the new target is absent from the ordinary view/format
closure. No dependency is admitted until those records exist.

The first admission chooses the MIT RmlUi core explicitly. Lua and Pulp font/render
providers remain later admissions with their own manifests, licenses, notices,
hashes, and export decisions. In particular, FreeType, rlottie, LunaSVG,
HarfBuzz, and LuaJIT are not transitive freebies. They are either disabled or
receive their own admission before configure can enable them.

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

The format adapters continue to use `ViewBridge`, but the RmlUi session must
plug into the existing scripted-editor and GPU-host seams rather than adding a
parallel lifecycle. `ViewBridge` owns open/attach/resize/close ordering,
`notify_attached()` is sent only after the host parent exists, and editor
teardown releases open gestures before the RmlUi context and Lua state are
destroyed. A dependency-graph test must reject any edge from the default
`pulp-view` or format targets to `pulp-view-rmlui`.

Lua is capability-restricted: no `io`, `os`, `package`, `debug`, filesystem,
network, process, or native-pointer access. Every callback has an instruction
and memory quota, cancellation and time budget, and a documented UI-thread
reentrancy/lifetime rule. Denied capabilities, quota exhaustion, cancellation,
and callbacks after teardown are typed test failures, not warnings.

The asset provider admits only an embedded or signed asset closure with a
hash manifest. It rejects traversal, symlink escapes, ambiguous Unicode or
case aliases, TOCTOU changes, oversized archives, and assets outside the
declared root. Installed-consumer tests must resolve the same closure without
reading the developer checkout.

## Implementation slices

1. **Backend contract.** Add `pulp-view-rmlui` interfaces and a headless test
   double for render, file, font, input, Lua, and action boundaries. No host or
   default target changes. **Complete for the Pulp-owned seam.**
2. **CPU render proof.** Pin RmlUi and Lua, implement the Canvas lowering, and
   prove deterministic frame output and context isolation without a DAW. **RmlUi
   core admission, lifecycle smoke, rooted file loading, and a geometry-recording
   adapter are complete; Skia/Dawn lowering, Lua, and font providers remain open.**
3. **Product adapter.** Add a generated/imported Nodal model namespace and
   action bridge. Existing RML/RCSS/Lua files remain external or embedded by
   the importer; no hand-edited skin coordinates are accepted.
4. **GPU and host proof.** Attach the backend through `ViewBridge` and
   `PluginViewHost`, then validate the exact Forge host tuples: AUv2 in Logic,
   and VST3 plus CLAP in REAPER. Each tuple gets its own binary, scan,
   lifecycle, pointer, automation, playback, state-recall, browser, and audio
   receipt. Logic is not used as the CLAP host, and REAPER is not used as the
   AUv2 host substitute.
5. **Packaging.** Add dependency notices, the named ROM-free payload scanner,
   strict signatures, notarization, and installed-consumer receipts. Repeat on
   Windows and Linux only after the macOS row is complete.

## Promotion gates

The RmlUi path is not promoted from a parser or screenshot alone. A product
row needs exact source and dependency hashes, unchanged asset/font resolution,
Lua callback evidence, deterministic render and hit-region checks, parameter
and action readback, AU/CLAP discovery, real host interaction, automation and
state round-trip, and audio evidence. Missing Lua APIs or assets fail closed
with a typed residual. Render coverage must exercise transforms, clipping,
scissor, opacity, blend modes, textures, text, DPI, and unsupported custom
geometry; each unsupported operation is an explicit residual. The native
editor remains the rollback path until every gate passes, and a test must
exercise a failed RmlUi load or backend initialization followed by successful
native editor recovery.

## Nodal handoff

The importer-side plan and product probe are authoritative for Nodal behavior
at importer head `b1c5937b02bdc10bf06daa604eb4ae2887d8e179` in branch
`codex/nodal-host-probe-20261022`. That worktree currently proves the
unchanged Trancy skin in the product-backed probe, while the installed package
still uses the native Pulp editor. This Pulp document is the integration
contract for the next slice; it does not claim that production RmlUi support
has landed.
