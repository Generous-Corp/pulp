"Phase 3: Platform Maturity — production-ready, cross-platform, accessible.

Reference: planning/phase-3-platform-maturity-parity.md

PRIORITY ORDER:

1. macOS CURSOR MANAGEMENT — connect View::cursor() enum to NSCursor.
   In mouseMoved handler, read hovered view's cursor style and call
   [[NSCursor arrowCursor/pointingHandCursor/crosshairCursor/IBeamCursor] set].

2. TAB FOCUS NAVIGATION — wire Tab key in window host keyDown to call
   View::focus_next(root, current). Shift+Tab calls focus_prev.

3. macOS ACCESSIBILITY (VoiceOver) — implement NSAccessibility protocol on PulpView.
   Map View::access_role to NSAccessibilityRole (slider, toggle, label, etc).
   Map access_label to accessibilityLabel, access_value to accessibilityValue.
   Announce focus changes via NSAccessibilityPostNotification.

4. IME COMPOSITION — implement setMarkedText/unmarkText in PulpView NSTextInputClient.
   Track marked text range in TextEditor. Render marked text with underline.
   Support firstRectForCharacterRange for IME candidate window positioning.

5. VISUAL REGRESSION CI — add test that renders screenshot and compares against golden.
   Use render_to_file() for headless PNG. Add pixel-diff comparison.
   Store golden images in test/golden/. CI fails if diff exceeds threshold.

6. RIGHT-CLICK CONTEXT MENU — add on_context_menu callback to View.
   Wire rightMouseDown in window host. Create ContextMenu widget (list of actions).
   Bridge: on(id, 'contextmenu', fn), showContextMenu(items, x, y)

7. KEYBOARD SHORTCUTS SYSTEM — global shortcut registration beyond TextEditor.
   Bridge: registerShortcut('cmd+z', fn), registerShortcut('cmd+shift+z', fn)
   Route through window host keyDown before widget dispatch.

8. FILE DIALOG INTEGRATION — native open/save dialog wrappers.
   Bridge: showOpenDialog({types, multiple}) -> Promise-like callback with paths.
   macOS: NSOpenPanel/NSSavePanel. Cross-platform via SDL3 later.

TESTING: ctest --test-dir build --exclude-regex 'AudioWorkgroup|CoreAudio' --timeout 30
EACH ITERATION: implement one item, build, test, commit.
COMPLETION: Output 'PHASE 3 COMPLETE' when all 8 items work." --completion-promise "PHASE 3 COMPLETE" --max-iterations 120
