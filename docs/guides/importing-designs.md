# Importing Designs

Pulp can import designs from external tools and translate them into web-compat JS code that runs as plugin UIs. Supported sources: **Figma**, **Google Stitch**, **v0.dev**, and **Pencil/OpenPencil**.

## Quick Start

```bash
# Import a Figma export
pulp import-design --from figma --file design.json

# Import from a Figma URL (fetches via curl)
pulp import-design --from figma --url 'https://figma.com/design/...' --frame 'Plugin UI'

# Import a v0.dev component by URL
pulp import-design --from v0 --url 'https://v0.dev/t/abc123'

# Import a Stitch screen
pulp import-design --from stitch --file screen.html --screen 'Main'

# Preview without writing files
pulp import-design --from pencil --file design.json --dry-run

# Export current theme as W3C Design Tokens
pulp export-tokens --output tokens.json
```

## How It Works

The import pipeline has three layers:

1. **Source adapter** — reads the design tool's output format
2. **Normalized IR** — a JSON intermediate representation that all sources map to
3. **Code generator** — produces Pulp web-compat JavaScript from the IR

```
Figma JSON ──┐
Stitch HTML ─┤
v0 TSX ──────┤──→ Normalized IR ──→ Pulp Web-Compat JS + W3C Tokens
Pencil JSON ─┘
```

## Source-Specific Guides

### Figma

Export your Figma file as JSON (via the Figma API or MCP), then import:

```bash
pulp import-design --from figma --file my-plugin-ui.json --output ui.js
```

What gets translated:
- Auto Layout → `flexDirection`, `gap`, `padding`
- Fills → `backgroundColor`, `background` (gradients)
- Strokes → `border`
- Effects → `boxShadow`, `filter`
- Text styles → `fontSize`, `fontWeight`, `color`
- Corner radius → `borderRadius`

With Claude Code, you can import live from the Figma MCP:
```
> /import-design
> "Import my Figma file's Plugin UI frame"
```

### Google Stitch

Export a Stitch screen as HTML or use the Stitch MCP:

```bash
pulp import-design --from stitch --file screen.html
```

Stitch design systems map to Pulp theme tokens automatically.

### v0.dev

Copy the generated TSX or provide a file:

```bash
pulp import-design --from v0 --file component.tsx --output ui.js
```

- Tailwind classes → inline style properties
- shadcn/ui components → Pulp widget equivalents
- `useState` → `getParam`/`setParam`

### Pencil / OpenPencil

Pencil uses Yoga layout internally (same engine as Pulp), so layout translation is nearly 1:1:

```bash
pulp import-design --from pencil --file design.json
```

Pencil variables map directly to Pulp theme tokens.

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

Name your design elements accordingly (e.g., "GainKnob", "OutputMeter") and they'll be translated to the correct Pulp audio widget.

## Design Tokens

Design tokens are extracted during import and saved in [W3C Design Tokens](https://design-tokens.github.io/community-group/format/) format:

```bash
# Import with token extraction (default)
pulp import-design --from figma --file design.json --tokens theme-tokens.json

# Export current theme as W3C tokens
pulp export-tokens --output tokens.json

# Skip token extraction
pulp import-design --from figma --file design.json --no-tokens
```

Token mapping:
- Design colors → `theme.colors["name"]`
- Spacing/sizing values → `theme.dimensions["name"]`
- Font families → `theme.strings["name"]`

### Token Aliases

W3C Design Tokens support **aliases** — tokens that reference other tokens. Pulp resolves these automatically:

```json
{
  "color": {
    "$type": "color",
    "blue": { "$value": "#3B82F6" },
    "primary": { "$value": "{color.blue}" },
    "accent": { "$value": "{color.primary}" }
  }
}
```

In this example, `color.accent` → `color.primary` → `color.blue` → `#3B82F6`. Chained aliases are resolved up to 10 levels deep.

### Group Type Inheritance

A group can set `$type` which applies to all children that don't specify their own:

```json
{
  "spacing": {
    "$type": "dimension",
    "sm": { "$value": "4" },
    "md": { "$value": "8" },
    "lg": { "$value": "16" }
  }
}
```

All three tokens inherit `"dimension"` type from the `spacing` group — no need to repeat `"$type"` on each token.

### Compatibility

The W3C parser handles tokens from:
- **Tokens Studio** (Figma plugin) exports
- **Specify** design token pipelines
- **Figma Variables** (via MCP `get_variable_defs`)
- **Stitch Design Systems** (via MCP `list_design_systems`)
- **Pencil Variables** (via MCP `get_variables`)
- Any tool exporting [DTCG format](https://design-tokens.github.io/community-group/format/)
- Font families → `theme.strings["name"]`

### Round-Trip

Tokens can flow bidirectionally:

```
External Tool → W3C Tokens → Pulp Theme → W3C Tokens → External Tool
```

In JavaScript, use the bridge functions:
```javascript
// Import W3C tokens into the current theme
importDesignTokens(w3cJsonString);

// Export current theme as W3C tokens
const w3c = exportDesignTokens();
```

### Figma Variables Sync

Figma Variables can be imported and exported bidirectionally:

```
Figma Variables (MCP get_variable_defs) → Pulp Theme → Figma Variables JSON
```

Figma uses slash-separated paths (`color/primary`) which are automatically converted to dot-separated (`color.primary`) for Pulp themes.

### Stitch Design System Sync

Stitch Design Systems map to Pulp tokens:

```
Stitch Design System (MCP list_design_systems) → Pulp Theme → Stitch JSON
```

Maps: `colors`, `fonts`, `roundness` (none/small/medium/large/full), `spacing`.

## CLI Reference

See [Design Import API Reference](../reference/design-import.md) for the full CLI and IR specification.

```
pulp import-design --from <source> [options]

Sources:
  figma     Figma export JSON or MCP data
  stitch    Google Stitch screen HTML or MCP data
  v0        v0.dev TSX/Tailwind output
  pencil    Pencil/OpenPencil node JSON or .pen export

Options:
  --from <source>   Design source (required)
  --file <path>     Input file path
  --url <url>       Design URL (Figma file URL, v0 share link)
  --frame <name>    Frame/artboard to import (Figma)
  --screen <name>   Screen to import (Stitch)
  --output <path>   Output JS file (default: ui.js)
  --tokens <path>   Output W3C token file (default: tokens.json)
  --dry-run         Show generated code without writing files
  --no-tokens       Skip token extraction
  --no-comments     Omit comments from generated code
  --web-compat      Use DOM API instead of native Pulp API
  --validate        Render and validate layout
  --reference <png> Compare against reference screenshot

pulp export-tokens [options]

Options:
  --file <path>     Input theme JSON (default: built-in dark theme)
  --tokens <path>   Output file (default: tokens.json)
  --dry-run         Print to stdout
```

## IR Format Reference

The normalized intermediate representation (IR) is a JSON format that all source adapters produce. You can also write IR directly:

```json
{
  "type": "frame",
  "name": "PluginUI",
  "layout": { "direction": "column", "gap": 16, "padding": 12 },
  "style": { "backgroundColor": "#1a1a2e", "borderRadius": 8 },
  "children": [
    {
      "type": "text",
      "name": "title",
      "content": "My Plugin",
      "style": { "fontSize": 24, "fontWeight": 700, "color": "#e0e0e0" }
    },
    {
      "type": "slider",
      "name": "GainKnob",
      "label": "Gain",
      "min": 0,
      "max": 1,
      "default": 0.75
    }
  ],
  "tokens": {
    "colors": { "bg.primary": "#1a1a2e" },
    "dimensions": { "spacing.md": 16 }
  }
}
```
