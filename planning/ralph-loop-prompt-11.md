"You are achieving feature parity between the Pulp Style Designer and the original AI Style Designer at /Users/danielraffel/Code/ai-style-designer/Tools/theme-designer.html. The original is a 5,724-line HTML/CSS/JS file. The Pulp version must match or exceed it using Pulp's native JS bridge + GPU rendering — no WebView, no browser.

CRITICAL RULE: Do NOT show the user the app or ask them to test it until you have verified through automated tests and screenshots that it matches the original. The user said: 'please do not show me this again until you have a 1:1 parity or super close to the original HTML.'

REFERENCE: The original source code is at /Users/danielraffel/Code/ai-style-designer/Tools/theme-designer.html. Read it. Study it. Port the features.
AUDIT SCREENSHOTS: planning/audit-reference/original-html.png and new-app.png show the gap.
W3C STANDARDS AUDIT: planning/w3c-standards-coverage-map.md — exhaustive coverage map with phased plan.
CSS LAYOUT SPEC: planning/css-flexbox-parity-spec.md — W3C Flexbox + Box Model properties to implement.
DESIGN TOOL PLAN: planning/design-tool-phase-plan.md has the full roadmap.

WORKING BRANCH: phase/design-tool-v2 (worktree at /Users/danielraffel/Code/pulp-design-v2)
BUILD IN: main tree (/Users/danielraffel/Code/pulp) — it has cached deps
SYNC: copy modified files between worktree and main for building

PREREQUISITE — W3C LAYOUT PARITY (Phase 1 from standards audit):
Before rebuilding the design tool JS, implement the critical missing layout features in C++ (core/view/src/view.cpp, core/view/include/pulp/view/geometry.hpp, core/view/include/pulp/view/view.hpp) and expose via JS bridge. Read planning/w3c-standards-coverage-map.md Phase 1 for the full list:

CRITICAL BUGS TO FIX FIRST:
1. justify_content is DEAD CODE — FlexStyle declares it, JS sets it, but layout_children() NEVER READS IT. Fix this first.
2. overflow: visible is impossible — hard-coded clip_rect in paint_all breaks ComboBox dropdowns and tooltips
3. No visual focus ring — paint focus indicator when has_focus_ is true

THEN IMPLEMENT:
4. justify-content: space-between / space-around / space-evenly (for toolbars, headers)
5. flex-wrap: wrap (for grids, shade ramps, button groups)
6. flex-shrink (prevent overflow on resize)
7. Per-side padding (padding_top/right/bottom/left)
8. max-width / max-height
9. Background/border on any View (not just Panel) — setBackground(id, hex), setBorder(id, hex, width, radius)
10. setTextColor(id, hex) for per-widget text color
11. setOpacity(id, float) for disabled/hover states
12. Font weight/style (replace SkFontStyle::Normal() hardcode)
13. cubic-bezier() custom easing function
14. box-shadow / drop-shadow on Canvas
15. Path builder API (begin_path, bezier_to, quad_to) for smooth curves
16. SVG path rendering (draw_svg_path for icons)
17. Clipboard in JS bridge (clipboardRead/Write)
18. Cursor style per widget (pointer/text/default)
19. prefers-reduced-motion platform query
5. max-width / max-height
6. setBackground(id, hex) and setBorder(id, hex, width, radius) on any View
7. setTextColor(id, hex) for per-widget text color
8. setOpacity(id, float) for disabled/hover states
Each with automated tests. Then the design tool JS can use these features to match the original HTML layout.

CURRENT STATE:
- WidgetBridge has 50+ JS functions (containers, layout, all widgets, properties, themes, canvas, exec)
- JS runtime preamble with on()/\_\_dispatch\_\_() for event callbacks
- clear()/snapshot_values()/restore_values() for hot reload
- TextEditor receives keyboard input (tested, 6 automated tests pass)
- Panel widget (tested, 4 automated tests pass)
- ComboBox fixed with correct tokens (tested, 5 automated tests pass)
- ScrollView with smooth scroll from animation merge (tested, 5 automated tests pass)
- CanvasWidget for JS-driven custom drawing (tested, 4 automated tests pass)
- OKLCH color engine in JS (oklch.js)
- 725 tests pass on main

THE GAP (from audit, prioritized):

PHASE A — Layout & Structure (must fix first, everything else depends on this)
1. Three-panel layout with VISIBLE PANEL BOUNDARIES (Panel widget with bg.secondary/bg.elevated backgrounds)
2. Proper flex sizing — left panel fixed 280px, right panel fixed 300px, center grows
3. Section headers with proper spacing (not scattered labels)
4. Toolbar row at top (theme name, presets, undo/redo, import/export)
5. Status bar at bottom

PHASE B — Center Panel (the preview canvas)
6. Plugin chrome container (rounded card with traffic light dots)
7. Foundations section (background swatches, text hierarchy, accent colors)
8. Controls section (knobs at 4 values, slider, buttons, toggles, text input, dropdown)
9. State previews (default/hover/disabled as separate rendered groups)

PHASE C — Left Panel (token browser + color system)
10. Token browser — list ALL theme tokens with color swatches + hex values
11. Collapsible sections for token groups (bg.*, text.*, accent.*, control.*, spacing.*, etc.)
12. Editable hex inputs per token (click swatch or edit hex → token updates live)
13. Color System UI — palette display with shade ramps
14. Hue slider + harmony selector (already exists, needs polish)

PHASE D — Right Panel (inspector + chat)
15. Two tabs: Inspector and Chat (use label buttons to toggle visibility)
16. Inspector shows component name + token list when Cmd+click on preview
17. Chat with proper message bubbles (user right-aligned, AI left-aligned)
18. Chat input with placeholder, visible border, proper height
19. Model selector in chat header
20. Snapshot thumbnails per AI response with 'Restore' button

PHASE E — Interactions & Polish
21. All dropdowns work (ComboBox click opens, select closes)
22. Text input works in chat and token hex fields
23. Keyboard shortcuts (Cmd+Z undo, Cmd+Shift+Z redo)
24. Export modal with tabs (JSON, CSS, C++ Header, OKLCH)
25. Import from file dialog

APPROACH:
- Read the original HTML code for each feature before implementing
- Use the original's CSS values (colors, sizes, spacing) as spec
- Each phase: implement → build → automated test → screenshot verify → commit
- Do NOT launch the app for the user until Phase D is complete at minimum

TESTING:
- Run ctest --exclude-regex 'AudioWorkgroup|CoreAudio' after every phase
- Use render_to_file() screenshots to verify layout before showing user
- Write automated tests for each new interaction (per CLAUDE.md requirement)

GIT:
- Commit to phase/design-tool-v2 worktree after each phase
- Clean commit messages

EACH ITERATION:
1. Read planning/ralph-loop-prompt-11.md
2. Read CLAUDE.md
3. Pick next incomplete phase
4. Study original HTML for that feature
5. Implement in design-tool.js
6. Build and test
7. Verify with screenshot
8. Commit

COMPLETION: When the design tool visually matches the original with working:
- Three-panel layout with proper chrome
- Token browser with editable hex values
- Center preview with plugin chrome and all widget sections
- Chat with message bubbles, typing indicator, restore
- All dropdowns and text inputs functional
- Export with multiple formats

Output: DESIGN TOOL PARITY ACHIEVED" --completion-promise "DONE" --max-iterations 80
