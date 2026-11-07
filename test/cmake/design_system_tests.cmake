# Design tool, style pack, window manager, design system, and web-compat tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# One executable for this manifest's pulp::view suites. Each member keeps its
# own registration, labels and properties (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake); only the binary behind them is shared.
pulp_add_test_group(pulp-test-group-design-system LIBRARIES pulp::view)

# Design system — pulp::design umbrella module + component catalog
pulp_add_test_suite(pulp-test-design-system GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# Sampler starter — real sampler UI built from the design catalog
pulp_add_test_suite(pulp-test-sampler-starter GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# Design-system interaction — verifies the widgets are wired (knob moves, etc.)
pulp_add_test_suite(pulp-test-design-system-interaction GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# OS appearance tracking
pulp_add_test_suite(pulp-test-appearance GROUP pulp-test-group-design-system
    SOURCES test_appearance_tracker.cpp
    LIBRARIES pulp::view)

# Splash screen lifecycle and paint behavior
pulp_add_test_suite(pulp-test-splash-screen GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# New widgets (EqCurve, MidiKeyboard, ColorPicker, FileDropZone, SplitView, PropertyList, Breadcrumb)
pulp_add_test_suite(pulp-test-phase9-widgets GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

pulp_add_test_suite(pulp-test-property-list GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# Focused SplitView and ConcertinaPanel coverage
pulp_add_test_suite(pulp-test-view-layout-widgets GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# Asset manager and resource system
pulp_add_test_suite(pulp-test-asset-manager GROUP pulp-test-group-design-system
    LIBRARIES pulp::view)

# Web-compat test suite — CSS parsing, layout, events, visual regression
add_subdirectory(web-compat)

# Palette health of a design's own colour tokens
# (tools/import-validation/check_palette_health.py) — accent-ramp structure,
# named-hue survival, and text-contrast bars, asserted in both directions. Also
# holds the shipped ink-signal pack to the bars the checker states, so the
# checker cannot drift away from the pack it judges. Pure-python, no build
# artifacts, no PIL.
if(Python3_Interpreter_FOUND)
    add_test(NAME palette-health-selftest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/import-validation/test_check_palette_health.py)
    set_tests_properties(palette-health-selftest PROPERTIES
        LABELS "import;design;palette"
        TIMEOUT 60)
endif()
