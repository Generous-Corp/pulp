"Phase 2: Developer Reality Parity — make Pulp usable for real frontend development.

Reference: planning/phase-2-developer-reality-parity.md
Reference: planning/w3c-css-support-matrix.md

PRIORITY ORDER (implement sequentially):

1. FIX JS animate() — currently registered but does nothing (sets value immediately).
   Wire it to create a ValueAnimation that interpolates over duration with easing.
   Bridge: animate(id, property, target, duration, easing)
   Properties to support: opacity, scale, translate_x, translate_y, rotation

2. DISABLED STATE — add set_enabled(bool) to View.
   When disabled: block all input events, reduce opacity to 0.5, gray out.
   Bridge: setEnabled(id, bool)

3. STATE-DRIVEN STYLING — declarative hover/active/focus/disabled styles.
   Bridge: setStateStyle(id, 'hover', {background: '#xxx', scale: 1.1})
   Internally: register hover/focus callbacks that apply style changes.

4. IMAGE LOADING — load images from file paths, display in views.
   Add loadImage(path) that returns an image handle.
   Add createImage(id, parentId) widget that displays a loaded image.
   Use Skia's image decode for PNG/JPEG/WebP.

5. LAYOUT INVALIDATION — dirty flags so setFlex() triggers auto relayout.
   Currently requires manual layout() call. Add auto-invalidation.

6. DEBUG PAINT OVERLAY — setDebugPaint(true) on root outlines all view bounds.
   Draw colored rectangles for padding, margin, content areas.

7. FULL TOKEN JS BRIDGE — setColorToken(name, hex), setDimensionToken(name, value).
   Currently only setMotionToken exists.

8. LIST VIRTUALIZATION — viewport-based rendering for ListBox with >100 items.
   Only create/paint items visible in the scroll viewport.

TESTING: ctest --test-dir build --exclude-regex 'AudioWorkgroup|CoreAudio' --timeout 30
EACH ITERATION: implement one item, build, test, commit.
COMPLETION: Output 'PHASE 2 COMPLETE' when all 8 items work." --completion-promise "PHASE 2 COMPLETE" --max-iterations 120
