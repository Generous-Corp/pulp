"Implement Phase 6: Standards-Inspired Validation & Testing for Pulp.

References:
- planning/phase-6-standards-inspired-validation-testing.md (full spec)
- planning/phase-5-web-native-authoring-model.md (web-compat layer being tested)
- planning/w3c-css-support-matrix.md (property parity tracking)
- CLAUDE.md (build, test, architecture)

GOAL:
Build a comprehensive test suite modeled on browser engine testing practices (WPT, Blink layout tests, Gecko reftests) to validate Pulp's CSS/HTML/JS compatibility layer. Every supported CSS property, layout algorithm, event interaction, and visual output gets tested.

NON-NEGOTIABLES:
- Every test must run headlessly (no window required)
- All tests must pass on macOS; Linux and Windows baselines are tracked separately
- Visual tests use the existing render_to_file / RecordingCanvas infrastructure
- Tests use Catch2 for assertions
- No flaky tests — deterministic inputs, deterministic outputs
- Screenshot baselines committed to the repo

PHASE 6.1 — Test Infrastructure + Parser Tests (do this first):
1. Create test directory structure:
   test/web-compat/CMakeLists.txt
   test/web-compat/test_helpers.hpp — shared helpers:
     - create_test_environment(width, height) -> {root View, ScriptEngine}
     - simulate_click(root, x, y)
     - simulate_mouse_enter(root, x, y)
     - simulate_mouse_leave(root, x, y)
     - simulate_key(root, key, ctrl, shift, alt)
     - render_to_image(root, width, height) -> Image
     - compare_images(actual, expected, tolerance) -> bool
     - load_reference(name) -> Image (platform-aware path selection)
     - regression_check(name, screenshot) -> RegressionResult
     - save_baseline(name, screenshot)
     - parse_rect(json) -> Rect
   test/reftests/CMakeLists.txt
   test/reftests/scripts/ (JS test scripts)
   test/reftests/baselines/macos-arm64/ (reference PNGs)
   test/screenshots/CMakeLists.txt
   test/screenshots/baselines/
   test/fixtures/CMakeLists.txt
   test/portability/CMakeLists.txt

2. Implement image comparison:
   - Exact: 0 pixels different
   - Tight: < 0.1% different, max channel delta 2
   - Loose: < 1% different, max channel delta 5
   - Structural: SSIM > 0.99
   - Generate diff image (red overlay on differences) for debugging

3. Write parser tests (150+ tests):
   - test_css_value_parser.cpp: px, em, rem, %, auto, invalid values
   - test_css_color_parser.cpp: hex 3/6/8-digit, rgb(), rgba(), hsl(), hsla(), named colors (spot-check 20+), transparent, invalid
   - test_css_shorthand.cpp: 1/2/3/4-value expansion for margin, padding, border
   - test_css_calc.cpp: calc(100% - 20px), min(a, b), max(a, b), clamp(min, val, max), nested expressions, invalid expressions

Files to create:
- test/web-compat/CMakeLists.txt
- test/web-compat/test_helpers.hpp
- test/web-compat/test_css_value_parser.cpp
- test/web-compat/test_css_color_parser.cpp
- test/web-compat/test_css_shorthand.cpp
- test/web-compat/test_css_calc.cpp

Acceptance criteria:
- All 150+ parser tests pass
- ctest -R WebCompat runs cleanly
- Image comparison helper produces correct diff images

PHASE 6.2 — Computed Style + Layout Tests:
1. Computed style tests (50+ tests):
   - Percentage resolution: width 50% on 400px parent -> 200px
   - Inherited properties: color propagates, backgroundColor does not
   - Token resolution: var(--accent) resolves to theme value
   - em resolution: 1.5em with 16px parent -> 24px
   - Cascading: two rules match, last one wins

2. Layout tests (100+ tests):
   - Flex row: basic, justify-content (6 values), align-items (4 values)
   - Flex column: basic, alignment, nested
   - Flex item: grow, shrink, basis, align-self, order
   - Flex wrap: single-line, multi-line, gap with wrap
   - Gap: row-gap, column-gap, shorthand
   - Margin: all sides, per-side, auto centering
   - Padding: all sides, per-side, nested
   - Min/max constraints: min-width prevents shrink, max-width caps growth
   - Nested flex: 3-level nesting, mixed row/column
   - Grid (if implemented): template columns/rows, placement, auto-flow
   - Positioned: absolute with TRBL offsets, relative offset, z-index stacking
   - Overflow: hidden clips content, scroll enables scrolling
   - Layout dump comparison for complex cases (with 1px rounding tolerance)

Files to create:
- test/web-compat/test_computed_style.cpp
- test/web-compat/test_layout_flex_row.cpp
- test/web-compat/test_layout_flex_column.cpp
- test/web-compat/test_layout_flex_wrap.cpp
- test/web-compat/test_layout_grid.cpp
- test/web-compat/test_layout_position.cpp
- test/web-compat/test_layout_nested.cpp

Acceptance criteria:
- All computed style tests pass
- All layout tests pass with exact position/size matching (1px tolerance for rounding)
- Layout dump format works for complex multi-element tests

PHASE 6.3 — Event Tests:
1. Event tests (50+ tests):
   - Click: fires on correct target, event object has clientX/clientY
   - Click bubbling: child click propagates to parent with correct target
   - stopPropagation: halts bubble chain
   - Event delegation: parent listener receives child clicks with e.target
   - mouseenter/mouseleave: fire correctly, do NOT bubble
   - Hover state: :hover style applied on enter, removed on leave
   - Focus/blur: fire on focusable elements
   - :focus style: applied on focus, removed on blur
   - :active state: applied on mousedown, removed on mouseup
   - :disabled: events suppressed, style applied
   - Keyboard: keydown fires on focused element with key/code properties
   - Input: TextEditor change event fires with current value
   - preventDefault: prevents default behavior

Files to create:
- test/web-compat/test_events_click.cpp
- test/web-compat/test_events_hover.cpp
- test/web-compat/test_events_keyboard.cpp
- test/web-compat/test_events_focus.cpp
- test/web-compat/test_events_bubbling.cpp
- test/web-compat/test_selector_matching.cpp
- test/web-compat/test_element_api.cpp

Acceptance criteria:
- All event tests pass
- Bubbling order verified (child first, then parent)
- Pseudo-class style toggling verified via computed style checks

PHASE 6.4 — Visual Reftests:
1. Create 80+ reftest scripts in test/reftests/scripts/:
   - Flex layout: row center, column stretch, wrap gap, justify variations
   - Box model: margin auto, padding nested, border radius corners
   - Colors: gradients (linear, radial), opacity layers
   - Typography: font sizes, weights, alignment, line-height
   - Transforms: scale, rotate, translate, combined
   - Shadows: box-shadow with spread, offset, blur
   - Overflow: hidden clips, scroll with content
   - Transitions: mid-transition frame capture

2. Generate initial baselines:
   - Run each script headlessly at fixed size
   - Capture screenshot via render_to_image
   - Save to test/reftests/baselines/macos-arm64/
   - Manual review and approval of each baseline

3. Build reftest runner:
   - Iterates test/reftests/scripts/*.js
   - Loads script, runs layout, captures screenshot
   - Compares against platform-appropriate baseline
   - Reports pass/fail with diff image on failure

Files to create:
- test/reftests/test_reftests.cpp (runner)
- test/reftests/scripts/*.js (80+ scripts)
- test/reftests/baselines/macos-arm64/*.png

Acceptance criteria:
- 80+ reftests pass with Tight tolerance
- Diff images generated on failure for debugging
- Baselines committed and reviewed

PHASE 6.5 — Screenshot Regression + Integration Fixtures + Portability:
1. Screenshot regression framework:
   - Baseline comparison with PULP_UPDATE_BASELINES=1 env var to refresh
   - Diff image generation on failure
   - Upload artifacts in CI on failure

2. Integration fixtures (10+):
   - form-layout: labeled inputs, select, checkboxes, action buttons
   - modal-dialog: overlay, backdrop blur, centered content
   - scrolling-list: ScrollView with 100+ items
   - dashboard-grid: grid cards with meters/labels/progress
   - tabbed-panel: tab bar with active state
   - nested-flex: 3-level mixed row/column
   - audio-mixer: faders, meters, knobs, labels
   - responsive-resize: 3 window sizes
   - theme-switching: light, dark, pro-audio
   - keyboard-navigation: tab through form elements
   Each fixture has: setup.js, test.cpp (layout + visual + interaction), baseline.png

3. Portability suite (10+):
   - holy-grail.js: header/footer/sidebar/main flex layout
   - card-grid.js: grid with shadowed cards
   - nav-bar.js: horizontal nav with hover states
   - accordion.js: expandable sections
   - toast-notifications.js: positioned notifications with z-index
   - color-picker.js: canvas 2D + range inputs
   - audio-eq-curve.js: canvas 2D draggable points
   - resizable-panels.js: pointer capture drag resize
   Each tests: no JS errors, correct layout, screenshot match, interaction works

Files to create:
- test/screenshots/test_screenshot_regression.cpp
- test/fixtures/test_fixtures.cpp
- test/fixtures/*/setup.js + baseline.png (10+ fixtures)
- test/portability/test_portability.cpp
- test/portability/*.js (10+ patterns)

Acceptance criteria:
- All fixture tests pass (layout + visual + interaction)
- All portability tests run without JS errors
- Screenshot regression catches intentional and unintentional changes
- CI uploads diff artifacts on visual failures

CI INTEGRATION:
- Add ctest labels: unit, visual, web-compat, integration
- Test binaries: pulp-test-web-compat-parser, pulp-test-web-compat-layout, pulp-test-web-compat-events, pulp-test-web-compat-reftest, pulp-test-web-compat-screenshot, pulp-test-web-compat-fixture, pulp-test-web-compat-portability
- Full suite must complete in < 120 seconds on CI
- All tests run on macOS; baselines tracked per-platform for Linux/Windows

VERIFICATION AT EACH STEP:
1. Run ctest --test-dir build --output-on-failure after every change
2. Verify no existing tests broken
3. Review diff images for any visual test failures before marking as baseline
4. Keep test naming consistent: TEST_CASE sections describe the specific scenario being tested"
