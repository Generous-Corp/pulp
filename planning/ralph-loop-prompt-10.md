"You are executing Phase 21: View System Foundation for the Pulp Design Tool. This phase fixes the core view system so that application-level UI (text input, dropdowns, scrolling, styled panels) works natively — no WebView, no browser, pure Pulp GPU rendering driven by JS.

GOVERNING RULES:
- All work happens on the phase/ai-designer branch (already has 16 commits of prior work).
- Build in the main tree (external SDKs are there), sync to worktree for commits.
- Read ~/Code/pulp/CLAUDE.md at the start of EVERY iteration.
- Run ctest after code changes. Use screenshots to verify visual output.
- Each sub-phase gets its own commit with clean message.

CONTEXT:
- Branch phase/ai-designer has: extended WidgetBridge (30+ JS functions), interactive widgets (knob/fader/toggle/xypad), multi-style rendering, OKLCH color engine in JS, smooth waveform (continuous path), design-tool.js with three-panel layout.
- BROKEN: TextEditor can't receive keyboard input. ComboBox has no popup. ScrollView doesn't exist. No styled panel containers. No custom canvas drawing from JS.
- The design tool phase plan is at planning/design-tool-phase-plan.md.
- The architecture: JS defines UI → QuickJS → View tree → Canvas → CoreGraphics/Skia. No WebView.
- CRITICAL DESIGN PRINCIPLE: Panels (color/chat) communicate through token diffs, not direct view references. This enables future HUD mode where panels attach to a running plugin.

PHASE 21.1 — Fix Keyboard Input (CRITICAL BLOCKER)

The TextEditor widget has full keyboard handling (on_key_event, on_text_input) but PulpView's NSTextInputClient forwarding doesn't work. Debug and fix this.

Approach:
1. Create a MINIMAL test: single TextEditor in a WindowHost, nothing else
   - File: examples/text-input-test/main.cpp (temporary test app)
   - Just: View root + TextEditor + WindowHost::create + run_event_loop
2. Debug the chain: NSView keyDown → interpretKeyEvents → insertText:replacementRange: → on_text_input
   - Add printf/NSLog at each step to trace where it breaks
   - Check: is interpretKeyEvents being called? Is insertText being called?
   - Check: is _focusedView set correctly on mouse click?
   - Check: does TextEditor::on_text_input actually insert text?
   - Check: does TextEditor::on_key_event handle Enter → on_return?
3. Fix each break in the chain
4. Verify: type text, see it appear in the TextEditor, press Enter, verify on_return fires
5. Verify: the design-tool.js chat input works (type + Enter → sends to AI)

Files: core/view/platform/mac/window_host_mac.mm, core/view/src/text_editor.cpp
Test: automated test that simulates text input and verifies content

PHASE 21.2 — Panel Widget (Styled Containers)

Add a Panel widget — a View with a painted background, optional border, and corner rounding. This gives us visual sections and chrome.

Implementation:
1. Add Panel class to widgets.hpp:
   - background_token (string, default "bg.surface")
   - border_token (string, default "control.border")
   - corner_radius (float, default 8)
   - border_width (float, default 1)
   - Panel::paint() draws rounded rect bg + border using resolve_color()
2. Register createPanel(id, parentId) in WidgetBridge
3. Register setPanelStyle(id, bgToken, borderToken, radius, borderWidth) in WidgetBridge

Files: core/view/include/pulp/view/widgets.hpp, core/view/src/widgets.cpp, core/view/src/widget_bridge.cpp
Test: screenshot showing panel with visible background/border

PHASE 21.3 — ComboBox Popup Overlay

The ComboBox click handler toggles internal state but nothing renders. Add an overlay mechanism.

Implementation:
1. Add overlay support to View:
   - View::show_overlay(unique_ptr<View>) — rendered on top of all children
   - View::dismiss_overlay() — removes it
   - paint_all() paints overlays last
   - hit_test() checks overlays first
2. ComboBox::on_mouse_event creates an overlay containing:
   - A column of clickable Label items
   - Click on item → set_selected, dismiss overlay
   - Click outside → dismiss overlay
3. The overlay is positioned below the ComboBox, full-width

Files: core/view/include/pulp/view/view.hpp, core/view/src/view.cpp, core/view/src/ui_components.cpp
Test: click ComboBox → items appear → click item → selection changes

PHASE 21.4 — ScrollView

Implement a basic scroll container.

Implementation:
1. Add ScrollView class to ui_components:
   - Owns content as a child View
   - scroll_offset (float) — vertical scroll position
   - paint_all overrides: translate by -scroll_offset, clip to bounds
   - Handles scroll wheel events
   - Optional scroll bar indicator
2. Register createScrollView(id, parentId) in WidgetBridge
3. Forward scrollWheel: from PulpView to the hovered ScrollView
4. Add setScrollContent(scrollViewId, contentId) to bridge — reparent a view into the scroll view

Files: core/view/include/pulp/view/ui_components.hpp, core/view/src/ui_components.cpp, core/view/platform/mac/window_host_mac.mm
Test: create a tall column of labels in a scroll view, verify scrolling works

PHASE 21.5 — Canvas Drawing from JS

For the color picker gamut canvas and custom visualizations, JS needs to be able to draw arbitrary shapes.

Implementation:
1. Add CanvasWidget — a View that replays recorded draw commands in paint()
2. Register createCanvas(id, parentId) in WidgetBridge
3. Register drawing functions:
   - canvasBegin(id) — start recording
   - canvasRect(x, y, w, h, fillHex) — filled rect
   - canvasCircle(cx, cy, r, fillHex) — filled circle
   - canvasLine(x0, y0, x1, y1, strokeHex, width) — stroked line
   - canvasText(text, x, y, fillHex, size) — text
   - canvasEnd(id) — finalize (store commands, trigger repaint)
4. CanvasWidget stores a vector of DrawCommands and replays in paint()
5. This is similar to RecordingCanvas but driven from JS

Files: new core/view/include/pulp/view/canvas_widget.hpp, new core/view/src/canvas_widget.cpp, core/view/src/widget_bridge.cpp
Test: JS draws a gradient grid, verify rendering via screenshot

AFTER ALL 21.x ARE DONE:

Rebuild the design-tool.js using the fixed components:
- Panels for visual sections (left panel background, right panel background)
- TextEditor for working chat input
- ComboBox with actual dropdown popup
- ScrollView for chat message history
- CanvasWidget for color picker gamut

Verify all success criteria from planning/design-tool-phase-plan.md:
1. Type in chat, press Enter → AI responds
2. ComboBox dropdown opens and closes
3. Chat messages scroll
4. Styled panels with backgrounds
5. Color picker renders custom canvas

TESTING (MANDATORY):
- Build after every code change
- Run ctest to catch regressions
- Use screenshot capture to verify visual output
- Test keyboard input with a minimal standalone app first
- Do not advance to next sub-phase if current sub-phase doesn't work

GIT DISCIPLINE:
- Commit after each sub-phase (21.1, 21.2, 21.3, 21.4, 21.5)
- Clean commit messages explaining what was fixed and why
- Sync between main tree (build) and worktree (commit)

EACH ITERATION MUST:
1. Read the current sub-phase status
2. Implement the next incomplete sub-phase
3. Build and test (ctest + screenshot if visual)
4. Commit to the worktree
5. Summarize what was done and what comes next

COMPLETION CONDITION:
- All 5 sub-phases (21.1-21.5) are implemented and tested
- TextEditor receives keyboard input and fires on_return
- ComboBox shows a popup dropdown on click
- ScrollView scrolls content
- Panel renders with background/border
- CanvasWidget draws shapes from JS
- The design-tool.js is rebuilt using these components and works end-to-end
- All tests pass

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: PHASE 21 COMPLETE" --completion-promise "DONE" --max-iterations 80
