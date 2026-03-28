# CSS Flexbox Parity Spec for Pulp View System

**Date:** 2026-03-27
**Goal:** Pulp's JS layout API should feel natural to frontend developers familiar with CSS Flexbox. A developer should be able to take a CSS layout and reproduce it with Pulp's `setFlex()` API with minimal friction.

**Reference:** W3C CSS Flexible Box Layout Module Level 1 (Recommendation)
https://www.w3.org/TR/css-flexbox-1/

---

## Currently Implemented

| CSS Property | Pulp API | Status |
|-------------|----------|--------|
| `display: flex` | `createRow()` / `createCol()` | ✅ |
| `flex-direction: row/column` | `setFlex(id, "direction", "row"/"col")` | ✅ |
| `align-items: start/center/end/stretch` | `setFlex(id, "align_items", ...)` | ✅ |
| `justify-content: start/center/end` | `setFlex(id, "justify_content", ...)` | ✅ Partial |
| `gap` | `setFlex(id, "gap", px)` | ✅ |
| `flex-grow` | `setFlex(id, "flex_grow", n)` | ✅ |
| `padding` (uniform) | `setFlex(id, "padding", px)` | ✅ |
| `width` / `height` | `setFlex(id, "width"/"height", px)` | ✅ |
| `min-width` / `min-height` | `setFlex(id, "min_width"/"min_height", px)` | ✅ |

## Must Add (Priority Order)

### 1. justify-content: space-between / space-around / space-evenly
**Used for:** Toolbars (title left, buttons right), headers, evenly spaced grids.
**Implementation:** In `layout_children()`, after computing total fixed size, distribute remaining space between/around items instead of pushing to start.

### 2. flex-wrap: wrap
**Used for:** Grid-like layouts (color swatches, shade ramps, button groups).
**Implementation:** When children exceed main axis size, wrap to next line. Compute cross-axis height per line.

### 3. flex-shrink
**Used for:** Preventing overflow when container is smaller than total preferred sizes.
**Implementation:** When total preferred exceeds available, shrink proportionally by flex_shrink factor.
**Default:** flex_shrink = 1 (all items shrink equally).

### 4. Per-side padding
**Used for:** Different top/bottom vs left/right padding (common in panels).
**API:** `setFlex(id, "padding_top", px)`, `setFlex(id, "padding_right", px)`, etc.
**Implementation:** Change `FlexStyle::padding` from single float to 4 floats.

### 5. margin (per-widget spacing)
**Used for:** Spacing individual widgets differently from the container gap.
**API:** `setFlex(id, "margin", px)` or `setFlex(id, "margin_top", px)`, etc.
**Implementation:** Add margin to FlexStyle, apply in layout_children() as extra space around each child.

### 6. max-width / max-height
**Used for:** Constraining growth (e.g., sidebar max-width, chat input max-height).
**API:** `setFlex(id, "max_width", px)`, `setFlex(id, "max_height", px)`
**Implementation:** Clamp computed size to max after flex distribution.

### 7. opacity
**Used for:** Disabled states (0.5 opacity), hover effects, fade animations.
**API:** `setOpacity(id, 0.0-1.0)`
**Implementation:** In `paint_all()`, set CGContext/Skia global alpha before painting.

### 8. background-color and border on any View
**Used for:** Section backgrounds, cards, dividers — not just Panel.
**API:** `setBackground(id, "#hex")`, `setBorder(id, "#hex", width, radius)`
**Implementation:** Add optional bg_color/border_color/border_width/corner_radius to View. Paint in View::paint() if set.

### 9. align-self (per-child override)
**Used for:** One child aligned differently from siblings.
**API:** `setFlex(id, "align_self", "start"/"center"/"end"/"stretch")`
**Implementation:** Check child's align_self before parent's align_items in layout_children().

### 10. Per-widget text color
**Used for:** Status text (green/red), accent-colored labels, disabled text.
**API:** `setTextColor(id, "#hex")`
**Implementation:** Override resolve_color("text.primary") with explicit color if set.

## Should Also Implement (Completeness)

### 11. flex-basis
**W3C:** Initial main size of a flex item before free space distribution.
**API:** `setFlex(id, "flex_basis", px)` — use instead of preferred_width/height on main axis.

### 12. order
**W3C:** Controls visual order without changing DOM/tree order.
**API:** `setFlex(id, "order", n)` — sort children by order before layout.

### 13. row-gap / column-gap (separate)
**W3C Level 1 added these.** Currently we only have uniform `gap`.
**API:** `setFlex(id, "row_gap", px)`, `setFlex(id, "column_gap", px)`

### 14. overflow: visible / hidden / scroll / auto
**API:** `setOverflow(id, "hidden"/"scroll"/"auto"/"visible")`
**Default:** hidden (current clip_rect behavior). "scroll" = ScrollView. "visible" = no clip.

### 15. cursor style
**API:** `setCursor(id, "pointer"/"text"/"default"/"grab")`
**Implementation:** Change NSCursor on mouse enter/leave for the hovered view.

### 16. visibility: hidden (vs display: none)
**API:** `setFlex(id, "visibility", "hidden")` — takes space but doesn't paint.
**vs** `setVisible(id, false)` which removes from layout entirely.

## Defer (Not Needed Yet)

- CSS Grid — flex-wrap covers most grid use cases; add if demand proves otherwise
- `position: absolute/fixed` — overlay system handles modals/dropdowns
- `transform` — not needed for layout (rotation/scale for effects only)
- `@media` queries — app controls its own size directly
- CSS selectors / cascade — we use direct property setting, not selector matching
- `float` — obsolete layout technique
- `box-sizing` — always border-box (simpler)

## W3C References

- CSS Flexible Box Layout Module Level 1: https://www.w3.org/TR/css-flexbox-1/
- CSS Box Model Module Level 3: https://www.w3.org/TR/css-box-3/
- CSS Overflow Module Level 3: https://www.w3.org/TR/css-overflow-3/
- CSS Color Module Level 4: https://www.w3.org/TR/css-color-4/ (OKLCH support)

## Testing Strategy

Each new layout feature gets automated tests:
```cpp
// Example: space-between test
View root;
root.flex().direction = FlexDirection::row;
root.flex().justify_content = FlexJustify::space_between;
root.set_bounds({0, 0, 300, 50});
// Add 3 children, each 50px wide
// After layout: child0 at x=0, child1 at x=125, child2 at x=250
REQUIRE(root.child_at(0)->bounds().x == 0.0f);
REQUIRE(root.child_at(2)->bounds().x == 250.0f);
```

## Integration with JS Bridge

All new properties exposed via `setFlex()` using the existing key-value pattern:
```javascript
setFlex("toolbar", "justify_content", "space-between");
setFlex("grid", "flex_wrap", "wrap");
setFlex("sidebar", "max_width", 320);
setFlex("panel", "padding_top", 12);
setFlex("panel", "padding_bottom", 8);
setOpacity("disabled-label", 0.5);
setBackground("section", "#2a2a3c");
setBorder("card", "#585b70", 1, 8);
setTextColor("error-msg", "#f38ba8");
```

This gives frontend developers a familiar, CSS-like API without the full CSS cascade.
