# Pulp UI Architecture Spec (As-Built)

**Date:** 2026-03-26
**Type:** Architecture documentation — describes the system as it exists, not aspirational goals
**Source files:** core/view/, core/canvas/, core/render/

---

## 1. View Hierarchy

**File:** `core/view/include/pulp/view/view.hpp`

The `View` class is the base node in the UI tree.

### Properties
| Property | Type | Purpose |
|----------|------|---------|
| `bounds_` | `Rect` | Position and size in parent space |
| `children_` | `vector<unique_ptr<View>>` | Owned child views |
| `parent_` | `View*` | Non-owning back-pointer |
| `theme_` | `Theme` | Local token storage |
| `flex_` | `FlexStyle` | Layout configuration |
| `id_` | `string` | Identifier for inspector/bridge lookup |
| `visible_` | `bool` | Visibility flag |
| `access_role_` | `AccessRole` | Accessibility role |
| `access_label_` | `string` | Accessibility label |
| `access_value_` | `string` | Accessibility value |

### Child Management
- `add_child(unique_ptr<View>)` — ownership transfer, sets parent pointer
- `remove_child(View*)` — releases ownership
- `child_count()`, `child_at(index)` — indexed access

### Rendering
- `paint(Canvas&)` — virtual, draw this view only
- `paint_all(Canvas&)` — draws self + all children recursively
- `layout_children()` — flex layout algorithm, called before paint

---

## 2. Theme Resolution Chain

**File:** `core/view/src/view.cpp` lines 151-156

```
View::resolve_color(name, fallback)
  1. Check own theme_.colors map
  2. If not found → parent_->resolve_color(name, fallback)
  3. At root (no parent) → return fallback
```

This creates CSS-like inheritance: set a theme at the root, override specific tokens at any intermediate view. A child without a local color definition inherits from its nearest ancestor that defines it.

### Implications for AI Designer
- Applying a theme at the root affects all widgets automatically
- Per-widget overrides are possible by setting theme on individual views
- The AI Designer only needs to modify the root theme for global restyling
- Scoped edits (just change the meters, just change the knobs) would require per-view theme overrides

---

## 3. Design Token Schema

**File:** `core/view/src/theme.cpp` lines 100-221

### Color Tokens (16 total)
| Category | Tokens |
|----------|--------|
| Background | `bg.primary`, `bg.secondary`, `bg.surface`, `bg.elevated` |
| Text | `text.primary`, `text.secondary`, `text.disabled` |
| Accent | `accent.primary`, `accent.secondary`, `accent.success`, `accent.warning`, `accent.error` |
| Control | `control.track`, `control.fill`, `control.thumb`, `control.border` |

### Dimension Tokens (17 total)
| Category | Tokens |
|----------|--------|
| Spacing | `spacing.xs` (2), `spacing.sm` (4), `spacing.md` (8), `spacing.lg` (16), `spacing.xl` (24) |
| Radius | `radius.sm` (4), `radius.md` (8), `radius.lg` (12), `radius.full` (9999) |
| Font size | `font.xs` (10), `font.sm` (12), `font.md` (14), `font.lg` (18), `font.xl` (24) |
| Control size | `control.knob_size` (48), `control.fader_width` (24), `control.meter_width` (12) |

### String Tokens (2 total)
- `font.family` = "Inter"
- `font.mono` = "JetBrains Mono"

### Token Naming Convention
- Dot-separated hierarchy: `category.name`
- Categories: bg, text, accent, control, spacing, radius, font
- Values in default units: colors as hex, dimensions in logical pixels, strings as-is

### Built-in Themes
| Theme | Character |
|-------|-----------|
| `dark()` | Catppuccin Mocha-inspired. Blue accent. |
| `light()` | Catppuccin Latte-inspired. Same dimensions/strings as dark. |
| `pro_audio()` | Tighter spacing (6px md, 12px lg), smaller font (12px md), smaller knobs (40px). |

---

## 4. Widget Token Usage Map

Every widget reads specific tokens. This is the complete mapping:

| Widget | Color Tokens Read | Dimension Tokens Read |
|--------|-------------------|----------------------|
| Label | text.primary, font.family | font.md |
| Knob | control.track, control.fill, control.thumb, text.secondary, text.primary | control.knob_size |
| Fader | control.track, control.fill, control.thumb, text.secondary | control.fader_width |
| Toggle | accent.primary, control.track, control.thumb | — |
| Meter | control.track, accent.success, accent.warning, accent.error, control.thumb | control.meter_width |
| XYPad | bg.surface, control.border, control.fill, control.thumb, text.secondary | — |
| WaveformView | bg.surface, control.border, accent.primary | — |
| SpectrumView | bg.surface, accent.primary, control.border | — |
| ComboBox | bg.surface, text.primary, control.border, accent.primary | radius.md, font.md |
| ProgressBar | control.track, accent.primary | radius.md |
| TabPanel | bg.secondary, text.primary, accent.primary | — |
| ListBox | bg.surface, text.primary, accent.primary | — |
| TextEditor | bg.surface, text.primary, accent.primary, control.border | font.md |

All 16 color tokens are actively used. The most-used tokens are `control.track`, `control.fill`, `accent.primary`, `text.primary`, and `bg.surface`.

---

## 5. Canvas Abstraction

**File:** `core/canvas/include/pulp/canvas/canvas.hpp`

Abstract `Canvas` interface with ~20 methods:
- State: save(), restore()
- Transform: translate(), scale(), rotate()
- Clip: clip_rect()
- Style: set_fill_color(), set_stroke_color(), set_line_width(), set_line_cap(), set_line_join()
- Shapes: fill_rect(), stroke_rect(), fill_rounded_rect(), fill_circle(), stroke_circle()
- Arcs: stroke_arc()
- Lines: stroke_line()
- Text: set_font(), set_text_align(), fill_text(), measure_text()

### Backends
| Backend | File | Use Case |
|---------|------|----------|
| CoreGraphicsCanvas | `cg_canvas.hpp` | macOS native rendering, screenshot |
| SkiaCanvas | `skia_canvas.hpp` | GPU rendering (Dawn/Graphite) |
| RecordingCanvas | `canvas.hpp` (embedded) | Test harness, captures draw commands |

Widgets never reference a specific backend — they paint via the abstract Canvas.

---

## 6. Inspector System

**File:** `core/view/include/pulp/view/inspector.hpp`, `core/view/src/inspector.cpp`

### API
- `ViewInspector::to_json(root)` → JSON tree of the entire UI
- `ViewInspector::find_by_id(root, id)` → View pointer lookup
- `ViewInspector::count_views(root)` → total node count
- `ViewInspector::type_name(view)` → widget class name string

### JSON Structure Per Node
```json
{
  "type": "Knob",
  "id": "gain",
  "bounds": { "x": 10, "y": 20, "width": 48, "height": 48 },
  "visible": true,
  "value": 0.75,
  "label": "Gain",
  "children": []
}
```

Type detection uses dynamic_cast chain: Knob → Fader → Toggle → Label → Meter → XYPad → WaveformView → SpectrumView → fallback "View".

### Implications for AI Designer
- Inspector can show what widgets exist and their current state
- Could be extended to show resolved token values per widget (gap)
- Combined with screenshot, gives AI agents both visual and structural context

---

## 7. Hot Reload System

**File:** `core/view/include/pulp/view/hot_reload.hpp`, `core/view/src/hot_reload.cpp`

### Architecture
1. Background thread: choc::file::Watcher polls filesystem every 200ms
2. On .js/.mjs change: read file, store in pending buffer (mutex-protected)
3. UI thread: poll_reload() checks pending flag, executes callback if pending
4. Callback runs on UI thread (safe for widget creation/modification)

### Current Limitation
- Watches JS files only — designed for ScriptEngine-based UIs
- Does NOT currently watch JSON theme files
- For the AI Designer, we need theme JSON hot-reload (either extend HotReloader or add a parallel watcher)

---

## 8. Screenshot Pipeline

**File:** `core/view/include/pulp/view/screenshot.hpp`, `core/view/platform/mac/screenshot_mac.mm`

### Process
1. Create CGBitmapContext at physical pixel size (width×scale, height×scale)
2. Wrap in CoreGraphicsCanvas
3. Fill background with `bg.primary` color from theme (or default 0x1E1E2E)
4. Set root bounds, call layout_children(), call paint_all(canvas)
5. Extract CGImage, write PNG via CGImageDestination

### Constraints
- macOS only (returns empty on other platforms)
- Headless — no window needed
- Default scale: 2.0 (Retina)
- Output: PNG bytes or file

---

## 9. JS Widget Bridge

**File:** `core/view/include/pulp/view/widget_bridge.hpp`, `core/view/src/widget_bridge.cpp`

Registers JS functions in ScriptEngine context:
- `createKnob(id, x, y, w, h)`, `createFader(...)`, `createToggle(...)`, `createLabel(...)`
- `setValue(id, value)`, `getValue(id)`
- `getParam(name)`, `setParam(name, value)`
- `sync_from_store()` — refreshes all widget values from StateStore

### Implications for AI Designer
- JS bridge could be extended with `setTheme(json)` and `getTheme()` functions
- This would allow JS-scripted design sessions, not just C++ CLI

---

## 10. Layout System

**File:** `core/view/include/pulp/view/geometry.hpp`, `core/view/src/view.cpp` lines 158-249

CSS-like flex layout:
- Direction: row or column
- Alignment: start, center, end, stretch
- Justify: start, center, end
- Gap, padding, flex_grow
- Min/preferred dimensions

Algorithm: calculate fixed children, distribute remaining space to flex-grow children, apply cross-axis alignment.

---

## 11. Gaps Identified for AI Designer

| Gap | Severity | Description |
|-----|----------|-------------|
| No hover/active state tokens | Medium | Widgets hardcode hover brightness. AI Designer can't restyle hover states via tokens. |
| No animation duration tokens | Low | Transition timing is hardcoded. Style packs can't control animation feel. |
| No theme JSON hot-reload | High | HotReloader watches .js only. Need .json watching for live design sessions. |
| Inspector doesn't show tokens | Medium | Inspector shows widget state but not resolved token values. AI needs to see what tokens resolve to. |
| Screenshot is macOS-only | Medium | Cross-platform preview would need Skia-based headless rendering. |
| No token validation API | High | Need to validate that a JSON diff only contains known token names. |
| No style pack format | High | Need a named, saveable, loadable theme overlay format. |

### Priority for MVP
1. **Token validation API** — essential for AI interaction safety
2. **Style pack format** — essential for save/load workflow
3. **Theme JSON hot-reload** — essential for live preview
4. Inspector token display and hover tokens are nice-to-have for v1.
