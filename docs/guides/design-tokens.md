# Design Tokens

Design tokens are the building blocks of Pulp's theme system. They store colors, dimensions, typography, and motion values as structured data — not code. This guide covers how tokens work, how to create themes, and how to integrate with external design tools.

## How Tokens Work

A `Theme` is three flat maps:

```
Theme
├── colors      { "background": #0f0f1a, "accent": #58a6ff, ... }
├── dimensions  { "spacing_sm": 8, "corner_radius_md": 8, ... }
└── strings     { "font_family": "Inter", ... }
```

Every view in the tree can have its own theme. When a widget resolves a token, it walks up the parent chain until it finds a match:

```
child → parent → grandparent → root → built-in fallback
```

This means you can override tokens for a specific section without affecting the rest of the UI.

## Token Categories

### Colors

| Token | Purpose | Dark Default | Light Default |
|-------|---------|-------------|---------------|
| `background` | Window/root background | `#0f0f1a` | `#f5f5f5` |
| `surface` | Panel/card background | `#1a1a2e` | `#ffffff` |
| `surface_variant` | Alternate surface | `#1e1e36` | `#f0f0f0` |
| `on_surface` | Text on surface | `#e0e0e0` | `#1a1a1a` |
| `accent` | Primary interactive color | `#58a6ff` | `#0066cc` |
| `primary` | Brand/primary color | `#58a6ff` | `#0066cc` |
| `secondary` | Secondary accent | `#8b5cf6` | `#6633cc` |
| `tertiary` | Tertiary accent | `#ec4899` | `#cc3399` |
| `success` | Success/positive | `#22c55e` | `#16a34a` |
| `warning` | Warning/caution | `#f59e0b` | `#d97706` |
| `error` | Error/destructive | `#ef4444` | `#dc2626` |

### Dimensions

| Token | Purpose | Default |
|-------|---------|---------|
| `spacing_xs` | Extra-small spacing | `4` |
| `spacing_sm` | Small spacing | `8` |
| `spacing_md` | Medium spacing | `12` |
| `spacing_lg` | Large spacing | `16` |
| `spacing_xl` | Extra-large spacing | `24` |
| `corner_radius_sm` | Small border radius | `4` |
| `corner_radius_md` | Medium border radius | `8` |
| `corner_radius_lg` | Large border radius | `12` |

### Typography

| Token | Purpose | Default |
|-------|---------|---------|
| `font_family` | Primary font | `"Inter"` |
| `font_size_xs` | Extra-small text | `12` |
| `font_size_sm` | Small text | `14` |
| `font_size_md` | Medium text | `16` |
| `font_size_lg` | Large text | `18` |
| `font_weight_normal` | Normal weight | `400` |
| `font_weight_bold` | Bold weight | `700` |

### Motion

| Token | Purpose | Default |
|-------|---------|---------|
| `motion.duration.fast` | Quick transitions | `80` ms |
| `motion.duration.normal` | Standard transitions | `150` ms |
| `motion.duration.slow` | Slow/dramatic transitions | `300` ms |
| `motion.duration.meter_decay` | Meter falloff speed | `300` ms |
| `motion.duration.peak_hold` | Peak indicator hold time | `1500` ms |
| `motion.easing.interaction` | User interaction easing | `ease_out_cubic` |
| `motion.easing.enter` | Element enter easing | `ease_out_quad` |
| `motion.easing.exit` | Element exit easing | `ease_in_quad` |

## Creating a theme.json

A theme JSON file defines token overrides. Only include tokens you want to change — unspecified tokens fall through to the parent or built-in default.

```json
{
    "colors": {
        "background": "#0d1117",
        "surface": "#161b22",
        "surface_variant": "#1c2128",
        "on_surface": "#c9d1d9",
        "accent": "#58a6ff",
        "primary": "#58a6ff",
        "secondary": "#bc8cff",
        "success": "#3fb950",
        "warning": "#d29922",
        "error": "#f85149"
    },
    "dimensions": {
        "spacing_sm": 8,
        "spacing_md": 12,
        "corner_radius_sm": 6,
        "corner_radius_md": 10
    },
    "strings": {
        "font_family": "JetBrains Mono"
    }
}
```

## Using Tokens in JS

### Built-in Themes

```js
setTheme("dark");       // Default dark theme
setTheme("light");      // Light theme
setTheme("pro_audio");  // Pro Audio (DAW-style) theme
```

### Reading Theme Data

```js
const themeJson = getThemeJson();
const theme = JSON.parse(themeJson);
// theme.colors.accent → "#58a6ff"
```

### Applying Partial Overrides

```js
// Override just the accent color without replacing the entire theme
applyTokenDiff(JSON.stringify({
    colors: {
        accent: "#ff6b6b",
        primary: "#ff6b6b"
    }
}));
```

### Motion Tokens

```js
// Read motion timing from the theme
const duration = getMotionToken("motion.duration.normal"); // 150

// Use theme-consistent animation timing
animate("knob", "opacity", 1.0, duration, "ease_out_cubic");

// Override a motion token
setMotionToken("motion.duration.fast", 50);
```

## Using Tokens in C++

```cpp
// Set up a theme
Theme theme = Theme::dark();

// Override specific tokens
theme.colors["accent"] = Color::hex(0xff6b6b);
theme.dimensions["spacing_md"] = 16.0f;

// Apply to a view
root->set_theme(theme);

// Resolve a token (walks up parent chain)
Color bg = view.resolve_color("background", Color::hex(0x000000));
```

### Loading from JSON

```cpp
std::string json = read_file("ui/theme.json");
Theme custom = Theme::from_json(json);
root->set_theme(custom);
```

### Serializing to JSON

```cpp
std::string json = theme.to_json();
write_file("ui/theme.json", json);
```

## Inheritance and Cascading

Tokens cascade through the view tree. Override tokens at any level:

```
Root (dark theme)
��── Header Panel (override: accent → red)
│   ├── Title Label  → resolves accent as red
│   └── Subtitle     → resolves accent as red
├── Main Panel (no overrides)
│   ├── Knob         → resolves accent as blue (from root)
│   └── Fader        → resolves accent as blue (from root)
└── Footer (override: surface → darker)
    └── Label        → resolves surface as darker, accent as blue
```

In JS:

```js
// Override tokens for a specific panel
setBackground("header", getThemeColor("error")); // Use error red
// Children of "header" inherit the panel's visual context
```

In C++:

```cpp
Theme header_theme;
header_theme.colors["accent"] = Color::hex(0xff4444);
header_panel->set_theme(header_theme);
// All children of header_panel resolve "accent" as red
```

## Exporting to CSS Custom Properties

For web builds (WASM), tokens map to CSS custom properties:

```css
:root {
    --pulp-background: #0f0f1a;
    --pulp-surface: #1a1a2e;
    --pulp-accent: #58a6ff;
    --pulp-spacing-sm: 8px;
    --pulp-corner-radius-md: 8px;
    --pulp-font-family: 'Inter', sans-serif;
}
```

Generate CSS from a theme:

```bash
pulp export-tokens --file theme.json --format css-variables --tokens tokens.css
```

## Using Tokens in WGSL

`pulp export-tokens` does not generate WGSL or C++ headers today. For custom
shader widgets, load the exported W3C JSON or CSS custom-property values in the
host code that configures the shader, then map those values into uniforms:

```wgsl
struct ThemeUniforms {
    accent: vec4<f32>,
    background: vec4<f32>,
    surface: vec4<f32>,
    on_surface: vec4<f32>,
}

@group(0) @binding(0) var<uniform> theme: ThemeUniforms;

@fragment
fn main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    return mix(theme.background, theme.accent, uv.x);
}
```

## Figma Integration

Export Pulp tokens as W3C Design Tokens JSON for design handoff:

```bash
pulp export-tokens --file theme.json --tokens tokens.json
```

The exported JSON uses the W3C/DTCG token format:

```json
{
    "color": {
        "background": { "$value": "#0f0f1a", "$type": "color" },
        "accent": { "$value": "#58a6ff", "$type": "color" }
    },
    "spacing": {
        "sm": { "$value": "8", "$type": "dimension" },
        "md": { "$value": "12", "$type": "dimension" }
    }
}
```

Import into Figma using a token workflow that accepts W3C/DTCG JSON, such as
the [Tokens Studio](https://tokens.studio/) plugin. Changes in Figma can be
exported back to `theme.json` to keep design and code in sync.

## Best Practices

1. **Use tokens, not hardcoded values.** Write `getThemeColor("accent")` instead of `"#58a6ff"`. This ensures your UI respects theme switching.

2. **Override at the narrowest scope.** Don't replace the entire theme when you only need a different accent color for one panel.

3. **Keep motion tokens consistent.** All animations in the same category (interactions, enters, exits) should use the same duration and easing.

4. **Name custom tokens with namespaces.** If you add plugin-specific tokens, prefix them: `mysynth.oscillator_color`, `mysynth.filter_glow`.

5. **Test with all three built-in themes.** A UI that only looks good in dark mode is incomplete.

## Reskinning Pulp end-to-end

"Reskinning" means changing the look of every widget — colors, radii, type
scale — by editing **tokens**, never by editing widget code. The guarantee that
a reskin "just works" comes from one rule: **widgets resolve their appearance
from theme tokens at paint time, and the design language is the only thing that
varies.** Components (the Knob, Fader, WaveformEditor, Table…) are fixed, native,
and trusted; only their skin changes.

### The flow

1. **Edit the token set** — either in Figma Variables (designer-facing) or
   directly in a `theme.json` / Tokens-Studio JSON in the repo.
2. **Import it** — `importDesignTokens(json)` (JS) or `parse_figma_variables` /
   `parse_w3c_tokens` / `Theme::from_json` (C++).
3. **Apply it** — `root->set_theme(theme)` (C++) or `applyTokenDiff(diff)` (JS).
   `set_theme` repaints the surface, and because color lookups walk the parent
   chain (`View::resolve_color`), every descendant that doesn't carry its own
   theme restyles on the next paint. No recompile in the JS lane; one rebuild in
   the baked C++ lane.

### Semantic colors derive the rest

A theme is seeded from ~16 **semantic** colors (background, foreground, primary,
accent, destructive, the chart colors, …). The derivation layer
(`derive_theme`) expands those into ~35 audio-specific tokens — `knob.arc`,
`slider.fill`, `meter.green/yellow/red`, `waveform.line`, `tab.active`, and so
on — so you restyle a whole instrument UI by changing a handful of inputs. A
preset may add explicit overrides for tokens the derivation can't infer (e.g.
brand-specific meter inks). The built-in **`ink-signal`** preset ("Ink &
Signal", Pulp's flagship language) is the reference example of this:
signal-teal primary, cool graphite surfaces, coral reserved for peak/danger,
with leaf/amber meter overrides.

### Make a widget reskinnable

Resolve from a token with a sensible fallback, never a bare literal:

```cpp
// Good — themeable, with a fallback for the no-theme case:
auto fill = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));

// Bad — ignores the theme; the widget can never be reskinned:
auto fill = canvas::Color::rgba8(100, 150, 255);
```

Low-alpha **material effects** (drop shadows, a thin highlight stroke, a hover
scrim) are *not* theme colors and may stay literal — they read as light/shadow,
not brand. Everything that carries the design's identity (fills, text,
accents, borders, meters) must resolve from a token.

> Gotcha: `Color::rgba(r,g,b,a)` takes **0–1 floats** and clamps; `Color::rgba8`
> takes **0–255 bytes**. Passing `rgba(120,160,255)` clamps every channel to
> 1.0 and paints solid white. Use `rgba8` for byte values.

### Use the *real* token key — a typo silently disables reskinning

`resolve_color("key", fallback)` returns the fallback whenever `key` isn't a
token in the active theme — including when `key` is a **typo or an old name**.
The widget then compiles, renders the hardcoded fallback, and silently can't be
reskinned. This shipped real bugs: a `ProgressBar` that resolved
`"progress_track"` / `"accent"` (neither exists — the real names are
`progress.track` / `progress.fill`) painted a permanent coral fill, and a
`TabPanel` underline resolved `"accent"` and stayed coral instead of the teal
`tab.active`.

The canonical token names are exactly the `t.colors["…"]` keys assigned in
`core/view/src/theme_presets.cpp` (`derive_theme` + the per-preset overrides) —
dotted, e.g. `bg.surface`, `text.primary`, `control.border`, `meter.green`,
`progress.fill`, `tab.active`. Never underscore (`tab_bar_bg`) or bare
(`text`, `border`) forms.

```cpp
// Bad — "text"/"border"/"callout_bg" are NOT tokens; the fallback is permanent:
resolve_color("text",   canvas::Color::hex(0xe0e0e0));   // -> text.primary
resolve_color("border", canvas::Color::hex(0x3a3a5a));   // -> control.border / modal.border

// Good:
resolve_color("text.primary",   canvas::Color::hex(0xe0e0e0));
resolve_color("modal.border",   canvas::Color::hex(0x3a3a5a));
```

This is **enforced**: `tools/scripts/token_key_check.py` (the
`token-key-correctness` ctest) fails the build on any `resolve_color("…")` whose
key isn't canonical. A deliberate widget-specific override token that *falls back
to a canonical token* (e.g. `resolve_color("text_editor_bg", resolve_color("bg.surface", …))`)
is allowed via that script's small `ALLOWLISTED_OVERRIDES` set.

### Verifying a reskin

- **Headless**: drive a widget through a `RecordingCanvas` and assert the fill
  colors it emits track the theme (see `test/test_buttons.cpp`).
- **Visual**: render with the Skia backend and diff against a reference
  (`render_to_png(ScreenshotBackend::skia)`); the CoreGraphics backend does not
  composite file-backed images, so use Skia for any asset-bearing UI.
