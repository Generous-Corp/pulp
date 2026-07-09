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

## How It Works

The import pipeline has three layers:

```
Local Figma .fig file --.
Figma REST/file JSON ----.
Figma plugin .pulp.zip --|
Stitch HTML / directory -|--> Normalized IR --> JS / DesignIR / baked native artifacts
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

Figma imports are file/URL based at the CLI. Use `fig` for local `.fig` save
files decoded offline, `figma-plugin` for the Pulp Figma plugin's `.pulp.json`
/ `.pulp.zip` envelope, or `figma` for raw REST/file JSON:

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
`description`, `colors`, `typography`, `rounded`, `spacing`,
`components`), resolves `{group.key}` references at parse time, and
preserves composite typography references inside `components.*`
verbatim so downstream tooling can resolve them in widget context.
It tracks the upstream format spec at tag `0.3.0`: color values may be
any valid CSS color (hex, named, `rgb()`/`hsl()`/`oklch()`/`color-mix()`,
…), token groups nest to arbitrary depth (keyed on the dot-joined path),
`spacing` accepts bare numbers, and an unrecognized top-level key is
flagged with a warning rather than silently dropped.

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

Claude Design exports are standalone HTML files with an inline bundler
script tag. Pulp detects them via the `__bundler/template` script type
and parses the loader shell:

```bash
pulp import-design --from claude --file design.html --classnames classnames.json
```

The `classnames.json` artifact maps every plain-classname `<style>`
rule to its camelCase CSS properties, for downstream merge into
inline styles. See [`reference/cli.md#import-design`](../reference/cli.md#import-design).

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

### Worked example — re-importing two mode frames

When a design stacks its states in one spec frame (to show them side-by-side),
import each state **sub-frame** standalone — they become the swap targets:

```bash
# Typing mode (Figma node 187:15) and piano mode (187:349) of one component.
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:15  --out typing.pulp.json --faithful-vector
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:349 --out piano.pulp.json  --faithful-vector
```

Each export's faithful SVG (a `data:image/svg+xml;base64` asset in the
`asset_manifest`) is embedded; the component adds both as frames and wires the
toggle's buttons as `swap` elements. Re-importing a revised frame is the same
command on the same node — re-export, re-embed, re-extract rects. Name the link
in plain English at import time using the interaction-linking vocabulary (swap /
resize / modal / popover / navigate / open-window / drawer); `swap` is the one
used here. (`MusicalTypingKeyboard` is the reference consumer.)

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
