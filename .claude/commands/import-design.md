Import a design from an external tool (Figma, Stitch, v0, Pencil, Claude Design, DESIGN.md, or React JSX) into this Pulp project.

Detect which design source the user wants by checking:
1. If a Figma MCP server is available (com.figma.mcp), offer to read the current file/selection
2. If Stitch MCP is available (mcp__stitch__*), offer to list projects and get screens
3. If Pencil MCP is available (mcp__pencil__*), offer to read the current editor state
4. If the user mentions Claude Design or hands over a manually-exported HTML/zip from Anthropic Labs' Claude Design tool, treat as `--from claude` (no MCP — manual export only, see pulp #468)
5. If the user provides DESIGN.md, treat it as `--from designmd`
6. If the user provides a precompiled JSX runtime bundle, treat it as `--from jsx`
7. If the user provides a file path or URL, use that directly
8. If none of the above, ask the user for a source and file

## Workflow

### Step 1: Identify source and input

Ask the user or detect from context:
- **Source**: figma, stitch, v0, pencil, claude, designmd, or jsx
- **Input**: file path, URL, or MCP live data (manual file only for claude; precompiled bundle only for jsx)

### Step 2: Read the design data

**Figma (MCP available)**:
- Use `com.figma.mcp` to read the current file or selection
- Extract frames, auto-layout, fills, strokes, effects, text, components

**Stitch (MCP available)**:
- Use `mcp__stitch__list_screens` to show available screens
- Use `mcp__stitch__get_screen` to read the selected screen
- Extract component tree, inline styles, design system tokens

**Pencil (MCP available)**:
- Use `mcp__pencil__get_editor_state` to check current file
- Use `mcp__pencil__batch_get` to read the node tree
- Use `mcp__pencil__get_variables` for design tokens
- Use `mcp__pencil__get_style_guide` for style references

**v0 (URL or TSX file)**:
- If URL provided, fetch the v0 generation
- If TSX file, read directly
- Extract JSX tree, Tailwind classes, shadcn/ui components

**Claude Design (manual HTML export)**:
- Anthropic Labs has no MCP / public API; the user manually exports the design (Standalone HTML / Send to Local Coding Agent) and hands you the file
- Run `pulp import-design --from claude --file <path>` — parser delegates to the Stitch HTML pipeline and tags the IR as Claude
- The CLI also writes a `bridge_handlers.cpp` scaffold next to the generated JS (see pulp #709). Open it, fill in the `add_handler()` registrations for the messages your editor will postMessage from JS, then `bridge_.attach_webview(*panel_)` (or `attach_native_runtime(...)` if hosting in a JS runtime instead of a WebView)

**DESIGN.md**:
- Run `pulp import-design --from designmd --file DESIGN.md --tokens tokens.json`
- This is tokens-only; no `ui.js` is emitted.

**React JSX runtime bundle**:
- Compile the JSX/TSX first with `tools/import-design/jsx-runtime/jsx-transform.mjs`.
- `--mode live --emit js` writes the precompiled bundle verbatim for runtime import and rejects `--validate`, `--reference`, `--diff`, and `--debug`.
- `--mode baked --emit ir-json|cpp` captures a runtime snapshot. DOM bundles are walked as DOM; live/native bundles that render through `@pulp/react` freeze the native `WidgetBridge` tree and record `snapshotSource: native-view`. Use `--snapshot-semantics accept` only when accepting dynamic APIs is intentional.

**File-based fallback**:
- Read the file and parse based on --from source type

### Step 3: Translate to Pulp code

Use the appropriate mapping document as your translation reference:
- `planning/figma-to-pulp-mapping.md` for Figma designs
- `planning/stitch-to-pulp-mapping.md` for Stitch screens
- `planning/v0-to-pulp-mapping.md` for v0 generations
- `planning/pencil-to-pulp-mapping.md` for Pencil designs

Generate Pulp web-compat JavaScript:
- Layout: `document.createElement('div')` + `el.style.flexDirection`, etc.
- Typography: `el.style.fontSize`, `el.style.fontWeight`, etc.
- Colors: `el.style.backgroundColor`, `el.style.color`, etc.
- Audio widgets detected by name: knob/dial → `createKnob()`, fader/slider → `createFader()`, meter/level/vu → `createMeter()`, xypad → `createXYPad()`, waveform → `createWaveformView()`, spectrum/analyzer → `createSpectrumView()`
- Design tokens: `theme.colors["name"] = value`, `theme.dimensions["name"] = value`

### Step 4: Write output files

- Write the primary artifact to `--output` (`ui.js`, DesignIR JSON, or baked C++)
- Extract design tokens to `tokens.json` in W3C Design Tokens format
- Report: number of elements, number of tokens, any warnings

### Step 5: Offer refinement

After generating, offer to:
- Adjust specific elements ("make the knob smaller")
- Add audio widgets ("add a meter next to the gain knob")
- Change theme tokens ("use darker background colors")
- Preview the UI if a preview tool is available

## Mapping Quick Reference

### Figma → Pulp
| Figma | Pulp |
|-------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Component | JS function |
| Fill (solid) | `backgroundColor` |
| Fill (gradient) | `background: linear-gradient(...)` |
| Stroke | `border` |
| Drop shadow | `boxShadow` |
| Corner radius | `borderRadius` |

### Stitch → Pulp
| Stitch | Pulp |
|--------|------|
| Container | `div` with flex |
| Text | `span` |
| Button | `button` |
| Input | `input` |
| Card | Panel |
| Design system colors | `theme.colors` |

### v0 → Pulp
| v0 (Tailwind) | Pulp |
|----------------|------|
| `flex flex-col` | `flexDirection: 'column'` |
| `gap-4` | `gap: '16px'` |
| `bg-slate-900` | `backgroundColor: '#0f172a'` |
| `rounded-lg` | `borderRadius: '8px'` |
| `<Button>` | `createButton()` |
| `<Slider>` | `createFader()` |

### Pencil → Pulp
| Pencil | Pulp |
|--------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Variables (COLOR) | `theme.colors` |
| Variables (FLOAT) | `theme.dimensions` |

## Automated Validation Loop

After generating Pulp code, ALWAYS validate by comparing with the source design:

1. **Screenshot the source design** via MCP:
   - Pencil: `get_screenshot(nodeId)`
   - Save to a temp file as the reference

2. **Render the generated JS** headlessly:
   ```bash
   pulp-screenshot --script generated.js --output render.png --width W --height H
   ```

3. **Compare** reference vs render:
   ```bash
   pulp import-design --from X --file input --validate --reference source.png --diff diff.png
   ```

4. **Review the diff image** — red highlights show differences

5. **Iterate if needed** — adjust the generated code and re-render until similarity is acceptable (>85%)

### Yoga Layout Rules (MUST follow)
- Every container needs explicit `height`, `min_height`, or `flex_grow`
- Labels need `min_height` (14px for normal text, 12px for small)
- Faders need `min_width >= 40px` for thumb rendering
- Meters need `min_width >= 20px` for bar visibility
- Knobs need `min_size >= 56px` for arc rendering
- Use `createCol`/`createRow` for containers (NOT `createPanel` which adds glass overlay)
- Row height = max child height; Column height = sum of child heights + gaps

## CLI Alternative

The deterministic import tool is also available:
```bash
pulp import-design --from figma --file design.json
pulp import-design --from stitch --file screen.html
pulp import-design --from v0 --file component.tsx
pulp import-design --from pencil --file design.json
pulp import-design --from claude --file design.html   # writes ui.js + tokens.json + classnames.json + bridge_handlers.cpp
pulp import-design --from designmd --file DESIGN.md --tokens tokens.json
pulp import-design --from jsx --file bundle.js --mode live --emit js --output live-ui.js
pulp import-design --from jsx --file bundle.js --mode baked --emit cpp --snapshot-semantics accept --output imported_ui.cpp

# With validation
pulp import-design --from pencil --file design.json --validate --reference source.png --diff diff.png

# Skip the bridge scaffold (claude only)
pulp import-design --from claude --file design.html --no-bridge-scaffold
```

Artifact flags:
- `--output <path>` selects the primary artifact destination.
- Built-in default is live runtime import: `--mode live --emit js`.
- `--emit {js|ir-json|cpp}` selects the artifact kind; `cpp` requires `--mode baked`.
- `--mode {live|baked}` selects the runtime model.
- `--snapshot-semantics {fail|warn|accept}` gates JSX baked snapshots with dynamic APIs.
- Persistent defaults live in `~/.pulp/config.toml`: `pulp config set import_design.default_mode live|baked` and `pulp config set import_design.default_emit js|ir-json|cpp`. If only `default_mode=baked` is set, `ir-json` is implied. `PULP_IMPORT_DESIGN_DEFAULT_MODE` and `PULP_IMPORT_DESIGN_DEFAULT_EMIT` override config for one environment/session. Direct CLI flags override the matching preference.

Artifact mental model:
- Live/runtime import: run the original app at launch.
- Baked DesignIR: save the materialized UI tree as an inspectable snapshot.
- Baked C++: compile that saved tree into native view-construction code.
- You can go live iteration -> baked IR -> baked C++; you cannot reconstruct live React from baked IR because program logic is gone.

Use `--dry-run` to preview without writing files.

## Figma-plugin lane: knob rendering

`--from figma-plugin` accepts a knob render style. Default: silver (native vector); sprite is opt-in.

```bash
# Default — silver native vector knobs (crisp at any size, no PNG bleed)
pulp import-design --from figma-plugin --file scene.pulp.json

# Pixel-exact PNG sprite-strip (matches Figma rendering exactly)
pulp import-design --from figma-plugin --file scene.pulp.json --knob-style=sprite
```

Per-node override via Figma name suffix:
- A node named `Knob/Hero@sprite` forces sprite for THAT knob only.
- A node named `Knob/Send@silver` forces silver for THAT knob only.

Lets a designer cherry-pick one hero knob to be pixel-exact while everything else uses the crisper vector path. Convention matches Figma's own `Knob/State=hover` variant syntax.

**When asking the user**: don't position sprite as a "fallback". Pixel-exact and native-vector are real choices per design. If you're not sure which the user wants, default silver and offer to re-import with `--knob-style=sprite` so they can compare. If they say a specific knob "needs to look like the Figma", suggest adding the `@sprite` suffix on that node's name in the Figma file.

See `.agents/skills/import-design/SKILL.md` § "Knob rendering — silver by default" for the full decision matrix and the failure-mode catalogue from the ELYSIUM port.

## Bridge Handler Scaffold (Claude Design)

When `--from claude` runs, the CLI also writes `bridge_handlers.cpp` (override path with `--bridge-output`). The scaffold demonstrates how to wire `pulp::view::EditorBridge` so editor JS can `postMessage` into the C++ processor. Replace the `MyPluginEditor` placeholder with your editor class, register one `add_handler("type", ...)` per message kind, and call `bridge_.attach_webview(*panel_)`.

See `docs/reference/editor-bridge.md` for the full API and the standard error vocabulary (`malformed_json`, `unknown_type`, `missing_field`, `wrong_type`, `internal_error`).
