# Importing Designs

Pulp can import designs from external tools and translate them into web-compat JS code that runs as plugin UIs. Supported sources: **Figma**, **Google Stitch**, **v0.dev**, **Pencil/OpenPencil**, **Claude Design**, and **Google DESIGN.md** (design *system* — tokens only, no screen).

## Quick Start

```bash
# Import a Figma export
pulp import-design --from figma --file design.json

# Import a v0.dev component
pulp import-design --from v0 --file component.tsx --output my-ui.js

# Import a Pencil design with validation
pulp import-design --from pencil --file design.json --validate --reference source.png

# Preview without writing files
pulp import-design --from pencil --file design.json --dry-run

# Export current theme as W3C Design Tokens
pulp export-tokens --output tokens.json
```

## How It Works

The import pipeline has three layers:

```
Figma JSON ──┐
Stitch HTML ─┤
v0 TSX ──────┤──→ Normalized IR ──→ Pulp Native JS + W3C Tokens
Pencil JSON ─┘
```

## Source-Specific Guides

### Pencil / OpenPencil

Pencil uses Yoga layout internally (same engine as Pulp), so layout translation is nearly 1:1:

```bash
pulp import-design --from pencil --file design.json
```

Pencil variables map directly to Pulp theme tokens.

#### Pencil Layout Precision

For highest fidelity, the import skill uses Pencil's `snapshot_layout` MCP tool to get exact pixel positions and sizes for every element. This data is injected into the IR as `_layoutHeight`/`_layoutWidth` attributes, which the code generator uses instead of computing heights from children (which can differ by 5-20px due to estimation).

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

Figma Design files are imported via the Figma MCP server:

```bash
pulp import-design --from figma --file design.json
```

Key Figma MCP tools used:
- `get_design_context` — code + screenshot + design tokens in one call
- `get_screenshot` — reference PNG for validation
- `get_variable_defs` — design tokens (colors, spacing, typography)
- `get_metadata` — layer tree with IDs, names, types, positions, sizes

See [GitHub issue #49](https://github.com/danielraffel/pulp/issues/49) for Figma Design + Make adapter status.

### Google Stitch

Stitch screens are imported via the Stitch MCP server:

```bash
pulp import-design --from stitch --file screen.html
```

Key Stitch MCP tools:
- `get_screen` — HTML code + screenshot
- `get_project` — design system (50+ named colors, fonts, roundness)
- `generate_screen_from_text` — AI-generate a screen from prompt

### v0.dev

v0 generates React/Tailwind which maps to Pulp:

```bash
pulp import-design --from v0 --file component.tsx --output my-ui.js
```

- Tailwind classes → inline style properties
- shadcn/ui components → Pulp widget equivalents
- `useState` → `getParam`/`setParam`

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

Detection is strict: filename must be `DESIGN.md`, the frontmatter
fence must be present, and the frontmatter must declare `name:` plus
at least one canonical token group. A generic Jekyll blog post with
`name:` in its frontmatter will not match.

Phase 1 is tokens-only. Phase 2 adds `pulp design lint` / `pulp
design diff` and Tailwind v3 + v4 export. Phase 3 makes DESIGN.md a
round-trippable project source of truth. See
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

## WIP MCP Integration Status

| Source | MCP Connected | Design Created | Imported | Rendered | Validated | Parity |
|--------|:---:|:---:|:---:|:---:|:---:|:---:|
| **Pencil** (PulpGain) | ✓ | ✓ | ✓ | ✓ | ✓ | **96%** |
| **Pencil** (PulpEQ) | ✓ | ✓ | ✓ | ✓ | ✓ | **96%** |
| **Stitch** (PulpDelay) | ✓ | ✓ | ✓ | ✓ | N/A* | Layout match |
| **Figma Make** | ✓ | Source read ✓ | [#49](https://github.com/danielraffel/pulp/issues/49) | — | — | — |
| **Figma Design** | ✓ (auth) | Screenshot ✓ | [#49](https://github.com/danielraffel/pulp/issues/49) | — | — | — |

*\*Stitch validation compares plugin render vs app thumbnail (different resolution/chrome) — not comparable.*

Screenshot files in `build/design-debug/`:
- `pulpgain-pencil-source.png`, `pulpgain-import-pencil-render.png`, `pulpgain-import-pencil-diff.png`
- `pulpeq-pencil-source.png`, `pulpeq-import-pencil-render.png`, `pulpeq-import-pencil-diff.png`
- `pulpdelay-stitch-source.png`, `pulpdelay-stitch-render.png`, `pulpdelay-stitch-diff.png`

## CLI Reference

```
pulp import-design --from <source> [options]

Sources:
  figma     Figma export JSON or MCP data
  stitch    Google Stitch screen HTML or MCP data
  v0        v0.dev TSX/Tailwind output
  pencil    Pencil/OpenPencil node JSON or .pen export
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
