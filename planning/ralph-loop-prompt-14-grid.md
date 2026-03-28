"Implement CSS Grid Layout Level 1 for Pulp's native flex/grid engine.

Reference: https://www.w3.org/TR/css-grid-1/
Tracking: planning/w3c-css-support-matrix.md (Section 3: CSS Grid)
GitHub Issue: #23

WHAT TO IMPLEMENT:
1. Grid container type (createGrid JS bridge function)
2. grid-template-columns / grid-template-rows (parse '1fr 2fr auto 100px' syntax)
3. grid-column / grid-row (child placement)
4. grid-gap / row-gap / column-gap
5. grid-auto-flow (row/column)
6. Auto-sizing (fr units, min-content, max-content)

ARCHITECTURE:
- Add GridStyle struct to geometry.hpp alongside FlexStyle
- Add grid layout algorithm to view.cpp (separate from flex layout_children)
- Wire to JS bridge: setGrid(id, 'template_columns', '1fr 2fr 1fr')
- Grid layout computed in layout_children() when direction == grid

TESTING:
- Add tests to test/test_layout_w3c.cpp for grid behavior
- Screenshot verification of design tool token list with grid layout

FILES TO MODIFY:
- core/view/include/pulp/view/geometry.hpp (GridStyle)
- core/view/src/view.cpp (grid layout algorithm)
- core/view/src/widget_bridge.cpp (JS bridge)
- test/test_layout_w3c.cpp (tests)

EACH ITERATION: build, test, screenshot, commit.
COMPLETION: Output 'GRID LAYOUT COMPLETE' when all properties work." --completion-promise "GRID LAYOUT COMPLETE" --max-iterations 20
