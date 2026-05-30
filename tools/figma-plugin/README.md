# Design for Pulp — Figma plugin

Exports Figma designs to a Pulp-native JSON schema that the Pulp importer (`pulp import-design --from figma-plugin`) consumes. Plan: [`planning/2026-05-28-pulp-figma-plugin-strategy.md`](../../planning/2026-05-28-pulp-figma-plugin-strategy.md) (in the `pulp-planning` submodule).

**Status: working exporter with Pulp library recognition.** The plugin walks the
selected frame(s), extracts geometry / layout / typography / style / tokens /
assets, recognizes Pulp library components (Knob / Fader / Meter / XYPad / Waveform / Spectrum) and emits them
as semantic audio-widget nodes, and exports a `*.pulp.json` (or `*.pulp.zip`
when assets are present) that `pulp import-design --from figma-plugin` consumes.
The exporter lives in `src/extract.ts`, `src/serialize.ts`, and
`src/library-registry.ts`, and builds to `dist/code.js`.

For full setup — both the Figma Community install path and the local-dev path,
plus publishing — see [`docs/guides/figma-plugin.md`](../../docs/guides/figma-plugin.md).

---

## Local development install

```bash
cd tools/figma-plugin
npm install
npm run gen-types     # regenerates src/types.generated.ts from schema/figma-plugin-export-v1.json
npm run build         # produces dist/code.js + dist/ui.html
npm run typecheck     # both sides
```

Then in Figma desktop (**first time only**):

1. Open any file (a **Figma Pro** workspace — local-dev plugins don't load in
   Community/free files).
2. **Plugins → Development → Import plugin from manifest…**
3. Pick `tools/figma-plugin/manifest.json` from this checkout.
4. **Plugins → Development → Design for Pulp** to launch.

Select a frame, then **Export to Pulp** to download the `*.pulp.json` (or
`*.pulp.zip` when the design has assets). Feed either straight to the importer —
`pulp import-design --from figma-plugin <file>` reads a `.pulp.zip` directly.

**Rebuild, don't re-import.** After the first manifest import, code changes are
picked up by **`npm run build` + re-running the plugin** — Figma re-reads
`dist/` fresh on every run. You only **re-import the manifest** if
`manifest.json` itself changes. Use `npm run build:watch` to rebuild on save.

---

## Layout

```
tools/figma-plugin/
├── manifest.json                       # Figma plugin manifest
├── library-manifest.json               # Pulp Figma Library version + widget keys (Phase 0)
├── package.json
├── tsconfig.json                       # stub; use per-side configs below
├── schema/
│   └── figma-plugin-export-v1.json     # SHARED SOURCE OF TRUTH (planning §7.4)
├── scripts/
│   ├── build.mjs                       # esbuild → dist/code.js + dist/ui.html
│   └── gen-types.mjs                   # schema → src/types.generated.ts
├── src/
│   ├── code.ts                         # plugin SANDBOX half (figma.* APIs, no DOM)
│   ├── code.tsconfig.json
│   ├── ui.ts                           # plugin IFRAME half (DOM, no figma.*)
│   ├── ui.tsconfig.json
│   ├── ui.html                         # iframe shell; build.mjs inlines compiled ui.js
│   ├── types.ts                        # hand-authored postMessage types
│   └── types.generated.ts              # generated from schema/ — DO NOT EDIT
└── docs/
    └── building-the-pulp-library.md    # Phase 0 spec — design the Figma library file
```

---

## Versioning relationships

Three independently versioned things:

- **Plugin version** (`package.json` + `manifest.json`'s `_pluginVersion` once Figma assigns an id) — bumped on plugin code changes.
- **Library version** (`library-manifest.json` `library_version`) — bumped when the Pulp Figma Library file gains/changes widgets.
- **Export format version** (`schema/figma-plugin-export-v1.json`) — bumped on incompatible JSON-shape changes. Currently pinned to `2026.05-figma-plugin-v1`.

The plugin embeds its `library_manifest` snapshot into every export so the Pulp importer knows what version of the library a given JSON came from.

---

## Privacy posture

- Manifest declares `"networkAccess": { "allowedDomains": ["none"] }` — the plugin can't make any network call.
- No telemetry, no analytics, no usage tracking.
- All processing happens on the user's machine.

---

## Pipeline (all landed)

- **Extract** (`src/extract.ts`) — walk the selection into an `ExtractedFigmaNode`
  tree: geometry, auto-layout, typography, fills/strokes/radius/opacity, images,
  and recursive children.
- **Recognize** (`src/library-registry.ts`) — map Pulp library component
  instances to audio-widget kinds (Knob / Fader / Meter / XYPad / Waveform / Spectrum), authoritatively by
  component key and as a fallback by name prefix.
- **Serialize** (`src/serialize.ts`) — emit the v1 JSON envelope declared in
  `schema/figma-plugin-export-v1.json`, with provenance, the library-manifest
  snapshot, tokens, an asset manifest, and diagnostics.
- **Import** — the Pulp CLI lane: `parse_figma_plugin_json` in
  `core/view/src/design_import.cpp`, reached via
  `pulp import-design --from figma-plugin`.

See the planning doc and [`docs/guides/figma-plugin.md`](../../docs/guides/figma-plugin.md)
for details.
