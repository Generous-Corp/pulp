---
name: import-design
description: Import designs from Figma, Stitch, v0, Pencil, or Claude Design into Pulp web-compat JS with automated visual validation. Claude Design imports also scaffold a pulp::view::EditorBridge handler file (pulp #709). Versioned (parser-version / format-version / compat-schema-version) detection lives behind `--detect-only` and `--report-new-format` (pulp #1031).
---

# Import Design

Import a design from an external tool (Figma, Stitch, v0, Pencil, Claude Design) into this Pulp project.

Detect which design source the user wants by checking:
1. If a Figma MCP server is available (com.figma.mcp), offer to read the current file/selection
2. If Stitch MCP is available (mcp__stitch__*), offer to list projects and get screens
3. If Pencil MCP is available (mcp__pencil__*), offer to read the current editor state
4. If the user mentions Claude Design or hands over a manually-exported HTML file from Anthropic Labs' Claude Design tool, treat as `--from claude` (no MCP — Anthropic has no public API; per pulp #468, manual file export is the supported path; Spectr's editor.html mapping is the precedent)
5. If the user provides a file path or URL, use that directly
6. If none of the above, ask the user for a source and file

## Workflow

### Step 1: Identify source and input

Ask the user or detect from context:
- **Source**: figma, stitch, v0, pencil, or claude
- **Input**: file path, URL, or MCP live data (manual file only for claude)

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

**Claude Design (manual HTML export — pulp #468)**:
- Anthropic Labs has no MCP / public API. The user runs Claude Design, exports the canvas as Standalone HTML (or "Send to Local Coding Agent"), and hands you the resulting file.
- Run `pulp import-design --from claude --file <path>` — the parser delegates to the Stitch HTML pipeline and tags the IR as Claude. **This is the static path** — it sees only the loader-shell HTML wrapping the bundled React app (~9 elements: title, bundler placeholders, inline styles, the `<script>` blob).
- Add `--execute-bundle` to invoke the **native-runtime path**: Pulp parses the JSON envelope, decodes the gzip+base64 asset map, evaluates the React + React-DOM + app payloads in a headless `ScriptEngine`, then walks the materialized DOM into the `DesignIR`. Falls back to the static path on any harness failure (engine error, walker output below the 9-node loader-shell floor, JS payload too large). Use this when the user's Claude export is a real bundled-React app and they need the actual editor tree, not just the shell.
- The CLI also writes a `bridge_handlers.cpp` scaffold next to the generated JS (override path with `--bridge-output`, skip with `--no-bridge-scaffold`). The scaffold demonstrates registering `pulp::view::EditorBridge` handlers and attaching to a `WebViewPanel` (or future `JsRuntime`).

**Inline `<script>` evaluation in `--execute-bundle` (pulp #758)**: The harness now evaluates inline `<script type="text/javascript">` (and untyped `<script>`) blocks AFTER the src-loaded payloads, then compiles + evaluates inline `<script type="text/babel">` (and `text/jsx`) blocks via the bundle's own Babel-standalone (looked up as `globalThis.Babel.transform`). After both, the harness dispatches a `readystatechange` → `DOMContentLoaded` → `readystatechange(complete)` → `window.load` sequence and pumps four message-loop / frame-callback cycles for async settling. This is what makes a real Spectr-style Claude bundle (where the actual React app lives in inline `text/babel` blocks, not src-loaded payloads) materialize beyond the 9-element shell. Per-script soft-fail matches the existing src-loaded payload pattern. Inline `application/json` (and other `*/json`) blocks are intentionally skipped — they're config blobs, not executable code.

**Gotchas that bit pulp #758 implementation:**
- `core/view/js/web-compat.js`'s `document` is a plain object literal (not an `Element`), so it ships **without** `addEventListener` / `dispatchEvent`. The Step 3 dispatcher constructs events defensively — uses `new Event(t)` when available, falls back to `{type, target, bubbles:false, preventDefault, stop*}` literal otherwise — but bundles that call `document.addEventListener('DOMContentLoaded', ...)` only fire if the bundle (or some library it loads) installs `addEventListener`/`dispatchEvent` on `document`/`window` first. Real React-DOM does not do this — it attaches to the `root` element it controls — so the DCL dispatch is a best-effort safety net, not a guarantee. See test `DOMContentLoaded dispatch runs the queued handler when document supports it` in `test/test_design_import_inline_babel.cpp` for the exact shim shape that satisfies the contract.
- The harness's `error_out` is the **fallback reason** (or empty on success). Don't piggyback diagnostic warnings on it from inside the harness; on success the harness clears the slot. If you need to surface a warning that survives a successful run, push it through a different channel (e.g. add a `warnings` vector to `ClaudeRuntimeOptions`).
- Empty `<span>` (and other text-mapped tags: `p`, `label`, `h1`-`h6`, `a`, `strong`, `em`, `small`, `code`) get filtered out by the text-empty pruning in `json_to_ir_node`. If a fixture or test relies on observing an empty `<span>` with attributes round-tripping through the IR, use a `<div>` instead — divs map to `frame` and survive the prune.
- Babel-standalone's loaded test: probe `typeof globalThis.Babel.transform === 'function'` rather than `typeof Babel`. Some bundles install `Babel` as a sentinel object before the real implementation arrives, which would false-positive on the looser check.

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

- Write the generated JS to `ui.js` (or user-specified output path)
- Extract design tokens to `tokens.json` in W3C Design Tokens format
- Report: number of elements, number of tokens, any warnings

### Step 5: Offer refinement

After generating, offer to:
- Adjust specific elements ("make the knob smaller")
- Add audio widgets ("add a meter next to the gain knob")
- Change theme tokens ("use darker background colors")
- Preview the UI if a preview tool is available

For interactive review of the current checkout, prefer:

```bash
pulp design
```

If the design tool lives in a nonstandard worktree/build setup, use:

```bash
pulp design --build-dir /path/to/build --script /path/to/design-tool.js
```

When run outside a Pulp checkout, automatic binding currently only works when the `pulp` binary
itself lives inside a Pulp build tree. In generic PATH-installed or split repo/SDK layouts, pass
`--build-dir` and `--script` explicitly.

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
pulp import-design --from claude --file design.html   # writes ui.js + tokens.json + classnames.json + bridge_handlers.cpp (static parser — loader-shell only)
pulp import-design --from claude --file design.html --execute-bundle   # runs the bundled React app in QuickJS, walks the materialized DOM (#468)

# With validation
pulp import-design --from pencil --file design.json --validate --reference source.png --diff diff.png

# Override or skip the bridge scaffold (claude only)
pulp import-design --from claude --file design.html --bridge-output editor/handlers.cpp
pulp import-design --from claude --file design.html --no-bridge-scaffold

# Override or skip the classnames artifact (claude only — pulp #1035)
pulp import-design --from claude --file design.html --classnames editor/classnames.json
pulp import-design --from claude --file design.html --no-emit-classnames
```

Use `--dry-run` to preview without writing files.

## Bridge Handler Scaffold (Claude Design only)

For `--from claude`, the CLI emits a starter C++ file demonstrating how to wire `pulp::view::EditorBridge` so the imported design's editor JS can `postMessage` into the C++ processor:

- Replace the `MyPluginEditor` placeholder with the editor class that owns the `WebViewPanel`.
- Register one `bridge_.add_handler("type", ...)` per message type your editor emits. Use `EditorBridge::get_float / get_uint / get_string` for safe payload reads, and `EditorBridge::ok_response() / ok_response(extras) / err_response(msg)` for replies.
- Call `bridge_.attach_webview(*panel_)` to route WebView messages through the dispatcher.
- For pulp #468's native-JS-runtime path, swap `attach_webview(...)` for `bridge_.attach_native_runtime(runtime, "<handler_name>")` once the runtime exposes its postMessage primitive.

See `docs/reference/editor-bridge.md` for the full API and the standard envelope-level error vocabulary (`malformed_json`, `unknown_type`, `missing_field`, `wrong_type`, `internal_error`).

## Classnames Artifact (Claude Design only — pulp #1035)

For `--from claude`, the CLI also emits `classnames.json` mapping
`classname → { cssProp(camelCase): cssValue, ... }` for every plain-classname `<style>` rule it finds in the export. Mirrors the output of Spectr's `tools/extract-html-bundle/extract.mjs` so downstream consumers (`@pulp/css-adapt`, `dom-adapter`) can merge class-based styles into inline before forwarding to bridge calls — no separate Node-side extraction script needed.

What the extractor honours:

- Bundler envelopes — when `<script type="__bundler/template">` is present, the extractor walks both the loader shell *and* the unwrapped template HTML's `<style>` blocks.
- Multiple `<style>` blocks cascade: later blocks override earlier ones per-property; unrelated declarations from earlier blocks are preserved.
- Comma-separated selector lists (`.btn-primary, .btn-secondary { ... }`) emit one entry per classname with identical declarations.
- Hyphenated CSS properties are camelCased (`font-family` → `fontFamily`).

What it skips:

- `:root`, `.scheme-*` rules — those are theme-mode token overrides handled upstream as `tokens.json` artifacts.
- Pseudo-classes (`.foo:hover`), attribute selectors (`.foo[data-x]`), descendant combinators (`.foo .bar`, `.foo > .bar`) — anything that isn't a plain `.classname { ... }` rule.
- `@media` / `@keyframes` / other at-rule wrappers.
- `<style>` blocks whose first 200 chars contain `@font-face` (those carry only font-face rules, no classnames).

This skill must stay aligned with the `view-bridge` skill — `view-bridge` covers editor lifecycle (create_view, open/notify_attached/resize/close), this skill covers message dispatch over that lifecycle.

## Versioned Detection (pulp #1031)

`pulp import-design` ships a three-layer version model so the CLI surface stays stable as external tools evolve their export formats:

- **`parser-version`** — Pulp's parser implementation for a given source.
- **`format-version`** — the export shape Pulp recognises.
- **`compat-schema-version`** — the schema of `compat.json` itself.

The matrix is declared in [`compat.json`](../../../compat.json) and consumed by `pulp import-design --detect-only`. See [`docs/reference/imports/index.md`](../../../docs/reference/imports/index.md) for the full vocabulary, recognized matrix, and "add a new format-version" workflow.

### Detect-only flow

When the user hands you an unknown export, run detection first before guessing the source:

```bash
# File or directory; --detect-only prints (source, format-version,
# parser-version, match-count, confidence) and exits.
pulp import-design --detect-only --file <path>
pulp import-design --detect-only --directory <path>
```

Exit codes: `0` = match, `1` = usage error, `2` = no match.

If confidence is below 80%, the CLI emits a warning and an invitation to run `--report-new-format`:

```bash
pulp import-design --file <path> --report-new-format > stitch-2026-XX.json
```

Hand-edit the resulting JSON into a new entry under `compat.json[imports/<source>/detected-formats]`. The `notes` field is mandatory — describe the upstream change in one line.

### Adding a fixture

Every new format-version needs a fixture so the detection gate covers it:

1. `mkdir -p test/fixtures/imports/<source>/<format-version>/`
2. Drop in the smallest representative export that triggers every fingerprint clause (synthetic is fine — clauses are content-addressed, not byte-addressed).
3. Add an `expected.json` sidecar with the assertion shape from existing fixtures (`source`, `format-version`, `parser-version`, `matched-clauses`, `total-clauses`, `min-confidence-pct`, `fingerprint-kinds`).
4. Run `ctest --test-dir build -R pulp-test-cli-import-detect` to confirm the fixture loop picks up the new row.

The detector module lives at `tools/import-design/import_detect.{hpp,cpp}` and is intentionally free of `pulp::view` / `pulp::state` link deps so the test target compiles fast and the unit tests don't drag the full design-import pipeline along.
