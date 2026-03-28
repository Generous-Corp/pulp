"You are completing the Pulp Style Designer to match the original AI Style Designer at /Users/danielraffel/Code/ai-style-designer/Tools/theme-designer.html.

CRITICAL RULE: Do NOT show the user the app until you have verified through automated tests AND headless screenshots that it visually matches the original. Use ./build/tools/screenshot/pulp-screenshot --script examples/design-tool/design-tool.js --width 1100 --height 700 --output /tmp/pulp-verify.png to verify. Compare against planning/audit-reference/original-html.png.

WHAT IS ALREADY DONE (on main, pushed):
- W3C Flexbox Phase 1: justify-content (5 modes), flex-shrink, per-side padding, max-width/height, opacity, background/border on any View, overflow:visible, focus ring
- Extended JS bridge: 50+ functions including setBackground, setBorder, setOpacity, setTextColor, setOverflow, full setFlex with all new properties
- Widget parenting: createKnob/Fader/Toggle/Label all support parent IDs
- Three-panel layout: toolbar(44px) | main(left 310px | center flex | right 272px) | status(28px)
- Plugin chrome with traffic lights, knobs in row, fader, buttons, toggles, waveform, meters
- Token list in left panel (names only, no swatch colors yet)
- Right panel with Inspector/Chat tabs, model selector, chat input
- Screenshot tool loads JS with library support
- 736 tests pass

WHAT IS STILL MISSING (prioritized):

PHASE A — Left Panel Content
1. Token swatches need actual colors from theme. Parse getThemeJson() in JS, set swatch backgrounds.
2. Color System shade ramp: 11 OKLCH shades as colored squares per palette.
3. Accent hue slider updates swatch colors live.

PHASE B — Center Panel Content
4. Foundations section: bg color swatches + text hierarchy in actual theme colors.
5. Text input + ComboBox previews in the plugin chrome area.
6. Layout section: 2x2 card grid with Panel backgrounds.

PHASE C — Right Panel Polish
7. Chat messages as styled cards (user darker, AI lighter, role labels).
8. Snapshot per message with Restore button.
9. Inspector content on Cmd+click.

PHASE D — Interactions
10. Keyboard shortcuts (Cmd+Z undo).
11. Export/Import buttons functional.

PHASE E — UX Polish (must match original HTML behavior)
12. ScrollView: scrollbar should be grabbable with mouse click-and-drag (not just trackpad two-finger scroll). Implement mouse-down on scrollbar thumb → drag to scroll.
13. TextEditor focus state: when selected/focused, show a visible highlight border and a blinking cursor (like the original HTML input field). Reference: /Users/danielraffel/Code/ai-style-designer/Tools/theme-designer.html chat input styling.
14. TextEditor: add image upload button (camera icon) and send button (arrow icon) in the chat input area, matching the original HTML layout.
15. Inspector tab: must be clickable/selectable. Currently clicking 'Inspector' does nothing — implement tab switching between Inspector and Chat content areas.
16. TextEditor selected text: selection highlight should be tight to the actual text characters, not extend past the last letter to the end of the field. Reference screenshots: /Users/danielraffel/Desktop/selected text/
17. Hover states: interactive elements (buttons, tab labels, upload icon) should show subtle color change on hover, matching the original HTML behavior. Use the on_mouse_enter/on_mouse_leave system from the animation merge.

APPROACH: Work on main. Build, test, screenshot, commit after each phase.

TESTING:
- ctest --test-dir build --exclude-regex 'AudioWorkgroup|CoreAudio' --timeout 30
- ./build/tools/screenshot/pulp-screenshot --script examples/design-tool/design-tool.js --width 1100 --height 700 --output /tmp/pulp-verify.png

COMPLETION: When screenshot matches original with colored swatches, foundations, styled chat.
Output: DESIGN TOOL PARITY ACHIEVED" --completion-promise "DONE" --max-iterations 80
