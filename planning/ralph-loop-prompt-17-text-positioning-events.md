"Implement CSS Text L3 + Positioned Layout L3 + DOM Events parity for Pulp.

References:
- https://www.w3.org/TR/css-text-3/
- https://www.w3.org/TR/css-text-decor-4/
- https://www.w3.org/TR/css-position-3/
- DOM Events specification
Tracking: planning/w3c-css-support-matrix.md
GitHub Issues: #27 (Positioning), #28 (Text), #34 (Events)

TEXT PROPERTIES TO IMPLEMENT:
1. text-transform: uppercase / lowercase / capitalize
2. text-decoration: underline / line-through / overline
3. text-decoration-color / text-decoration-style / text-decoration-thickness
4. white-space: nowrap / pre / pre-wrap / normal
5. word-break: break-all / keep-all
6. overflow-wrap: break-word
7. text-indent
8. text-shadow (offset, blur, color)
9. font-family bridge (select from available system fonts)

POSITIONED LAYOUT TO IMPLEMENT:
1. position: relative (offset without removing from flow)
2. position: absolute (positioned relative to nearest positioned ancestor)
3. position: fixed (positioned relative to viewport/root)
4. position: sticky (sticks during scroll within parent)
5. top / right / bottom / left offset properties
6. z-index (explicit stacking order)
7. Stacking context creation rules

DOM EVENTS TO IMPLEMENT:
1. mousemove per-widget (continuous tracking)
2. mousedown / mouseup (separate from click)
3. keydown / keyup per-focused-widget (not just global)
4. dblclick (double-click event)
5. contextmenu (right-click)
6. wheel per-widget (not just ScrollView)
7. resize (widget size change notification)

ARCHITECTURE:
- Text: add text_transform_, text_decoration_, white_space_ to Label, render in paint()
- Position: add PositionStyle to View (position type + offsets), apply in layout_children as post-layout offset
- Absolute/fixed: render in overlay queue like ComboBox dropdown
- Sticky: track scroll offset, switch between relative and fixed
- Events: extend wire_callbacks pattern, add per-widget on_mouse_move/down/up callbacks

JS BRIDGE:
- setTextTransform(id, 'uppercase'), setTextDecoration(id, 'underline')
- setPosition(id, 'absolute'), setTop/Right/Bottom/Left(id, px), setZIndex(id, n)
- on(id, 'mousemove', fn), on(id, 'mousedown', fn), on(id, 'keydown', fn)

TESTING:
- Text transform: unit test uppercase/lowercase
- Position: layout test for relative offset, absolute positioning
- Events: headless event dispatch tests

EACH ITERATION: build, test, commit.
COMPLETION: Output 'TEXT POSITIONING EVENTS COMPLETE'" --completion-promise "TEXT POSITIONING EVENTS COMPLETE" --max-iterations 25
