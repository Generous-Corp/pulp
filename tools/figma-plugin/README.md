# Design for Pulp — Figma plugin

Exports Figma designs to a Pulp-native JSON schema that the Pulp importer (`pulp import-design --from figma-plugin`) consumes. Plan: [`planning/2026-05-28-pulp-figma-plugin-strategy.md`](../../planning/2026-05-28-pulp-figma-plugin-strategy.md) (in the `pulp-planning` submodule).

Phase 1 status: **scaffold only.** The plugin loads in Figma, shows its UI, reports your current selection back. No extraction or export yet — Phase 2a adds the walker.

---

## Local development install

```bash
cd tools/figma-plugin
npm install
npm run gen-types     # regenerates src/types.generated.ts from schema/figma-plugin-export-v1.json
npm run build         # produces dist/code.js + dist/ui.html
npm run typecheck     # both sides
```

Then in Figma desktop:

1. Open any file.
2. **Plugins → Development → Import plugin from manifest…**
3. Pick `tools/figma-plugin/manifest.json` from this checkout.
4. **Plugins → Development → Design for Pulp** to launch.

Select a frame in your Figma canvas and click "Refresh selection" — the plugin echoes its name and type back. That's the Phase 1 milestone.

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

## Next phases

- **Phase 2a** — Figma extractor: walk selection, build the `ExtractedFigmaNode` model in memory.
- **Phase 2b** — Serialize the model into the v1 JSON schema.
- **Phase 3** — Recognize Pulp Library components and emit them as widget nodes (knob/fader/meter/…) instead of generic frames.
- **Phase 4** — Pulp CLI lane: `parse_figma_plugin_json` parser in `core/view/src/`, `--from figma-plugin` dispatch in the CLI.

See the planning doc for details.
