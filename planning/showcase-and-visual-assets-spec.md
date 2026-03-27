# Showcase & Visual Assets Spec

**Date:** 2026-03-26
**Type:** Product specification
**Depends on:** design-token-and-ai-designer-spec.md, pulp-ui-architecture-spec.md
**Implements:** Stream C showcase component from ai-designer-workstream-plan.md

---

## 1. Purpose

The showcase is the primary preview target for `pulp design`. It displays every theme-sensitive widget in a single view, so the AI Designer (and the developer) can see the full effect of a style change in one screenshot.

It also serves as:
- A visual regression baseline for theme changes
- A validation tool for new themes (do all widgets look correct?)
- Documentation material (screenshots for docs/examples)
- A proving ground for the AI Designer workflow

---

## 2. Showcase Layout

### Window / Screenshot Dimensions
- **Width:** 800px logical
- **Height:** 600px logical
- **Scale:** 2.0 (Retina, 1600×1200 physical)

### Layout Structure

```
┌─────────────────────────────────────────────────┐
│  Pulp Widget Showcase                    [dark]  │  ← Title row (24px)
├─────────────────────────────────────────────────┤
│                                                   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐         │
│  │  Gain     │ │  Freq    │ │  Mix     │         │  ← Knob row (80px)
│  │    ◯      │ │    ◯     │ │    ◯     │         │
│  │   0.5     │ │   0.7    │ │   1.0    │         │
│  └──────────┘ └──────────┘ └──────────┘         │
│                                                   │
│  ┌─ Volume ──────────────────────────○──┐        │  ← Horizontal fader (32px)
│  └──────────────────────────────────────┘        │
│                                                   │
│  ┌──┐  ┌──────────────────────┐  ┌──────────┐   │
│  │▮▮│  │  ◉ Bypass  ○ Mute   │  │ Filter ▾ │   │  ← Meter + Toggles + Combo (40px)
│  │▮▮│  │                      │  │          │   │
│  └──┘  └──────────────────────┘  └──────────┘   │
│                                                   │
│  ┌──────────────────────────────────────────┐    │
│  │ ~~~~~ Waveform ~~~~~                      │    │  ← WaveformView (80px)
│  └──────────────────────────────────────────┘    │
│                                                   │
│  ┌──────────────────────────────────────────┐    │
│  │ ▁▂▃▅▇█▇▅▃▂▁  Spectrum                   │    │  ← SpectrumView (80px)
│  └──────────────────────────────────────────┘    │
│                                                   │
│  ┌──────────────┐  ┌──────────────────────┐      │
│  │   XY Pad     │  │ ████████░░ 65%       │      │  ← XYPad + ProgressBar (80px)
│  │     ●        │  │                      │      │
│  └──────────────┘  └──────────────────────┘      │
│                                                   │
│  ┌── Tab1 ──┬── Tab2 ──┐                         │  ← TabPanel (40px)
│  │  Content             │                         │
│  └──────────────────────┘                         │
│                                                   │
│  Processing... ████████████░░░░░░ 65%            │  ← Status label + progress
└─────────────────────────────────────────────────┘
```

### Widget Inventory (18 widgets)

| Widget | ID | Initial Value | Purpose |
|--------|----|---------------|---------|
| Label (title) | `title` | "Pulp Widget Showcase" | Title bar, shows font.family |
| Label (theme) | `theme-name` | "dark" / etc. | Shows current theme name |
| Knob × 3 | `knob-gain`, `knob-freq`, `knob-mix` | 0.5, 0.7, 1.0 | Different values show arc range |
| Fader (horiz) | `fader-volume` | 0.65 | Shows track + thumb |
| Meter | `meter-level` | 0.75 RMS | Shows color-coded levels |
| Toggle × 2 | `toggle-bypass`, `toggle-mute` | on, off | Shows both states |
| ComboBox | `combo-filter` | "Lowpass" | Shows dropdown chrome |
| WaveformView | `waveform` | Synthetic sine | Shows accent.primary line |
| SpectrumView | `spectrum` | Synthetic rolloff | Shows bar/line rendering |
| XYPad | `xypad` | (0.3, 0.6) | Shows 2D surface |
| ProgressBar | `progress` | 0.65 | Shows track + fill |
| TabPanel | `tabs` | 2 tabs | Shows tab switching chrome |
| Label (status) | `status` | "Processing..." | Shows text.secondary |

### Layout Implementation

The showcase is built as a C++ function (not JS), constructing a View tree programmatically:

```cpp
// tools/design/showcase.hpp
namespace pulp::design {

std::unique_ptr<pulp::view::View> build_showcase(const pulp::view::Theme& theme);

constexpr size_t showcase_widget_count = 18;

}
```

**Why C++ not JS:** The showcase is a build tool, not a user-facing plugin. C++ gives reliable, fast construction with no ScriptEngine dependency. The screenshot tool's JS path remains available for custom previews.

### Synthetic Data

- **Waveform:** 512 samples of `sin(2π × 3 × t)` — 3 cycles visible
- **Spectrum:** 64 bins of `1.0 / (1.0 + bin * 0.1)` — natural rolloff shape
- **Meter RMS:** Fixed at 0.75 (shows green-to-yellow transition)

---

## 3. Built-in Style Packs

Export the three existing themes as style pack JSON files:

### styles/dark.json
```json
{
  "name": "Dark",
  "description": "Catppuccin Mocha-inspired dark theme. Blue accent.",
  "base": "dark",
  "overrides": {}
}
```

### styles/light.json
```json
{
  "name": "Light",
  "description": "Catppuccin Latte-inspired light theme. Blue accent.",
  "base": "light",
  "overrides": {}
}
```

### styles/pro_audio.json
```json
{
  "name": "Pro Audio",
  "description": "Tighter spacing, smaller controls. Neutral dark.",
  "base": "pro_audio",
  "overrides": {}
}
```

These have empty overrides because they ARE the base themes. They exist so `pulp design --list` shows them alongside user-created packs, and so the naming convention is demonstrated.

---

## 4. Screenshot & Preview Infrastructure

### Reuse Existing Code

The showcase reuses the existing screenshot pipeline from `core/view/screenshot.hpp`:

```cpp
// In pulp-design binary:
auto root = build_showcase(theme);
bool ok = render_to_file(*root, 800, 600, output_path, 2.0f);
```

No new rendering infrastructure needed. The CoreGraphicsCanvas headless backend is sufficient for macOS. Cross-platform screenshots (Windows/Linux) would require the Skia-based headless path (future work).

### Preview Flow

1. Render to a temp PNG: `/tmp/pulp-design-XXXXXX.png`
2. Open with system viewer: `open /tmp/pulp-design-XXXXXX.png` (macOS)
3. For MCP: return base64-encoded PNG (same as pulp_screenshot MCP tool)

---

## 5. Visual Regression Testing

### Approach

Screenshot-based comparison is deferred for MVP. Instead, the showcase tests verify:

1. **No crash:** `build_showcase()` + `render_to_png()` succeeds under all 3 themes
2. **Non-empty output:** PNG data is non-empty (rendering produced pixels)
3. **Widget count:** `count_views(root)` matches `showcase_widget_count`
4. **All IDs present:** Every widget in the inventory has a non-empty ID

### Future: Pixel Comparison

When visual regression becomes important:
- Commit reference PNGs in `test/golden/showcase-dark.png`, etc.
- Compare new renders using per-pixel tolerance (allow ±2 per channel for anti-aliasing)
- Run as a separate CI step (not blocking, informational)

---

## 6. Style Pack Proof Path

The workstream plan requires proving that a style pack can be:
1. Generated by AI
2. Previewed on the showcase
3. Saved as a file
4. Applied to a real plugin example
5. Shipped with the plugin

### MVP Proof Targets

| Target | Type | What It Proves |
|--------|------|----------------|
| Showcase | Widget collection | Style pack affects all widget types |
| PulpGain | Simple effect plugin | Style pack works on a real plugin UI |

### Proof Workflow

```bash
# 1. Generate a style
pulp design --save styles/vintage.json "warm vintage with amber accents"

# 2. Preview on showcase (already happens during design)
pulp design --load styles/vintage.json --preview-only

# 3. Apply to a real plugin (future: pulp design --target pulp-gain --load styles/vintage.json)
# For MVP: load the style pack in plugin code:
#   auto pack = StylePack::load("styles/vintage.json");
#   root.set_theme(pack.resolve());

# 4. Ship: the style pack JSON is a build artifact, included in the plugin bundle
```

### Plugin Integration Pattern

For a plugin to ship with a custom style:

```cpp
// In plugin UI setup:
auto pack = pulp::view::StylePack::load(resource_path / "theme.json");
root.set_theme(pack.resolve());
```

This is a one-line integration. The style pack file is a plain JSON resource bundled with the plugin.

---

## 7. Asset Organization

### In-Repo Structure

```
styles/
├── dark.json          # Built-in (empty overrides, base reference)
├── light.json         # Built-in
└── pro_audio.json     # Built-in
```

### User-Created (not in repo)

```
~/.pulp/styles/
├── vintage-amber.json
├── neon-cyberpunk.json
└── ...
```

### Project-Specific (in user's plugin repo)

```
my-plugin/
├── styles/
│   └── my-brand.json    # Project-specific style
├── CMakeLists.txt
└── ...
```

---

## 8. Documentation Plan

After implementation, add:

1. **docs/guides/design.md** — "Styling Pulp Plugins with AI"
   - Installing Claude CLI
   - `pulp design` walkthrough with screenshots
   - Style pack format reference
   - Applying styles to plugins
   - Creating style packs manually

2. **docs/reference/cli.md** — Add `design` command documentation

3. **docs/reference/capabilities.md** — Add AI Designer to capability list

4. Update **docs/status/cli-commands.yaml** with the design command

---

## 9. Acceptance Criteria

The showcase and visual assets work is done when:

1. `build_showcase()` produces a View tree with 18 widgets
2. The showcase renders correctly under dark, light, and pro_audio themes
3. All showcase widgets have IDs and are visible in inspector JSON
4. `styles/dark.json`, `styles/light.json`, `styles/pro_audio.json` exist and parse correctly
5. `pulp design --preview-only` renders a showcase screenshot
6. `pulp design --list` shows all 3 built-in packs (+ any user packs)
7. At least one example plugin (PulpGain) can load and apply a saved style pack
