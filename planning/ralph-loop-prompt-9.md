"You are building the Pulp AI Style Designer — a flagship demo app that proves Pulp's JS-scriptable UI architecture. The design tool is built the same way plugins are built: JS defines the UI, C++ renders it via CoreGraphics/Skia, hot reload for fast iteration.

GOVERNING RULES:
- All work happens in git worktrees, never directly on main.
- Branch: phase/ai-designer (already exists with 6 commits of foundation work).
- Build and test in the main tree (external SDKs are there), sync files to worktree for commits.
- Read ~/Code/pulp/CLAUDE.md at the start of EVERY iteration.
- Use XcodeBuildMCP or cmake CLI for building.
- Use screenshot capture (pulp-design --screenshot) to verify visual output after changes.
- Run ctest after code changes to catch regressions.

CONTEXT:
- Branch phase/ai-designer has: token_diff, style_pack, showcase, multi-style widgets (knob arc/filled/notched, toggle pill/checkbox/rocker), native macOS app with PulpDesignView + chat sidebar, model selector, Cmd+click inspector.
- The WidgetBridge (core/view/src/widget_bridge.cpp) currently exposes only 4 widget types to JS with absolute positioning. It needs massive extension.
- Widgets (Knob, Fader, Toggle) have NO mouse handlers — they don't respond to interaction.
- The Canvas API has gradient structs defined but not exposed in drawing methods.
- The original ai-style-designer at ~/Code/ai-style-designer has: OKLCH color engine, 5-palette harmony system, 7 export formats, snapshot/restore per chat message, inspector with per-token editing, style system (geometry + effects + typography + widget styles). We want feature parity and beyond.

ARCHITECTURE (approved by user):
- JS defines the UI via extended WidgetBridge
- Flat ID registry: createKnob('gain', 'parent-row') — string IDs, no object handles
- setFlex(id, key, value) for all layout properties
- on(id, event, callback) for JS event callbacks
- Clear + rebuild on hot reload with value snapshot/restore
- The design tool is a .js file loaded by ScriptEngine + HotReloader
- The C++ host is minimal: WindowHost + ScriptEngine + WidgetBridge + HotReloader

PHASE 1: Interactive Widgets
Files: core/view/src/widgets.cpp, core/view/include/pulp/view/widgets.hpp
- Add on_mouse_down/on_mouse_drag/on_mouse_up to Knob (drag vertical = change value, call on_change)
- Add on_mouse_down to Toggle (click = flip state, call on_toggle)
- Add on_mouse_down/on_mouse_drag to Fader (drag along track axis = change value, call on_change)
- Add on_mouse_event to ComboBox if not already interactive (it is — verify)
- Test: build, render screenshot, verify widgets respond to interaction in live app
- Verify: ctest passes, existing widget tests unaffected

PHASE 2: WidgetBridge — Containers + Flex
Files: core/view/src/widget_bridge.cpp, core/view/include/pulp/view/widget_bridge.hpp
- Add clear(), snapshot_values(), restore_values(), resolve_parent()
- Register createRow(id, parentId), createCol(id, parentId), createView(id, parentId)
- Register setFlex(id, key, value) dispatching on key: direction, gap, padding, flex_grow, align_items, width, height, min_width, min_height
- Register layout() calling root_.layout_children()
- Backward compatible: createKnob(id, x, y, w, h) still works when args[1] is number
- Add JS runtime preamble (injected string with __dispatch__ and on() functions)
- Test: new test file test_widget_bridge_extended.cpp with container/flex tests
- Verify: existing test_widget_bridge tests pass, ctest clean

PHASE 3: Bridge All 13 Widget Types
Files: core/view/src/widget_bridge.cpp
- Register: createMeter, createXYPad, createWaveform, createSpectrum, createCombo, createListBox, createProgress, createTabPanel, createScrollView, createTextEditor
- Register property setters: setLabel, setStyle, setOrientation, setRange, setItems, addTab, setContentSize, setProgress, setWaveformData, setSpectrumData, setMeterLevel, setXY, setFontSize
- #include <pulp/view/ui_components.hpp> and <pulp/view/text_editor.hpp>
- Test: verify each widget type creates correctly via JS
- Verify: ctest, screenshot with all widget types

PHASE 4: Event Callbacks
Files: core/view/src/widget_bridge.cpp, core/view/src/widgets.cpp
- Register on(id, eventName, callbackName) function
- Implement wire_callbacks(): install on_change/on_toggle/on_select lambdas that call engine_.invoke('__dispatch__', id, eventName, value)
- Wire: Knob::on_change, Fader::on_change, Toggle::on_toggle, ComboBox::on_change
- Test: register callback in JS, trigger via mouse or setValue, verify callback fires
- Verify: ctest

PHASE 5: Theme Control + Hot Reload
Files: core/view/src/widget_bridge.cpp, core/view/include/pulp/view/widget_bridge.hpp
- Register setTheme(name), applyTokenDiff(jsonString), getThemeJson()
- Extend HotReloader or add ThemeReloader to watch .json theme files
- Implement hot reload protocol: snapshot_values → clear → re-execute → layout → restore_values
- Test: change theme via JS, verify widgets re-render with new colors
- Verify: screenshot before/after theme change, ctest

PHASE 6: Design Tool JS App
Files: examples/design-tool/main.cpp, examples/design-tool/design-tool.js, examples/design-tool/CMakeLists.txt
- Write the C++ host: WindowHost + ScriptEngine + WidgetBridge + HotReloader watching design-tool.js
- Write design-tool.js with three-panel layout:
  - Left: widget palette (knobs in different styles, faders, toggles, meters)
  - Center: live preview canvas (waveform, spectrum, xypad, larger knobs)
  - Right: chat panel (TextEditor for input, ScrollView for messages, ComboBox for model selector)
- Style it to look professional — dark theme, good spacing, proper proportions
- Wire the AI: when user types in TextEditor and hits Enter, call claude via exec, parse diff, apply theme
- Test: launch app, verify layout, type a prompt, verify theme changes
- Take screenshot to verify visual quality
- Verify: ctest

PHASE 7: Color Picker + Palette System
Reference: ~/Code/ai-style-designer Tools/theme-designer.html OklchEngine, ShadeGenerator, ColorSystemModel
- Port the OKLCH color math: sRGB ↔ OKLab ↔ OKLCH conversion, gamut mapping, contrast ratio
- Implement shade generation: 11-step ramp (50-950) from base OKLCH color
- Implement harmony system: monochromatic, analogous, complementary, split-complementary
- Implement semantic role mapping: dark/light mode role tables mapping tokens to palette shades
- Expose via JS: createColorPicker(id, parentId), setHue/setChroma/setLightness, onColorChange
- Build a color panel widget that shows palette swatches, hue slider, gamut picker
- Wire into the design tool left panel
- Test: verify OKLCH round-trip, shade generation, harmony derivation

PHASE 8: Import/Export + Full Feature Parity
Reference: ~/Code/ai-style-designer ExportPanel (7 formats), import, undo/redo
- Add export UI: TabPanel with JSON, CSS, C++ Header, OKLCH CSS, WGSL tabs (reuse DesignExport)
- Add import: load theme JSON from file via native file dialog
- Add snapshot/restore per chat message with thumbnail preview
- Add undo/redo for token changes
- Add style pack save/load via UI buttons
- Add token diff display in chat messages
- Add state scrubber (hover/focus/disabled/error preview states)
- Add image upload in chat (attach reference image to prompt)
- Verify all features work end-to-end
- Compare side-by-side with ~/Code/ai-style-designer to confirm parity

TESTING (MANDATORY):
- Run cmake --build build --target <target> after every code change
- Run ctest --test-dir build --output-on-failure after every phase
- Use ./build/tools/design/pulp-design --screenshot --output /tmp/test.png --no-open to verify renders
- Use XcodeBuildMCP if cmake builds fail to diagnose
- Do not advance to next phase if tests fail

GIT DISCIPLINE:
- Commit after each phase with clean message
- Sync files between main tree (for building) and worktree (for committing)
- Do NOT push unless asked

EACH ITERATION MUST:
1. Read the current phase status
2. Implement the next incomplete phase
3. Build and test
4. Take a verification screenshot if visual changes were made
5. Commit to the worktree
6. Summarize what was done and what comes next

COMPLETION CONDITION:
- All 8 phases implemented
- All widgets are interactive (Knob drag, Toggle click, Fader drag)
- WidgetBridge exposes all 13 widget types with layout + events + themes
- A design-tool.js exists that creates a three-panel design app
- The design tool renders professionally and responds to AI prompts
- Color picker with OKLCH palette system works
- Import/export with at least JSON, CSS, C++ header formats works
- Snapshot/restore per chat message works
- All tests pass
- Screenshots verify visual quality
- Feature parity with ~/Code/ai-style-designer confirmed

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: DESIGN TOOL COMPLETE" --completion-promise "DONE" --max-iterations 80
