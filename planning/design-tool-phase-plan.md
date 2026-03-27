# Pulp Design Tool — Phase Plan

**Date:** 2026-03-27
**Status:** Proposed
**Scope:** From current state → full visual plugin editor

---

## Vision

The Pulp Design Tool is where developers build, style, and refine plugin UIs without recompiling C++. It starts as a theme editor and evolves into a visual layout tool.

**Near term:** Load your plugin's theme, tweak colors/styles with AI or manual controls, export tokens back to your project. Hot reload means instant feedback.

**Medium term:** Load your actual plugin UI in the preview panel. See your real widgets with real parameter connections. Adjust styles and see them applied to your actual plugin.

**Long term:** A visual editor where you can unlock the canvas, drag widgets to reposition them, resize with handles, snap to grid, group/ungroup, and have the tool generate/update the JS code that defines that layout. Think Figma for audio plugins, but the preview IS the real renderer.

---

## Current State (phase/ai-designer branch, 15 commits)

### What Works
- Multi-style widget rendering (knob arc/filled/notched, toggle pill/checkbox/rocker)
- Interactive widgets (knob drag, fader drag, toggle click, XYPad drag)
- Extended JS bridge (30+ functions: containers, layout, all widget types, events, themes)
- OKLCH color engine in JS (shade generation, harmony, palette system)
- JS-defined three-panel layout with hot reload
- AI prompt generation + Claude CLI invocation from JS
- Style pack save/load
- Screenshot mode for CI

### What's Broken
- TextEditor can't receive keyboard input (NSTextInputClient forwarding broken)
- ComboBox has no popup/dropdown rendering
- ScrollView doesn't exist
- No visual panel/chrome rendering (backgrounds, borders)
- No custom canvas drawing from JS (needed for color picker)

---

## Foundation Phase: Fix the View System (Phase 21)

**Goal:** Make application-level UI work in Pulp's native renderer. These are prerequisites for any design tool that isn't a toy.

### 21.1 — Keyboard Input (Critical)

**Problem:** PulpView's NSTextInputClient implementation doesn't route keystrokes to TextEditor correctly.

**Approach:**
1. Create a minimal test: single TextEditor in a WindowHost, verify keystroke handling
2. Debug the NSTextInputClient → on_key_event → on_text_input pipeline
3. Issues to investigate:
   - Is `interpretKeyEvents:` being called?
   - Is `insertText:replacementRange:` being called with the right text?
   - Is the focused view actually receiving `on_text_input`?
   - Does TextEditor's `on_key_event` handle Enter correctly to fire `on_return`?
4. Fix each break in the chain
5. Test: type text, see it appear, press Enter, verify callback fires

**Files:** `core/view/platform/mac/window_host_mac.mm`, `core/view/src/text_editor.cpp`

**Test:** `test/test_text_editor_live.cpp` — automated test that creates a TextEditor, simulates text input, verifies content

### 21.2 — Styled Panel Widget

**Problem:** No way to render a visual container with background, border, and rounding.

**Approach:**
1. Add a `Panel` widget class — subclass of View with:
   - `background_token` (string, default "bg.surface")
   - `border_token` (string, default "control.border")
   - `corner_radius` (float, default 8)
   - `border_width` (float, default 1)
2. `Panel::paint()` draws rounded rect background + border
3. Register `createPanel(id, parentId)` in WidgetBridge
4. Register `setPanelStyle(id, bgToken, borderToken, radius, borderWidth)` in WidgetBridge

**Files:** `core/view/include/pulp/view/widgets.hpp`, `core/view/src/widgets.cpp`, `core/view/src/widget_bridge.cpp`

**Test:** Screenshot comparison — panel renders with correct colors from theme

### 21.3 — ComboBox Popup Overlay

**Problem:** ComboBox `on_mouse_event` toggles internal state but nothing renders.

**Approach:**
1. Add an overlay mechanism to the View system:
   - `View::add_overlay(unique_ptr<View>)` — renders on top of all children
   - Overlays are painted last in `paint_all()`, after all normal children
   - Hit testing checks overlays first (topmost)
2. ComboBox creates an overlay View when opened containing:
   - A column of Label items
   - Click on an item selects it and removes the overlay
   - Click outside the overlay closes it
3. The overlay is positioned below the ComboBox in window coordinates

**Files:** `core/view/include/pulp/view/view.hpp`, `core/view/src/view.cpp`, `core/view/src/ui_components.cpp`

**Test:** Open ComboBox, verify items shown, click item, verify selection changes

### 21.4 — ScrollView

**Problem:** Listed in STATUS.md but doesn't exist in the codebase.

**Approach:**
1. Add `ScrollView` class:
   - Owns a single content View
   - `content_offset` (Point) — how far the content is scrolled
   - Clips children to its own bounds (already done by `paint_all`)
   - `paint_all` translates by `-content_offset` before painting children
   - Handles scroll wheel events (NSView `scrollWheel:`)
   - Optional scroll bars (visual indicators)
2. Register `createScrollView(id, parentId)` in WidgetBridge
3. Forward `scrollWheel:` from PulpView to the focused/hovered ScrollView

**Files:** `core/view/include/pulp/view/ui_components.hpp`, `core/view/src/ui_components.cpp`, `core/view/platform/mac/window_host_mac.mm`

**Test:** Create a ScrollView with content taller than the viewport, verify scrolling

### 21.5 — Canvas Drawing from JS

**Problem:** Can't draw arbitrary shapes from JS (needed for color picker gamut canvas, custom visualizations).

**Approach:**
1. Add a `CanvasView` widget — a View with a JS paint callback
2. Register `createCanvas(id, parentId)` in WidgetBridge
3. Register drawing functions that operate on the "current canvas":
   - `canvasBeginPaint(id)` — sets the target canvas
   - `canvasFillRect(x, y, w, h, colorHex)`
   - `canvasStrokeLine(x0, y0, x1, y1, colorHex, width)`
   - `canvasFillCircle(cx, cy, r, colorHex)`
   - `canvasStrokeArc(cx, cy, r, start, end, colorHex, width)`
   - `canvasFillText(text, x, y, colorHex, size)`
   - `canvasEndPaint(id)` — finalizes (stores draw commands for replay)
4. CanvasView stores a list of draw commands and replays them in `paint()`
5. Alternative: simpler approach using RecordingCanvas pattern

**Files:** New `core/view/include/pulp/view/canvas_view.hpp`, `core/view/src/canvas_view.cpp`, `core/view/src/widget_bridge.cpp`

**Test:** JS draws a gradient grid, verify it renders correctly via screenshot

---

## Design Tool Phase: Rebuild on Solid Foundation (Phase 22)

**Goal:** Rebuild the design tool using the fixed view system. All UI defined in JS, hot-reloadable.

### 22.1 — Three-Panel Layout with Working Chrome

Using the Panel widget for visual sections, TextEditor for chat input, ComboBox for dropdowns, ScrollView for message list.

```javascript
// Left panel with styled background
createPanel("left-panel", "");
setPanelStyle("left-panel", "bg.secondary", "control.border", 0, 0);

// Chat input that actually works
createTextEditor("chat-input", "right-panel");
on("chat-input", "return", function(text) { sendToAI(text); });

// Scrollable message list
createScrollView("messages", "right-panel");
// Messages added dynamically, scroll follows

// Working dropdown
createCombo("harmony", "left-panel");
// Click → popup appears → select → closes
```

### 22.2 — Color Picker with Custom Canvas

Using CanvasView for the gamut picker (2D L×C space at current hue):

```javascript
createCanvas("gamut-canvas", "color-section");
setFlex("gamut-canvas", "height", 120);

// Repaint when hue changes
function drawGamutPicker(hue) {
    canvasBeginPaint("gamut-canvas");
    for (var x = 0; x < 100; x++) {
        for (var y = 0; y < 60; y++) {
            var L = x / 100;
            var C = y / 60 * 0.35;
            if (OklchEngine.isInGamut(L, C, hue)) {
                var hex = OklchEngine.oklchToHex(L, C, hue);
                canvasFillRect(x * w/100, y * h/60, w/100 + 1, h/60 + 1, hex);
            }
        }
    }
    canvasEndPaint("gamut-canvas");
}
```

### 22.3 — Full AI Chat Integration

With working TextEditor and ScrollView, the chat becomes functional:
- Type a prompt, hit Enter
- "Generating..." status while Claude runs
- Response appears as a styled message in the scroll view
- Token changes listed below the response
- "Restore" action per message (snapshot system)

### 22.4 — Export/Import UI

Using ComboBox for format selection, TextEditor (multi-line) for preview:
- JSON, CSS, C++ Header, OKLCH CSS, WGSL
- Copy to clipboard button
- Import from file (native file dialog via a new bridge function)

---

## Plugin Preview Phase: Load Real Plugins (Phase 23)

**Goal:** Load an actual plugin's UI definition into the preview panel.

### 23.1 — Plugin Style Loading

```bash
pulp design --plugin path/to/my-plugin/
```

The tool reads the plugin's JS UI script and theme JSON. Renders the actual plugin UI in the preview panel. Theme changes apply live.

### 23.2 — Parameter Connection

If the plugin has a Processor, the design tool can instantiate it headlessly and connect parameters to the preview widgets. Turning a knob in the preview changes the actual parameter value.

### 23.3 — Side-by-Side Comparison

Split the preview area to show two themes applied to the same plugin UI. A/B comparison for design decisions.

---

## Visual Editor Phase: Layout Editing (Phase 24)

**Goal:** Unlock the canvas for spatial editing — move, resize, group widgets.

### 24.1 — Edit Mode Toggle

A toolbar button switches between "Preview" and "Edit" modes:
- **Preview mode:** Widgets are interactive (knobs turn, faders slide)
- **Edit mode:** Widgets are selectable, draggable, resizable

### 24.2 — Selection System

- Click → select single widget (blue outline)
- Shift+click → add to selection
- Rubber band drag → select multiple
- Cmd+A → select all
- Selection shows resize handles (8 points: corners + edges)

### 24.3 — Move and Resize

- Drag selected widgets → reposition
- Drag resize handles → change size
- Hold Shift → constrain to axis or maintain aspect ratio
- Arrow keys → nudge by 1px (Shift+Arrow → 10px)

### 24.4 — Snapping

- Grid snap (configurable: 4px, 8px, 16px)
- Edge snap (align to other widgets' edges)
- Center snap (align centers)
- Visual guides shown during drag

### 24.5 — Grouping

- Cmd+G → group selected widgets into a container
- Cmd+Shift+G → ungroup
- Groups are flex containers in the View tree
- Double-click group → enter group (edit children)

### 24.6 — Code Generation

When the user makes spatial changes in edit mode:
1. The tool captures the new layout as JS code
2. Shows a diff: "these lines of your JS changed"
3. User can accept → JS file is updated
4. Hot reload picks up the change → layout persists

This closes the loop: visual editing → code → hot reload → visual result.

---

## Implementation Sequence

| Phase | Name | Effort | Depends On |
|-------|------|--------|------------|
| 21.1 | Keyboard input fix | 1-2 days | — |
| 21.2 | Panel widget | Half day | — |
| 21.3 | ComboBox popup | 1-2 days | 21.2 (for overlay) |
| 21.4 | ScrollView | 1-2 days | — |
| 21.5 | Canvas from JS | 1-2 days | — |
| 22.1 | Design tool rebuild | 2-3 days | 21.1-21.5 |
| 22.2 | Color picker | 1 day | 21.5, 22.1 |
| 22.3 | AI chat | 1 day | 21.1, 21.4, 22.1 |
| 22.4 | Export/Import | 1 day | 22.1 |
| 23.1 | Plugin style loading | 1 day | 22.1 |
| 23.2 | Parameter connection | 2 days | 23.1 |
| 23.3 | Side-by-side | 1 day | 23.1 |
| 24.1 | Edit mode | 1 day | 22.1 |
| 24.2 | Selection | 2 days | 24.1 |
| 24.3 | Move/resize | 2 days | 24.2 |
| 24.4 | Snapping | 1 day | 24.3 |
| 24.5 | Grouping | 1 day | 24.3 |
| 24.6 | Code generation | 2-3 days | 24.3, 24.5 |

**Phase 21 (Foundation):** ~1 week
**Phase 22 (Design Tool):** ~1 week
**Phase 23 (Plugin Preview):** ~1 week
**Phase 24 (Visual Editor):** ~2 weeks

---

## Principles (from VISION.md)

Every decision above follows these:

1. **Permissive licensing** — all code is MIT, OKLCH engine is original (ported from our own ai-style-designer)
2. **Modular architecture** — view system fixes are in core/, design tool is in examples/, they're independent
3. **Native platform experiences** — GPU rendering via CoreGraphics/Skia, no WebView
4. **GPU-first rendering** — all widget painting through Canvas abstraction, backends swappable
5. **Modern development workflows** — JS defines UI, hot reload for iteration, AI-driven design, structured tokens

---

## Success Criteria

The design tool is ready when a developer can:

1. Run `pulp design` and see a professional three-panel app
2. Click the chat input, type "warm vintage", press Enter, and see all widgets restyle live
3. Drag the hue slider and see the entire palette regenerate in real time
4. Open a ComboBox, select an item, see it applied
5. Scroll through chat history
6. Export the theme as JSON, CSS, or C++ header
7. Load their plugin's theme and see their actual UI in the preview
8. Everything renders natively — no WebView, no browser

And in the future:
9. Unlock edit mode, drag a knob to a new position, lock, and have the JS code update
10. End-users can restyle their plugin at runtime
11. Open floating HUD panels (color + chat) that attach to a running plugin
12. Edit tokens live on your actual plugin UI without the full design tool

---

## Design HUD Phase: Floating Panels for Live Editing (Phase 25)

**Goal:** The color panel and chat panel become independent floating windows that can attach to any running Pulp plugin's view tree. Developers can tweak styles on their actual plugin during development without opening the full design tool.

### Architecture Insight

The left panel (colors/tokens) and right panel (chat/AI) communicate through **token diffs** (`applyTokenDiff`), not through direct references to preview widgets. This means they can connect to ANY Pulp view tree that uses the token system:

```
Design Tool (standalone):
  ColorPanel → applyTokenDiff → PreviewPanel ← applyTokenDiff ← ChatPanel

HUD Mode (during development):
  ColorPanel → applyTokenDiff → YourActualPlugin ← applyTokenDiff ← ChatPanel
```

### 25.1 — Decoupled Panel Architecture

Refactor the color and chat panels to be self-contained modules:
- Each panel is its own View tree + WindowHost
- Panels communicate through a shared `ThemeTarget` interface
- `ThemeTarget::apply_diff(json)` — applies token changes
- `ThemeTarget::get_theme_json()` — reads current state
- In standalone mode: ThemeTarget = the preview panel
- In HUD mode: ThemeTarget = your plugin's root View

### 25.2 — Floating Window Mode

- `pulp design --hud` launches just the color + chat panels as floating windows
- Panels are always-on-top, translucent background
- They connect to the most recently focused Pulp plugin window
- Or specify: `pulp design --hud --target "My Plugin"`

### 25.3 — Auto-Discovery

The HUD discovers running Pulp plugins via:
- Shared memory or local socket for token exchange
- Or: MCP server connection (plugin's MCP endpoint)
- Or: file watching (plugin reads theme.json, HUD writes it)

The simplest approach: file-based. HUD writes to a `.pulp-theme-live.json` file. Plugin's HotReloader watches it and applies changes. Zero infrastructure needed.

### 25.4 — Lock and Save

When the developer is happy with the tweaks:
- "Lock" button freezes the current state
- "Save to Project" exports the theme to the plugin's `theme.json`
- The plugin picks it up via hot reload — permanent change
- Or "Save as Style Pack" for reuse across projects

### Design Consideration for Phases 21-24

This future HUD mode means:
- **Panels must be self-contained** — they should NOT directly manipulate the preview's View tree. All communication through token diffs.
- **Token diff is the protocol** — every style change (color picker, AI chat, harmony selector) must produce a serializable token diff, not a direct function call.
- **WindowHost needs multi-window support** — floating panels as separate windows. The WindowHost API already supports creating multiple windows; we just need to use it.
- **No tight coupling between panels and preview** — the design-tool.js should treat left/right panels as modules that emit diffs, not as integrated parts of a single view tree.
