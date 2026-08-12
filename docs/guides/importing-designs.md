# Importing Designs

Pulp can import designs from external tools and translate them into web-compat JS, DesignIR, baked C++/SwiftUI, or token artifacts. Supported sources: local **Figma `.fig` files**, **Figma REST/file JSON**, the **Pulp Figma plugin**, **Google Stitch**, **v0.dev**, **Pencil/OpenPencil**, **Claude Design**, and **Google DESIGN.md** (design *system* -- tokens only, no screen).

## Quick Start

```bash
# Import a Figma export
pulp import-design --from figma --file design.json

# Inspect a local Figma .fig save file
pulp import-design --from fig --file design.fig --outline

# Import a v0.dev component
pulp import-design --from v0 --file component.tsx --output my-ui.js

# Import a Pencil design with validation
pulp import-design --from pencil --file design.json --validate --reference source.png

# Preview without writing files
pulp import-design --from pencil --file design.json --dry-run

# Export current theme as W3C Design Tokens
pulp export-tokens --tokens tokens.json
```

### Runnable HTML and Claude Design

For Claude project archives, `.dc.html` design components, standalone HTML, or
ordinary runnable HTML, pass the file directly:

```bash
pulp import-design --file design.html
```

The CLI detects the export shape and uses one path: isolated Chromium evaluates
the real DOM, CSS, fonts, canvas, SVG, and JavaScript; Pulp records a DPR-2
reference plus CSS custom-property tokens and semantic evidence; DesignIR keeps
that evaluated visual as a portable `faithful_capture`; and Skia immediately
renders it for A/B comparison. That validation is required even without
`--validate`, and its proof remains in the durable browser-capture evidence
directory. Pass `--validate` to additionally publish convenient render and diff
copies beside the requested output. Authored controls—including custom knob
artwork—remain authored pixels and are not replaced by Pulp widget skins.

The default artifact is deliberately a **pixel-exact static frame**. Semantic
evidence is captured for future reconstruction, but browser interactions do not
become live Pulp controls unless a runtime bridge is added. Extracted CSS custom
properties describe the active light / no-preference computed capture mode;
they are not presented as a complete multi-theme authored token system.
Only custom properties whose active computed value is visible on
`documentElement` or `body` are promoted; component-scoped values remain in the
captured source evidence rather than being misrepresented as global tokens.

For an executable React/Claude import, keep the accepted Chromium frame as the
initial visible DesignIR paint authority and materialize the captured app's
behavior separately:

```bash
pulp import-design --from claude --file editor.html --mode baked \
  --emit ir-json --materialized-canvas-composition \
  --browser "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --render-size 1320x860 --output editor.ir.json --validate

node tools/import-design/jsx-runtime/materialized-runtime-transform.mjs \
  --in editor.ir-browser-capture/materialized-document.json \
  --design-ir editor.ir.json --out editor-behavior.js

pulp-screenshot --script editor-behavior.js --design-ir editor.ir.json \
  --width 1320 --height 860 --scale 2 --backend skia \
  --settle-frames 64 --output editor-native.png
```

This is not a WebView or a hand-built visual approximation: native Skia draws
the hash-verified Chromium frame, while transparent native CanvasWidget targets
receive pointer input and retain the original materialized closures. The full
frame and every canvas snapshot come from the same frozen Chromium transaction.
Do not substitute executable canvas paint for the accepted pixels until that
paint independently passes the same-frame visual gate. Keep the chrome-only and
per-canvas captures as diagnostic evidence for that later handoff.

Local relative assets load from the input folder. External requests are denied
by default. If the health report identifies a reviewed CDN dependency, retry
with `--allow-browser-network`. That consent is limited to public HTTPS origins
declared by the source; loopback, private/link-local addresses, WebSockets, and
undeclared redirect origins remain blocked. Successful external response
content is hashed into capture provenance. Use `--browser <path>` for a
nonstandard Chrome/Chromium installation. `--offline` explicitly selects the
older partial static/QuickJS fallback and may lose layout or runtime content.

Browser selection is deterministic:

1. `--browser <path>`
2. `PULP_DESIGN_BROWSER=<path>`
3. `PULP_DESIGN_BROWSER_MODE=auto|managed|system`, then
   `import_design.browser` in `~/.pulp/config.toml`
4. the explicitly installed managed Chrome for Testing
5. system Chrome/Chromium

The default `auto` mode therefore behaves exactly like system discovery until
you explicitly install the managed browser:

```bash
pulp tool install chrome-for-testing
pulp tool doctor chrome-for-testing --run
pulp tool update chrome-for-testing
pulp tool uninstall chrome-for-testing

pulp config set import_design.browser auto     # managed if installed, else system
pulp config set import_design.browser system   # never use the managed copy
pulp config set import_design.browser managed  # require the managed copy
```

An import never downloads Chrome. The installer verifies a committed SHA-256
pin, extracts the complete official archive transactionally under
`$PULP_HOME/tools/chrome-for-testing/<version>/<platform>/`, and publishes the
exact selection through `current.json`. Google currently publishes no Linux
arm64 Chrome-for-Testing archive; on that host install system Chromium and use
`system` mode or set `PULP_DESIGN_BROWSER` explicitly.

Runnable sources with asynchronous initialization may expose
`globalThis.__pulpCaptureReady` as a Promise or a function returning one. Pulp
awaits it after the initial layout observation window and fails the import if
it rejects. This is optional; sources without the contract use bounded
DOM/network/compositor settling.

To import a secondary screen instead of the landing state, describe the route
with a bounded interaction plan:

```bash
pulp import-design --file prototype.html \
  --browser-interactions patch-composer.json
```

The `pulp-browser-interactions-v1` JSON plan supports `click`, `context-click`,
`type`, `wait-for`, and `wait-ms`. Use `context-click` for a real secondary-
button context-menu gesture. Prefer `wait-for` with a visible selector after a
click; strings in hidden or inert DOM are not proof that a screen rendered.
Each completed action is recorded in `interaction-report.json`; typed text is
represented only by its length, without plaintext or a per-action text hash.
Typed text still becomes rendered prototype state and can appear in the
screenshot, DOM/semantic evidence, or tokens. Never put passwords, credentials,
private drafts, or other secrets in a plan. Plans cannot execute arbitrary
JavaScript or open popup pages, and action timeouts cannot extend the
capture-wide deadline. If the final interacted state has a distinct
asynchronous completion boundary, expose `globalThis.__pulpInteractionReady`
as a Promise or one-shot function. Pulp awaits it after the final action
without invoking the initial `__pulpCaptureReady` contract again.

Chrome/Chromium and Node.js 22 are import-time tools only. Generated DesignIR,
JavaScript, and C++ artifacts do not embed or require them.

Node.js does not have to be on `PATH`. An app launched from Finder or Explorer
inherits a minimal `PATH` that excludes every common Node.js install location,
so Pulp searches `PATH` first and then the standard locations directly:
`/opt/homebrew/bin`, `/usr/local/bin`, `/usr/bin`, and the mise, nvm, fnm, and
asdf version-manager roots under your home directory, taking the first
installation that is version 22 or newer. When no installation qualifies, the
error names the Node.js versions it found or the locations it searched.

Portable image paths are resolved by the runtime that opens the artifact.
`ScriptedUiSession` anchors JavaScript assets to the generated script directory,
and native DesignIR callers pass the document directory through
`NativeMaterializeOptions::asset_base_directory`. Generated C++ exposes
`build_imported_ui(asset_base_directory)` for production plugins and apps; pass
the deployed resource directory there. Its zero-argument overload resolves
beside the generated source and is a source-tree development convenience, not
a deployment resource lookup.

For browser-backed HTML, `--dry-run` is a diagnostic preview: its printed
capture paths and absolute backing-image path are transient and disappear when
the command exits. Run without `--dry-run` to publish a portable artifact and
its verified evidence.

## How It Works

The import pipeline has three layers:

```
Local Figma .fig file --.
Figma REST/file JSON ----.
Figma plugin .pulp.zip --|
Stitch HTML / directory -|--> Normalized IR --> JS / DesignIR / baked native artifacts
Runnable HTML ------------|    (Chromium-evaluated faithful capture)
v0 / Figma Make TSX ----|
Pencil/OpenPencil JSON --'

DESIGN.md ------------------> tokens.json only
```

## Source-Specific Guides

### Pencil / OpenPencil

Pencil uses Yoga layout internally (same engine as Pulp), so layout translation is nearly 1:1:

```bash
pulp import-design --from pencil --file design.json
```

Pencil variables map directly to Pulp theme tokens.

#### Pencil Layout Precision

For highest fidelity, agent workflows can use Pencil's `snapshot_layout` MCP tool to acquire exact pixel positions and sizes for every element before writing the JSON consumed by `pulp import-design`. That data is injected into the IR as `_layoutHeight`/`_layoutWidth` attributes, which the code generator uses instead of computing heights from children (which can differ by 5-20px due to estimation).

```
Pencil MCP workflow:
  batch_get(nodeId)        → node tree (types, styles, children)
  snapshot_layout(nodeId)  → exact pixel positions (x, y, width, height)
  export_nodes(nodeId)     → reference PNG for validation
```

#### Screenshot Naming Convention

Import validation produces three files per design:
- `{design}-{source}-source.png` — original design tool export
- `{design}-{source}-render.png` — Pulp headless render
- `{design}-{source}-diff.png` — visual diff (red = differences)

Example: `pulpgain-pencil-source.png`, `pulpgain-pencil-render.png`, `pulpgain-pencil-diff.png`

### Figma

**Save a `.fig` and import it locally. It is the best lane, and it is not close.**

In Figma: **File → Save local copy…**, then:

```bash
pulp import-design --from fig --file design.fig --outline     # list frames
pulp import-design --from fig --file design.fig --frame "Plugin UI" --output ui.js
```

| | lane | use when |
|---|---|---|
| **1st** | **`--from fig`** (local save file) | **Always, if you can get the file.** No API, no quota, no rate limit, no network, reproducible forever. It also carries the MOST data (see below). |
| 2nd | Figma desktop MCP | You cannot get a `.fig` — someone else's file, or you only have view access. Metered: as low as **6 calls per _month_** on a View/Collab seat. |
| 3rd | REST / file JSON | CI, or true headless with a token. Strictest limits; the `/images` render endpoint 429s for minutes on a dense frame. |

**Why the local file wins on fidelity, not just on quota** — a `.fig` is a ZIP
that carries things the other lanes never see:

- **Vector geometry, pre-flattened.** Booleans resolved and strokes expanded into
  fillable outlines, so no vector-network evaluation is needed.
- **Glyph outlines for every text node.** Icon fonts address glyphs by LIGATURE
  ("lock" → a padlock), so without the font they render as the literal word. The
  outlines are in the file — icons render with no font installed, and text whose
  font is missing renders exactly as designed rather than re-measured with a
  substitute face.
- **Shared styles — the design's actual tokens.** Named colour/text/effect
  definitions that nodes reference. These are load-bearing: Figma caches a
  style's resolved colour on the referencing node only *sometimes*, and that
  cache is lossy (it drops paint opacity).
- **Figma's own SOLVED layout** for every node, including auto-layout children.
  This is what makes `layout_parity.py` possible — pixel-free geometry checking
  against the design's real answer.
- **`thumbnail.png` + `meta.json`** — Figma's own raster of the design plus an
  exact canvas→thumbnail transform. A free, offline reference image. It is ~0.4×,
  so it adjudicates gross colour and placement, never material detail.

Use `figma-plugin` for the Pulp Figma plugin's `.pulp.json` / `.pulp.zip`
envelope, or `figma` for raw REST/file JSON:

```bash
pulp import-design --from fig --file design.fig --outline
pulp import-design --from fig --file design.fig --frame "Plugin UI" --output ui.js
pulp import-design --from figma-plugin --file design.pulp.zip
pulp import-design --from figma --file design.json
```

Agent workflows may use Figma MCP tools to acquire source context or reference screenshots before producing those files:
- `get_design_context` — code + screenshot + design tokens in one call
- `get_screenshot` — reference PNG for validation
- `get_variable_defs` — design tokens (colors, spacing, typography)
- `get_metadata` — layer tree with IDs, names, types, positions, sizes

Raw MCP responses are acquisition data, not a separate CLI source. Translate or export them into one of the supported file shapes before importing.

> **Mind the Figma MCP quota.** The MCP read tools above are metered — as low as
> **6 calls per _month_** on a View/Collab seat or any Starter plan. For repeated
> or dense imports, prefer the **plugin export** (`--from figma-plugin`), which
> uses no MCP quota at all. When you do call the MCP, use a single
> `get_design_context` (code + screenshot + metadata together) over separate
> calls. Full seat/plan limits:
> [The Figma plugin — avoiding Figma's limits](figma-plugin.md#two-ways-to-get-a-design-into-pulp-and-which-one-avoids-figmas-limits);
> the authoritative numbers are in
> [Figma's MCP rate-limit docs](https://developers.figma.com/docs/figma-mcp-server/rate-limits-access/).

The raw Figma and Figma Make adapter lanes are tracked in the compatibility
import reference, which records the current parser status instead of relying on
a one-off issue link.

### Google Stitch

Stitch screens are imported from HTML/directory exports or translated IR JSON. Stitch MCP can be used by an agent to acquire the screen, but the CLI still consumes a file:

```bash
pulp import-design --from stitch --file screen.html
```

Useful Stitch acquisition helpers:
- `get_screen` — HTML code + screenshot
- `get_project` — design system (50+ named colors, fonts, roundness)
- `generate_screen_from_text` — AI-generate a screen from prompt

### v0.dev

The current v0 lane accepts a v0 project envelope or a single React TSX/JSX component that stays within Pulp's supported runtime-import DOM/CSS/API subset:

```bash
pulp import-design --from v0 --file component.tsx --output my-ui.js
```

- Inline styles and supported DOM tags are normalized into the runtime import surface.
- Default Tailwind, shadcn/Radix, Next.js, and custom-component-heavy exports need preprocessing into the supported subset; they are rejected rather than partially imported.
- Simple state/value evidence is captured where the parser can prove the control contract.

### Google DESIGN.md

`DESIGN.md` is Google's YAML-frontmatter + Markdown format for
describing a design *system* (colors, typography, spacing, component
recipes), not a screen. The format is Apache-2.0; the upstream spec
lives at [github.com/google-labs-code/design.md](https://github.com/google-labs-code/design.md).

```bash
pulp import-design --from designmd --file path/to/DESIGN.md
```

This produces `tokens.json` in W3C DTCG format. It does **not**
produce a `ui.js`, because DESIGN.md has no screen — there's nothing
to lay out. Use this importer when you want to bring a token system
into Pulp; pair it with a screen importer (Figma, Stitch, Pencil, v0,
Claude) when you also need a UI.

The parser handles the canonical frontmatter keys (`version`, `name`,
`description`, `omitted`, `colors`, `typography`, `rounded`, `spacing`,
`components`), resolves `{group.key}` references at parse time, and
preserves composite typography references inside `components.*`
verbatim so downstream tooling can resolve them in widget context.
It tracks the upstream format spec at tag `0.4.0`: color values may be
any valid CSS color (hex, named, `rgb()`/`hsl()`/`oklch()`/`color-mix()`,
…), token groups nest up to 20 levels (keyed on the dot-joined path),
`spacing` accepts bare numbers, and an unrecognized top-level key is
flagged with a warning rather than silently dropped. Intentional
`omitted` sections suppress their matching lint findings; typography
property typos and flattened token-name collisions are reported.

Detection is strict: filename must be `DESIGN.md`, the frontmatter
fence must be present, and the frontmatter must declare `name:` plus
at least one canonical token group. A generic Jekyll blog post with
`name:` in its frontmatter will not match.

The importer is tokens-only. `pulp design lint` and `pulp design diff`
cover DESIGN.md quality and semantic token changes. Tailwind v3/v4 export
and project-source round-tripping remain future work. See
[`reference/imports/designmd.md`](../reference/imports/designmd.md)
for the full reference.

### Claude Design

Claude Design can export project folders, `.dc.html` design components, and
standalone HTML bundles. Pass any of those HTML files directly; Pulp detects
the shape and evaluates the runnable page in isolated Chromium:

```bash
pulp import-design --file design.html --validate --screenshot-backend skia
```

This produces a pixel-exact default frame in portable DesignIR, a browser
reference image, a Skia render, a visual diff, computed CSS tokens, and
semantic evidence. Browser validation itself is automatic; `--validate` in the
example requests convenient render/diff copies beside the primary output.
`--from claude` remains accepted for compatibility but is not required.
External browser requests remain denied unless the reviewed source requires an
explicit `--allow-browser-network` retry.

When the desired Claude screen is not the landing state, add
`--browser-interactions <plan.json>` using the bounded
`pulp-browser-interactions-v1` schema described above. Do not infer success
from strings found in the HTML or DOM snapshot; require a `wait-for` action on
a selector that is actually visible.

The older static/QuickJS parser remains available through `--offline` for
diagnostics and environments without Chromium. Its optional
`classnames.json` sidecar maps plain-classname rules for that legacy path; it
is not the authoritative layout evaluator.

## Audio Widget Detection

The importer auto-detects audio-specific widgets from naming conventions in your design:

| Name contains | Pulp widget |
|---------------|-------------|
| knob, dial | `createKnob()` |
| fader, slider | `createFader()` |
| meter, level, vu | `createMeter()` |
| xypad, xy_pad | `createXYPad()` |
| waveform, oscilloscope | `createWaveformView()` |
| spectrum, analyzer | `createSpectrumView()` |

**Container detection:** Frames with child frames (like "KnobRow" containing 4 knob frames) are treated as containers, not widgets. Only leaf nodes with shape children (ellipse/rectangle + text) become audio widgets.

## Design Tokens

Design tokens are extracted during import and saved in [W3C Design Tokens](https://design-tokens.github.io/community-group/format/) format.

### Token Aliases

W3C Design Tokens support **aliases** — tokens that reference other tokens. Pulp resolves these automatically:

```json
{
  "color": {
    "$type": "color",
    "blue": { "$value": "#3B82F6" },
    "primary": { "$value": "{color.blue}" }
  }
}
```

Chained aliases are resolved up to 10 levels deep.

### Group Type Inheritance

A group can set `$type` which applies to all children:

```json
{
  "spacing": {
    "$type": "dimension",
    "sm": { "$value": "4" },
    "md": { "$value": "8" }
  }
}
```

### Composite Tokens

Typography, shadow, and border tokens are flattened to sub-properties:

```json
{
  "heading": {
    "$type": "typography",
    "$value": { "fontFamily": "Inter", "fontSize": "24", "fontWeight": "700" }
  }
}
```

Becomes: `heading.fontFamily = "Inter"`, `heading.fontSize = 24`, `heading.fontWeight = 700`.

### Math Expressions

Token values can contain simple math: `"{spacing.base} * 2"` → resolves alias then evaluates to `16`.

### Compatibility

The W3C parser handles tokens from: Tokens Studio, Specify, Figma Variables, Stitch Design Systems, Pencil Variables, and any DTCG-format tool.

## Multi-Frame Components (mode toggles / swap links)

Some components have more than one *state frame* — e.g. a keyboard with a
**typing** mode and a **piano** mode, switched by a toggle button. A
`DesignFrameView` can hold N frames and swap which one renders:

- `add_frame(svg, elements, panel…)` registers an alternate frame (frame 0 is
  the constructor's). Each frame has its own SVG, overlay elements, and panel
  crop — and its own intrinsic size.
- `set_active_frame(i)` swaps the rendered SVG **and** the view's intrinsic
  size, then invalidates layout so the host re-sizes. It releases any held
  momentary key first (no stuck notes across a swap).
- A `DesignFrameElement` of kind **`swap`** is a swap-link button: clicking its
  rect calls `set_active_frame(target_frame)`. This is how an in-design toggle
  control (the 🎹/⌨ buttons in the Musical Typing Keyboard) drives the swap.

### Capturing every state in one import

Alternate frames are a **faithful-vector** feature: only a faithful_svg node
renders them, so the capture has to come from a lane that exports one. Export
each state's sub-frame standalone, then pass one `--file` per state — the
importer merges them into a single view holding them all:

```bash
# Typing mode (Figma node 187:15) and piano mode (187:349) of one component.
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:15  --out typing.pulp.json --faithful-vector
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:349 --out piano.pulp.json  --faithful-vector

# Merge the two states into one component.
pulp import-design --from figma-plugin \
  --file typing.pulp.json --file piano.pulp.json \
  --emit cpp --mode baked --output keyboard.cpp
```

**`--file` order is the frame index.** The first `--file` is frame 0, the
second is frame 1, and so on — that index is what a swap button targets, so
reordering the flags re-points the swaps. Name the swap layer `swap 1` in the
design to target frame 1. (The trailing number in the layer name is the
target; a layer named just `swap` has no target and is reported as an error.)

A single `--file` behaves exactly as it always has: one frame, no swap wiring.

Each export's faithful SVG (a `data:image/svg+xml;base64` asset in the
`asset_manifest`) is embedded, and the merge folds the later states into the
first envelope's root as `alternate_frames`; both the C++ codegen and the
native materializer then emit one `add_frame` per entry, in order.
Re-importing a revised state is the same command on the same node — re-export,
re-merge.

> **Which lanes can capture states.** The faithful-vector REST export above and
> the Figma plugin's `Export to Pulp` both emit faithful frames, so both feed
> this merge. `--from fig` (the offline `.fig` decoder) emits a
> widget-recognition tree instead — it has no faithful mode yet — so it cannot
> capture multi-state designs. Asking it to (repeated `--frame`) is refused with
> an error rather than quietly importing one state, and the refusal names the
> REST recipe above.
>
> **Swap buttons come from the plugin lane.** The plugin reads a layer named
> `swap <n>` into a swap element. The REST exporter does not detect swaps today,
> so a REST-captured multi-state component holds all its frames but is driven by
> calling `set_active_frame(i)` from your own code (which is what
> `MusicalTypingKeyboard` does).

### A swap with no captured target is an error, not a dead button

A swap button pointing at a frame you didn't capture would render as a control
that silently does nothing. The importer refuses to let that pass quietly — it
reports the swap through the same channel as any other unresolved control:

```bash
pulp import-design --from figma-plugin --file typing.pulp.json \
  --emit cpp --mode baked --output keyboard.cpp \
  --import-report report.json --fail-on-unresolved
```

```
import report: 1 control(s), 1 conflicted, 0 low-confidence, 0 unresolved (inert)
  - 12:7  kind=swap rung=0 confidence=1 [verify-FAIL]
      conflict: swap link targets frame 1 but only 1 frame captured (valid indices 0..0)
```

`--fail-on-unresolved` exits `2`, so CI catches it. The fix is either to
capture the missing state (add its `--file`) or to correct the layer's target
number. This applies to a single-frame import too — a design whose swap was
always dead now says so. A swap that targets the frame it already sits on is
reported the same way: the button would render and do nothing.

Name the link in plain English at import time using the interaction-linking
vocabulary (swap / resize / modal / popover / navigate / open-window /
drawer); `swap` is the one used here. (`MusicalTypingKeyboard` is the
reference consumer, hand-written before multi-state capture existed.)

> Hit-rects for a standalone sub-frame are in the sub-frame's own coordinate
> space. Extract them from the node's `absoluteBoundingBox` geometry minus the
> frame origin (the export adds a uniform shadow margin — 6px for these frames),
> not by transcribing the combined-frame coordinates.

## Validation

### Automated Validation Loop

After generating Pulp code, validate by comparing with the source design:

```bash
pulp import-design --from pencil --file design.json \
  --validate --reference source.png --render-size 400x205
```

This automatically:
1. Renders generated JS headlessly
2. Compares with reference screenshot
3. Reports similarity percentage
4. Generates diff image highlighting differences

### Debug Output

```bash
pulp import-design --from pencil --file design.json --debug
```

Reports: element counts (containers/widgets/labels), token counts, timing (ms), validation results, and gaps (unmapped shapes).

## Acquisition vs Import

MCP connectors are acquisition helpers unless a source contract says otherwise. They can read a live tool, capture screenshots, or gather metadata, then an agent writes a supported file for `pulp import-design`.

| Source | Acquisition helpers | CLI input |
|--------|---------------------|-----------|
| Figma `.fig` save | Local Figma file export | `.fig` with `--from fig` |
| Figma plugin | In-tree Figma plugin or REST exporter | `.pulp.json` / `.pulp.zip` with `--from figma-plugin` |
| Figma / Figma Make | Figma MCP or REST data acquisition | Raw Figma JSON or constrained React TSX via `--from figma` |
| Stitch | Stitch MCP or directory export | HTML/directory export or translated IR via `--from stitch` |
| Pencil/OpenPencil | Pencil MCP / OpenPencil export | JSON export or translated node data via `--from pencil` |
| v0.dev | v0 MCP/project access | Project envelope or constrained React TSX via `--from v0` |

Current runtime parsers reject raw provider MCP JSON unless that source's parser explicitly documents the shape.

## CLI Reference

```
pulp import-design --from <source> [options]

Sources:
  fig           Local Figma .fig save file decoded offline
  figma         Figma REST/file JSON or constrained Figma Make React export
  figma-plugin  Pulp Figma plugin .pulp.json/.pulp.zip envelope
  stitch        Google Stitch screen HTML or translated IR file
  v0            v0.dev project envelope or constrained React TSX
  pencil        Pencil/OpenPencil JSON or translated node export
  claude    Claude Design standalone HTML export
  designmd  Google DESIGN.md (Apache-2.0) — tokens only, no ui.js

Options:
  --from <source>   Design source (required)
  --file <path>     Input file path
  --output <path>   Output JS file (default: ui.js)
  --tokens <path>   Output W3C token file (default: tokens.json)
  --dry-run         Show generated code without writing files
  --no-tokens       Skip token extraction
  --no-comments     Omit comments from generated code
  --web-compat      Use DOM API instead of native Pulp API
  --preview         Use minimal widget style for design comparison
  --validate        Render and validate layout
  --reference <png> Compare against reference screenshot
  --diff <png>      Save visual diff image
  --render-size WxH Render dimensions (default: 340x280)
  --debug           Output JSON report with metrics

pulp export-tokens [options]

Options:
  --file <path>     Input theme JSON (default: built-in dark theme)
  --tokens <path>   Output file (default: tokens.json)
  --dry-run         Print to stdout
```
