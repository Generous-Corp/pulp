# UI Pattern Extraction from Visage

**Date:** 2026-03-26
**Type:** Clean-room observation notes
**Provenance:** Patterns observed from ~/Code/visage (MIT-licensed GPU UI framework fork)
**Rule:** No copied APIs, source code, class names, or naming patterns. Observation only.

---

## 1. Widget Geometry Patterns

### Button Sizing
- Standard button height: ~40px logical
- Title/header bar height: ~32px
- Corner rounding: ~9px for buttons, with hover effect that tightens rounding (~0.7x multiplier)

### Proportional Scaling
- Widget internal elements scale proportionally to container height
- Text sizes derived from container: title = height/2, body = height/4, detail = height/8
- Padding derived from height: height/16 for internal padding
- This approach ensures widgets look correct at any size

### Knob/Arc Rendering
- Arcs rendered with configurable center angle, start angle, and sweep
- Both flat-cap and round-cap arc variants
- Thickness as a proportion of the arc radius
- Typical sweep: 270° (matching Pulp's existing knob implementation)

### Fader/Slider Rendering
- Track as a rounded rectangle
- Thumb as a circle centered on the current value position
- Track fill from start to current value position

### Showcase Section Sizing
- Individual widget sections: ~400px wide, ~220px tall
- Arranged in a flex-wrap grid (2 columns)
- Internal padding: 16px, gap between items: 8px
- Title bar at top of each section: 32px fixed height

---

## 2. Color Token Organization

### Naming Convention
- Pattern: `[Component][Element][State]`
- Examples: ButtonBackground, ButtonBackgroundHover, ButtonText, ButtonTextHover
- Disabled states explicitly named: ButtonTextDisabled
- Action/accent variants as separate category: ActionButtonBackground

### Color Categories Observed
- **Background tiers**: primary (darkest), secondary, surface, elevated (lightest)
- **Text tiers**: primary (brightest), secondary (dimmed), disabled (dark)
- **Accent/action**: distinct color for interactive elements
- **Control elements**: track, fill, thumb, border
- **Shadow/overlay**: semi-transparent black for shadows, semi-transparent white for overlay borders

### Color Values (ARGB observations)
- Dark backgrounds: very low brightness (0x21-0x33 range per channel)
- Text: high brightness (0xDD-0xFF)
- Disabled: low-mid brightness (0x4C range)
- Hover states: slightly brighter than resting state
- Shadows: ~50% alpha black (0x88000000)

### Interactive State Colors
- 5 states observed: off, off+hover, on/active, on+hover, disabled
- Transition between states via linear interpolation (blend from/to by hover_amount 0.0-1.0)
- Hover brightens, press temporarily snaps to resting, release restores hover

---

## 3. Animation & Interaction Patterns

### Timing
- Fast interactions (click feedback): ~50ms
- Standard transitions (hover): ~80ms
- Slow transitions (detailed animations): ~240ms

### Easing
- Linear, ease-in, ease-out, ease-in-out curves available
- Standard UI interactions use ease-out for responsive feel

### Button Interaction Flow
1. Mouse enter → animate hover_amount toward 1.0
2. Hover_amount drives color interpolation between resting and hover colors
3. Mouse down → snap hover_amount to 0.0 (press feedback)
4. Mouse up → if still in bounds, animate hover_amount back toward 1.0
5. Mouse exit → animate hover_amount toward 0.0
6. Disabled → no hover animation, use static disabled colors

### Rounding as Feedback
- Corner rounding tightens on hover (multiplied by ~0.7)
- Subtle shape change adds depth to hover feedback without color change alone

---

## 4. Layout System Patterns

### Flex-Based
- CSS-like flex layout: direction, alignment, gap, padding, grow
- Supports both row and column directions
- Wrap mode for grid-like arrangements

### Dimension Units
- Logical pixels (DPI-scaled) as the primary unit
- Native pixels for exact rendering
- Viewport-relative (% of width, height, min, max)
- Arithmetic on dimensions (add, subtract, scale)

### Responsive Sizing
- Min/max constraints available
- Preferred vs. actual sizes
- Proportional distribution of remaining space among flex-grow items

---

## 5. Typography Patterns

### Font System
- Primary UI font: sans-serif (Lato observed, similar to Inter)
- Monospace variant for code/technical display
- Font loaded from embedded TTF data
- DPI scaling applied to font size

### Text Justification
- Horizontal: left, center, right
- Vertical: top, center, bottom
- Combinations for any corner or edge alignment

### Sizing Strategy
- Dynamic: scale with container (height/4, height/2)
- Fixed: specific pixel sizes for consistent UI elements (10px, 12px, 14px)

---

## 6. Rendering Architecture Patterns

### Canvas Abstraction
- Abstract interface for all drawing operations
- Multiple backends (GPU, native, recording)
- Widget code never references a specific backend
- Save/restore state stack for transforms and clips

### Layering
- Multiple render layers with z-ordering
- Layers can be packed/optimized
- Post-processing effects (blur, bloom) as shader passes

### Color Blending
- Interpolation between two colors by a float parameter (0.0-1.0)
- Gradient support (linear, with configurable direction)
- Blend modes: alpha (standard), multiply

---

## 7. Key Design Insights for Pulp's AI Designer

### What Pulp Already Has That Matches
- Theme token system with colors, dimensions, strings (matches observed pattern)
- Flex-based layout system
- Canvas abstraction with multiple backends
- Widget rendering driven by tokens

### Gaps to Consider
1. **Interactive state tokens**: Visage has per-state colors (hover, active, disabled) as separate tokens. Pulp's current 16 color tokens don't include hover/active variants — widgets hardcode these as brightness adjustments or use the same token.
2. **Animation timing tokens**: Duration values for hover, press, transition could be token-ized to allow style packs to control animation feel.
3. **Rounding as interaction feedback**: Currently Pulp's `radius.*` tokens are static. Making them respond to interaction state would add polish.
4. **Proportional widget sizing**: Pulp's widget sizes are set via layout. The proportional internal rendering (text size = height/4) is a useful pattern to adopt.

### What NOT to Import
- Component-level macro system for token registration (too coupled)
- ARGB color format (Pulp uses RGBA, which is standard)
- Specific font (Lato) — Pulp uses Inter, which is a fine choice
- Palette/brush abstraction — Pulp's flat token map is simpler and more AI-friendly

---

## Provenance Record

All observations in this document are from studying the Visage framework source code (MIT license) at ~/Code/visage. No source code was copied. No API names, class names, or implementation details are reproduced. Only general UI design patterns, sizing conventions, and architectural approaches are documented.

The patterns described here are common across many GUI frameworks (Qt, GTK, SwiftUI, CSS) and are not unique to Visage. The observation is that Visage applies them well in an audio plugin context.
