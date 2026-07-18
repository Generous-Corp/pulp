# Design import view-widget bridge test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Reusable layout-tree parity oracle for design-import live/baked modes.
pulp_add_test_suite(pulp-test-layout-snapshot LIBRARIES pulp::view)

# Rect / IntRect layout geometry primitives: slicing, insets, unions,
# hit-testing, and the float-vs-integer center truncation that is the whole
# reason the two rect types do not share an implementation.
pulp_add_test_suite(pulp-test-view-geometry LIBRARIES pulp::view)

# CanvasWidget tests (JS-driven custom drawing)
pulp_add_test_suite(pulp-test-canvas-widget LIBRARIES pulp::view)

# CanvasWidget NaN/Infinity sanitization cluster.
# Pins that JS-supplied NaN / ±Inf coords land as 0 in the recorded
# draw command, while finite values pass through.
pulp_add_test_suite(pulp-test-canvas-widget-sanitize LIBRARIES pulp::view)

# CanvasWidget + SkiaCanvas Canvas2D shadow-state cluster. Replay +
# clear + transparent/zero short-circuit of sticky Canvas2D shadow*
# state.
pulp_add_test_suite(pulp-test-canvas-widget-shadow LIBRARIES pulp::view)

# Canvas2D JS-shim coverage. Drives the full
# web-compat-canvas.js → bridge → CanvasWidget path so a regression
# that drops a save/restore/setTransform/createLinearGradient method
# (and silently aborts FilterBank-style frame renders) gets caught.
pulp_add_test_suite(pulp-test-canvas2d-shim LIBRARIES pulp::view)

# Canvas2D _PulpCanvasMatrix DOMMatrix arithmetic — direct unit tests
# for the matrix prelude extracted to web-compat-canvas-matrix.js.
# Previously only exercised
# indirectly via ctx.getTransform() round-trip tests; this binary
# pins identity construction, mutator chain composition (not
# last-write-wins), rotateSelf degrees-not-radians, multiplySelf
# composition, inverse singular-matrix → NaN+is2D=false detection,
# toJSON honoring actual is2D state.
pulp_add_test_suite(pulp-test-canvas2d-dommatrix LIBRARIES pulp::view)

# Canvas2D shim coverage extracted from test_canvas2d_shim.cpp.
# Covers fillText / strokeText maxWidth, glyph cluster handling,
# arc-as-path fallback, ctx.direction / ctx.filter, catalog hygiene
# round-trip, lineDashOffset re-flush, and transform / hit-test APIs.
pulp_add_test_suite(pulp-test-canvas2d-shim-late LIBRARIES pulp::view)

# SvgPathWidget tests
pulp_add_test_suite(pulp-test-svg-path-widget LIBRARIES pulp::view)

# SvgRect + SvgLine widget tests
pulp_add_test_suite(pulp-test-svg-rect-widget LIBRARIES pulp::view)

# ScrollView tests
pulp_add_test_suite(pulp-test-scroll-view LIBRARIES pulp::view)

# Standalone ScrollBar widget tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-scroll-bar LIBRARIES pulp::view)

# SidePanel slide-in animation tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-side-panel LIBRARIES pulp::view)

# ComboBox dropdown interaction tests
pulp_add_test_suite(pulp-test-combo-dropdown LIBRARIES pulp::view)

# Generalized overlay-click routing (View::active_overlay_)
pulp_add_test_suite(pulp-test-overlay-routing LIBRARIES pulp::view)

# Auto-clearing input-focus slot (View::focused_input_)
pulp_add_test_suite(pulp-test-focused-input LIBRARIES pulp::view)

# Auto-claim active_overlay_ from CSS shape
# (`position:absolute` + `z-index >= 10`) or `data-overlay` author hint
# detected by the web-compat layer (web-compat-style-decl.js +
# web-compat-element.js). Uses the real WidgetBridge so the heuristic
# runs against the same prelude stack the runtime ships.
pulp_add_test_suite(pulp-test-web-compat-overlay LIBRARIES pulp::view)

# Panel widget tests (styled containers)
pulp_add_test_suite(pulp-test-panel LIBRARIES pulp::view)

# ComponentDragger — drag-to-move helper for any View.
pulp_add_test_suite(pulp-test-component-dragger LIBRARIES pulp::view)

# TooltipWindow — transient floating tooltip near cursor.
pulp_add_test_suite(pulp-test-tooltip-window LIBRARIES pulp::view)

# BubbleMessageComponent — floating message bubble anchored to a source
# view with auto-dismiss.
pulp_add_test_suite(pulp-test-bubble-message LIBRARIES pulp::view)

# PreferencesPanel — sidebar-of-pages container.
pulp_add_test_suite(pulp-test-preferences-panel LIBRARIES pulp::view)

# PropertyPanel — typed property editor sister widget. Header-only, links state
# for PropertiesFile persistence.
pulp_add_test_suite(pulp-test-property-panel
    SOURCES test_property_panel.cpp
    LIBRARIES pulp::view pulp::state)

# UI components tests (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox)
pulp_add_test_suite(pulp-test-ui-components LIBRARIES pulp::view)

# GraphEditorView tests
pulp_add_test_suite(pulp-test-graph-editor-view LIBRARIES pulp::view)
# GraphEditorView opening a node's plugin editor (first EditorAttachment consumer)
pulp_add_test_suite(pulp-test-graph-editor-open LIBRARIES pulp::view pulp::host)

# Modal overlay + ContextMenu (view-drawn popup menu) tests
pulp_add_test_suite(pulp-test-modal LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-context-menu LIBRARIES pulp::view)

# Black-box characterization harness — recovers a stock widget's implicit
# sizing law by headless measurement + a least-squares fit, and pins each
# widget's current law (ContextMenu panel width, Label intrinsic width,
# TextButton fixed height) as a regression guard.
pulp_add_test_suite(pulp-test-widget-characterization
    SOURCES test_widget_characterization.cpp support/widget_characterization.cpp
    LIBRARIES pulp::view)
# Design export tests
add_executable(pulp-test-design-export test_design_export.cpp)
target_link_libraries(pulp-test-design-export PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-export
    PROPERTIES LABELS "parser-import")
