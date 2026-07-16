# Design Import API Reference

Technical reference for the Pulp design import pipeline. For a getting-started guide, see [Importing Designs](../guides/importing-designs.md).

## CLI Commands

### import-design

Import a design from an external tool into Pulp JS, DesignIR, or baked C++
artifacts.

```
pulp import-design --from <source> [options]
```

**Sources:** `fig`, `figma`, `figma-plugin`, `stitch`, `v0`, `pencil`, `claude`, `designmd`, `jsx`

> `fig` decodes a local Figma `.fig` save file offline into the same envelope
> consumed by the `figma-plugin` path.
> `figma-plugin` is the **"Design for Pulp"** Figma plugin export (a semantic
> audio-widget envelope; see the [Figma plugin guide](../guides/figma-plugin.md)).
> It accepts a `.pulp.zip` directly. `figma` is the raw Figma REST/file import.

| Flag | Description | Default |
|------|-------------|---------|
| `--from <source>` | Design source (required) | — |
| `--file <path>` | Input file path | — |
| `--url <url>` | Design URL (Figma file URL, v0 share link) | — |
| `--frame <name>` | Frame/artboard to import. For `--from fig`, pass a frame guid or name; required unless using `--outline`. **Repeatable** — give it once per state to capture a multi-state design into one `DesignFrameView`; the order given is the frame index a `swap <n>` layer targets (the first `--frame` is frame 0). | first frame for `figma` |
| `--page <name>` | Restrict local `.fig` frame lookup to one page | — |
| `--outline` | List pages/frames of a local `.fig` file and exit | — |
| `--screen <name>` | Screen to import (Stitch) | first screen |
| `--output <path>` | Destination file for the primary artifact | `ui.js` |
| `--emit {js\|ir-json\|cpp\|swiftui}` | Primary artifact kind. `js`, `ir-json`, `cpp`, and `swiftui` are implemented; `cpp` and `swiftui` require `--mode baked`. `swiftui` emits a baked native SwiftUI view (`ImportedPulpView.swift` + a per-view `<RootView>Theme.swift` + binding manifest). | `js` built-in, or `import_design.default_emit` |
| `--mode {live\|baked}` | Runtime model. `live` is the built-in default. `baked` emits canonical IR or baked C++ via `--emit ir-json\|cpp`. | `live` built-in, or `import_design.default_mode` |
| `--snapshot-semantics {fail\|warn\|accept}` | JSX baked snapshot policy. `fail` rejects dynamic APIs by default, `warn` proceeds with diagnostics, and `accept` proceeds silently. | `fail` |
| `--allow-network-fetch` | Allow DesignIR asset-manifest HTTP(S) fetches at import time. | off |
| `--asset-cache <path>` | Asset cache directory for HTTP(S) imports. | `PULP_IMPORT_ASSET_CACHE` or user cache |
| `--asset-timeout-ms <ms>` | Per-request network asset timeout. | `30000` |
| `--asset-hash <uri=sha256>` | Expected content hash for an asset URI; may be repeated. | — |
| `--tokens <path>` | Output token file | `tokens.json` (`theme.css` for `css-variables`) |
| `--format {w3c\|css-variables\|tailwind\|json-tailwind\|css-tailwind}` | Token export format. `w3c` (DTCG JSON) is the default; `css-variables` emits CSS custom properties (base → `:root`, `.dark`-suffixed modes → `@media (prefers-color-scheme: dark)`); the `tailwind` variants currently require `--from designmd`. | `w3c` |
| `--dry-run` | Show generated code without writing | — |
| `--no-tokens` | Skip token extraction | — |
| `--no-comments` | Omit comments from generated code | — |
| `--web-compat` | Use DOM API instead of native Pulp API | — |
| `--validate` | Render generated JS and validate layout | — |
| `--screenshot-backend {skia\|coregraphics}` | `--validate` render backend. **Skia** (default) composites file-backed images; **CoreGraphics** does not (it draws an image's filename placeholder — not faithful). | `skia` |
| `--reference <png>` | Compare render against reference screenshot | — |
| `--diff <png>` | Save visual diff image | — |
| `--render-size WxH` | Render dimensions | `340x280` |
| `--preview` | Minimal widget styling for design comparison | — |
| `--debug` | Output JSON report with metrics | — |
| `--debug-output <path>` | Save debug JSON to file | stdout |
| `--detect-only` | Detect source, format-version, and parser-version without parsing; see [imports/](imports/index.md) for detection details. | — |
| `--directory <path>` | Path to directory export (alternative to `--file`) | — |
| `--compat <path>` | Override `compat.json` discovery | walk up from input |
| `--report-new-format` | Emit a fingerprint-diff JSON for a new format-version. Implies `--detect-only` | — |

Either `--file` or `--url` is required (or `--directory` for `--detect-only`). When `--url` is provided without `--file`, the URL is fetched through an argv-safe `curl` invocation into a unique temporary file. Literal `--file` paths are read directly and may contain normal filesystem punctuation; `--url` still rejects shell metacharacters before fetching.

The shipped default is live runtime import: `--mode live --emit js`. In that
mode Pulp keeps the generated JS/runtime artifact, which is the right default
for iteration, hot reload, dynamic React behavior, and design-tool workflows.
Users who strongly prefer a different default can persist it in
`~/.pulp/config.toml`:

```bash
pulp config set import_design.default_mode baked
pulp config set import_design.default_emit ir-json
```

For baked C++ by default, set `import_design.default_emit cpp` as well. If only
`import_design.default_mode baked` is set, Pulp chooses `ir-json` as the baked
artifact default. `PULP_IMPORT_DESIGN_DEFAULT_MODE` and
`PULP_IMPORT_DESIGN_DEFAULT_EMIT` override those config keys for one
environment/session, and direct `--mode` / `--emit` flags override the matching
preference.
`pulp status` reports the effective import-design defaults.

Mental model:

| Flow | What ships | Runtime behavior |
|------|------------|------------------|
| Live/runtime import | The generated JS or precompiled React bundle | Runs the app at launch through the JS/React/runtime bridge |
| Baked DesignIR | A serialized snapshot of the materialized UI tree | No React program remains; the IR is an inspectable UI blueprint |
| Baked C++ | Native C++ generated from DesignIR | Constructs native views directly without a JS engine |
| Baked SwiftUI | Native SwiftUI generated from DesignIR | A compiled declarative Apple view; no Pulp JS runtime |

Live/runtime import means "run the original app." Baked DesignIR means "run
the app once and save the resulting UI structure." Baked C++ means "compile
that saved structure into native code." You can move from live iteration to a
baked snapshot and then to baked C++; you cannot reconstruct the original live
React program from baked IR because loops, hooks, closures, and arbitrary JS
logic are not preserved in the snapshot.

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

`--emit swiftui` (baked-only, macOS 13 / iOS 16 floor) lowers the same DesignIR
to native SwiftUI — a fourth target alongside DOM/native-JS/baked-C++. It writes
`ImportedPulpView.swift` (a `View` generic over the `PulpParameterResolving`
protocol from `PulpSwift`), a sibling per-view theme file `<RootView>Theme.swift`,
and a `.bindings.json` manifest. Frames lower to `VStack`/`HStack`, text to
`Text`, fixed frame/padding/background modifiers apply, and knob/slider/toggle
bind to `PulpKnob`/`PulpSlider`/`PulpToggle`. Tokens lower to a code-first
`<RootView>Theme` enum (named per-view so two imports don't collide in one Swift
target) whose `.dark`-suffixed entries become light/dark dynamic colors. Binding resolves
a generated key by exact `PulpParameter.name` match (Pulp has no stable string
param key); missing or duplicate names render a visible placeholder rather than
silently binding the wrong parameter. The full visual style set is emitted
(opacity, corner radius, border, box-shadow, linear gradient, transform,
mix-blend-mode, and mixed-style text), and `rgb()`/`rgba()` colours are
supported alongside hex. Because SwiftUI stacks are not Yoga, flex layout is
approximate: cross-axis alignment maps to the stack's `alignment:`, and
space-between/around is approximated with `Spacer()`s. CSS grid lowers to a
`LazyVGrid` (column count from `grid-template-columns`; exact track sizing
approximated). Anything a SwiftUI stack cannot reproduce — flex-wrap, justify
distribution, `align:stretch`, `position:absolute` (approximated with
`.offset`), skew/matrix transforms, per-side borders, and multi-/inset shadows —
is reported as a `fidelity:` warning; the ones that genuinely render wrong fail
the import under `--strict-fidelity` (exit 4). The audio
meter/xy_pad/waveform/spectrum widgets map to native
`PulpMeter`/`PulpXYPad`/`PulpWaveform`/`PulpSpectrum` views, text buttons to a
SwiftUI `Button`, and images to `Image("<asset_id>")` (bundled, by asset-catalog
id) or `AsyncImage` (remote); svg/canvas vectors remain deferred. A
`templates/swiftui-design-host/` scaffold mounts the generated root view in a
standalone app for Simulator/macOS preview.

For `--from jsx --mode live --emit js`, Pulp writes the precompiled bundle
verbatim for runtime import. That pass-through path does not parse or render
the bundle, so `--validate`, `--reference`, `--diff`, and `--debug` are rejected;
use baked IR or baked C++ when an import report or native snapshot validation is
needed. For JSX baked IR/C++ snapshots, Pulp first runs the runtime harness and
walks the materialized DOM; when a live/native bundle routes React through
`@pulp/react` and leaves no expanded DOM, the harness freezes the native
`WidgetBridge` tree instead (`capture_method: runtime_native_snapshot`,
`snapshotSource: native-view`). Pulp scans the precompiled bundle for
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

Export a Pulp theme as W3C Design Tokens JSON (default) or another `--format`.

```
pulp export-tokens [options]
pulp export-tokens --format css-variables          # built-in dark theme → theme.css
pulp export-tokens --file theme.json --format css-variables --tokens theme.css
```

| Flag | Description | Default |
|------|-------------|---------|
| `--file <path>` | Input theme JSON | built-in dark theme |
| `--format <fmt>` | Token export format (`w3c`, `css-variables`) | `w3c` |
| `--tokens <path>` | Output file | `tokens.json` (`theme.css` for `css-variables`) |
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

For importer and backend planning, classify visual properties with the rendering
reference's effect/mask/composite split: filters and backdrop filters are
effects, `clip-path` and mask properties constrain visibility, and
`mix-blend-mode` / Canvas `globalCompositeOperation` control how a completed
subtree composites back into its parent. Keeping those categories distinct
prevents a design import gap from becoming the wrong runtime abstraction.

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

## CSS custom properties (`--format css-variables`)

Emits the captured theme as CSS custom properties — the external, themeable
output sink for the multi-mode tokens the importers capture. Base (light/default)
tokens land under `:root`; tokens whose name carries the `.dark` multi-mode
suffix (the convention the Figma plugin and DESIGN.md body parser use for
dark-mode values) become overrides under `@media (prefers-color-scheme: dark)`.

Token names map to custom-property ids by replacing `.` with `-`
(`color.bg` → `--color-bg`); colors become hex, dimensions get a `px` unit, and
string tokens are emitted verbatim.

```css
/* Generated by pulp import-design --format css-variables */
:root {
  --color-bg: #ffffff;
  --spacing-sm: 4px;
}

@media (prefers-color-scheme: dark) {
  :root {
    --color-bg: #000000;
  }
}
```

This is an external artifact consumable by web tooling and Style-Dictionary
pipelines. Pulp's runtime already resolves `var(--x)`, but a loader that applies
a themed `@media` CSS file through the theme/`setProperty` APIs is a separate,
later step — the `css-variables` exporter does not claim runtime consumption.
The `[data-theme="dark"]` selector strategy is intentionally not emitted; only
`@media` is, until a real host-toggle consumer exists.

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

### Figma plugin (`parse_figma_plugin_json`)

The **"Design for Pulp"** Figma plugin export envelope. Recognized Pulp library
components (Knob / Fader / Meter / XYPad / Waveform / Spectrum) carry semantic `audio_widget` metadata +
parameter bindings, so they materialize as native, bound widgets rather than
generic frames; everything else imports as layout/visual. Accepts a `.pulp.zip`
(assets bundled) directly — the importer unpacks it and resolves assets from the
manifest — or a bare `.pulp.json`. See the
[Figma plugin guide](../guides/figma-plugin.md) for the full workflow.

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
