# Design Import API Reference

Technical reference for the Pulp design import pipeline. For a getting-started guide, see [Importing Designs](../guides/importing-designs.md).

## CLI Commands

### import-design

Import a design from an external tool into Pulp JS, DesignIR, or baked C++
artifacts.

```
pulp import-design --from <source> [options]
```

**Sources:** `figma`, `stitch`, `v0`, `pencil`, `claude`, `designmd`, `jsx`

| Flag | Description | Default |
|------|-------------|---------|
| `--from <source>` | Design source (required) | — |
| `--file <path>` | Input file path | — |
| `--url <url>` | Design URL (Figma file URL, v0 share link) | — |
| `--frame <name>` | Frame/artboard to import (Figma) | first frame |
| `--screen <name>` | Screen to import (Stitch) | first screen |
| `--output <path>` | Destination file for the primary artifact | `ui.js` |
| `--emit {js\|ir-json\|cpp}` | Primary artifact kind. `js`, `ir-json`, and `cpp` are implemented; `cpp` requires `--mode baked`. | `js` |
| `--mode {live\|baked}` | Runtime model. `live` is the default. `baked` emits canonical IR or baked C++ via `--emit ir-json\|cpp`. | `live` |
| `--snapshot-semantics {fail\|warn\|accept}` | JSX baked snapshot policy. `fail` rejects dynamic APIs by default, `warn` proceeds with diagnostics, and `accept` proceeds silently. | `fail` |
| `--allow-network-fetch` | Allow DesignIR asset-manifest HTTP(S) fetches at import time. | off |
| `--asset-cache <path>` | Asset cache directory for HTTP(S) imports. | `PULP_IMPORT_ASSET_CACHE` or user cache |
| `--asset-timeout-ms <ms>` | Per-request network asset timeout. | `30000` |
| `--asset-hash <uri=sha256>` | Expected content hash for an asset URI; may be repeated. | — |
| `--tokens <path>` | Output W3C token file | `tokens.json` |
| `--dry-run` | Show generated code without writing | — |
| `--no-tokens` | Skip token extraction | — |
| `--no-comments` | Omit comments from generated code | — |
| `--web-compat` | Use DOM API instead of native Pulp API | — |
| `--validate` | Render generated JS and validate layout | — |
| `--reference <png>` | Compare render against reference screenshot | — |
| `--diff <png>` | Save visual diff image | — |
| `--render-size WxH` | Render dimensions | `340x280` |
| `--preview` | Minimal widget styling for design comparison | — |
| `--debug` | Output JSON report with metrics | — |
| `--debug-output <path>` | Save debug JSON to file | stdout |
| `--detect-only` | Detect (source, format-version, parser-version) without parsing — see [imports/](imports/index.md) (pulp #1031) | — |
| `--directory <path>` | Path to directory export (alternative to `--file`) | — |
| `--compat <path>` | Override `compat.json` discovery | walk up from input |
| `--report-new-format` | Emit a fingerprint-diff JSON for a new format-version. Implies `--detect-only` | — |

Either `--file` or `--url` is required (or `--directory` for `--detect-only`). When `--url` is provided without `--file`, the URL is fetched through an argv-safe `curl` invocation into a unique temporary file. Literal `--file` paths are read directly and may contain normal filesystem punctuation; `--url` still rejects shell metacharacters before fetching.

`--emit ir-json` writes a canonical [DesignIR v1](design-ir-v1.md) envelope.
Asset collection runs before serialization. Local files and data URIs are
recorded by default; HTTP(S) asset fetches require explicit
`--allow-network-fetch` consent and are cached by content hash. For `--url`
imports, relative asset references resolve against the source URL while the
authored relative value remains in the manifest as `original_uri`.

The IR envelope also records document-level provenance (`capture_method`,
`settle_rounds`, `fallback_reason`, `source_adapter`, `source_version`,
`imported_at`) plus structured diagnostics. All source adapters return the
shared normalized form: interactive `frame` nodes are promoted through the
library normalization pass before code generation or IR serialization.

For `--from jsx --mode live --emit js`, Pulp writes the precompiled bundle
verbatim. For JSX baked IR/C++ snapshots, Pulp scans the precompiled bundle for
dynamic APIs that make a frozen snapshot non-deterministic (`setInterval`,
`setTimeout`, `requestAnimationFrame`, `Date.now`, `new Date`,
`performance.now`, `Math.random`, and `fetch`). Comments and string literals
are ignored. The default `--snapshot-semantics fail` exits with code 2. `warn`
emits the IR and records a `snapshot-dynamic-api` diagnostic; `accept` emits
the IR or C++ with provenance recording the accepted policy.

Generated baked/native artifacts can link the core view target,
`pulp::view-core`, when they only construct `View` trees and do not evaluate
JS. Live import, `ScriptEngine`, `WidgetBridge`, and scripted UI consumers
should link `pulp::view-script` or the full compatibility target,
`pulp::view`.

### export-tokens

Export a Pulp theme as W3C Design Tokens JSON.

```
pulp export-tokens [options]
```

| Flag | Description | Default |
|------|-------------|---------|
| `--file <path>` | Input theme JSON | built-in dark theme |
| `--tokens <path>` | Output file | `tokens.json` |
| `--dry-run` | Print to stdout | — |

## Intermediate Representation (IR)

All source adapters produce a normalized JSON IR before code generation. You can also write IR by hand. The canonical schema is [DesignIR v1](design-ir-v1.md); the summary below covers the common node fields.

### IRNode

```json
{
  "type": "frame|text|image|button|input|slider|knob|fader|meter",
  "name": "ElementName",
  "content": "Text content (for text nodes)",
  "style": { ... },
  "layout": { ... },
  "label": "Audio widget label",
  "min": 0, "max": 1, "default": 0.5,
  "children": [ ... ]
}
```

### IRStyle

CSS-like visual properties on each node:

| Property | Type | Description |
|----------|------|-------------|
| `backgroundColor` | string | `#hex` color |
| `backgroundGradient` | string | `linear-gradient(...)` |
| `color` | string | Text color |
| `opacity` | float | 0.0–1.0 |
| `borderRadius` | float | Corner radius in px |
| `border` | string | e.g. `"1px solid #333"` |
| `boxShadow` | string | CSS box-shadow |
| `filter` | string | e.g. `"blur(4px)"` |
| `fontFamily` | string | Font name |
| `fontSize` | float | In px |
| `fontWeight` | int | 100–900 |
| `fontStyle` | string | `normal`, `italic` |
| `textAlign` | string | `left`, `center`, `right` |
| `letterSpacing` | float | In px |
| `lineHeight` | float | Multiplier |
| `textTransform` | string | `uppercase`, etc. |
| `overflow` | string | `hidden`, `scroll`, `auto` |
| `position` | string | `absolute`, `relative` |
| `top`, `left`, `right`, `bottom` | float | Position offsets in px |
| `zIndex` | int | Stacking order |
| `transform` | string | CSS transform |
| `width`, `height` | float | In px |
| `minWidth`, `minHeight` | float | In px |
| `maxWidth`, `maxHeight` | float | In px |

### IRLayout

Flexbox layout properties for container nodes:

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `direction` | string | `"column"` | `"row"` or `"column"` |
| `gap` | float | `0` | Gap between children in px |
| `padding` | float | `0` | Uniform padding (or use per-side) |
| `paddingTop/Right/Bottom/Left` | float | `0` | Per-side padding |
| `justify` | string | `"flex-start"` | Main axis alignment |
| `align` | string | `"stretch"` | Cross axis alignment |
| `wrap` | bool | `false` | Enable flex wrap |
| `widthMode` | string | `"fixed"` | `"fixed"`, `"hug"`, `"fill"` |
| `heightMode` | string | `"fixed"` | `"fixed"`, `"hug"`, `"fill"` |

### IRTokens

Design tokens extracted from the source:

```json
{
  "tokens": {
    "colors": { "bg.primary": "#1a1a2e", "accent": "#e94560" },
    "dimensions": { "spacing.md": 16, "radius.md": 8 },
    "strings": { "font.family": "Inter" }
  }
}
```

## Audio Widget Detection

Widgets are detected from node names (case-insensitive substring match):

| Pattern | Widget Type | Pulp Function |
|---------|-------------|---------------|
| `knob`, `dial` | knob | `createKnob()` |
| `fader`, `slider` | fader | `createFader()` |
| `meter`, `level`, `vu` | meter | `createMeter()` |
| `xypad`, `xy_pad`, `xy pad` | xy_pad | `createXYPad()` |
| `waveform`, `oscilloscope` | waveform | `createWaveform()` |
| `spectrum`, `analyzer`, `analyser` | spectrum | `createSpectrum()` |

Audio widgets get additional IR properties: `label`, `min`, `max`, `default`.

## Code Generation Modes

### Native mode (default)

Uses Pulp's native widget bridge API: `createCol()`, `createRow()`, `createKnob()`, `setFlex()`, etc. Encodes Yoga layout constraints:

- Every container has explicit height or `flex_grow`
- Labels get minimum height (14px)
- Knobs: min 56x56px
- Faders: min 40px wide, 80px tall
- Meters: min 20px wide, 80px tall

### Web-compat mode (`--web-compat`)

Uses `document.createElement()` + `element.style.*` for compatibility with the web-compat JS layer.

## W3C Design Tokens

### Format

Follows the [DTCG Design Tokens](https://design-tokens.github.io/community-group/format/) specification:

```json
{
  "color": {
    "$type": "color",
    "primary": { "$value": "#89B4FA" },
    "bg": { "$value": "#1E1E2E" }
  }
}
```

### Features

- **Group `$type` inheritance** — children inherit `$type` from parent group
- **Aliases** — `{ "$value": "{color.primary}" }` with chained resolution (max 10 levels)
- **Cycle detection** — circular aliases are detected and safely terminated
- **Math expressions** — `"8 * 2"`, `"{spacing.base} * 2"` evaluated at parse time
- **Composite tokens** — `typography`, `shadow`, `border` decomposed into individual tokens
- **Unit stripping** — `px`, `rem`, `em` suffixes stripped for numeric evaluation

### Token Sync

#### Figma Variables

```cpp
// Parse Figma Variables (from MCP get_variable_defs)
Theme theme = parse_figma_variables(json);

// Export as Figma Variables format
std::string json = export_figma_variables(theme);
```

Figma uses slash-separated paths (`color/primary`) which are converted to dot-separated (`color.primary`) for Pulp themes.

#### Stitch Design Systems

```cpp
// Parse Stitch Design System (from MCP list_design_systems)
Theme theme = parse_stitch_design_system(json);

// Export as Stitch format
std::string json = export_stitch_design_system(theme);
```

Maps Stitch-specific properties: `colors`, `fonts`, `roundness` (none/small/medium/large/full), `spacing`.

## Source Adapter Details

### Figma (`parse_figma_json`)

Accepts IR-format JSON. In Claude Code flows, the Figma MCP (`get_design_context`) provides the design data, which is translated to IR format using the Figma-to-Pulp mapping rules.

### Stitch (`parse_stitch_html`)

Dual parsing: tries JSON IR first (from MCP `get_screen`), falls back to HTML tag extraction for basic structure.

### v0 (`parse_v0_tsx`)

Dual parsing: tries JSON IR first (pre-processed by AI), falls back to Tailwind className extraction (`flex-row`, `flex-col`, `gap-*`, `bg-*`).

### Pencil (`parse_pencil_json`)

Accepts IR-format JSON from MCP `batch_get`. Pencil uses Yoga layout internally, so layout translation is nearly 1:1. Frames default to horizontal (row) when layout is unspecified.

## Validation

The `--validate` flag renders the generated JS headlessly and optionally compares against a reference screenshot:

```bash
# Validate layout renders correctly
pulp import-design --from figma --file design.json --validate

# Compare against source design screenshot
pulp import-design --from figma --file design.json \
    --validate --reference source.png --diff diff.png
```

Similarity threshold: 70% (PASS) / below 70% (NEEDS REVIEW).

## Project Templates

Create a new project pre-configured for design import:

```bash
pulp create "My Plugin" --template from-figma
pulp create "My Plugin" --template from-v0
```

These generate placeholder UIs with import workflow instructions in the JS file.
