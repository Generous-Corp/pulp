# Design tool, style pack, window manager, design system, and web-compat tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Design system — pulp::design umbrella module + component catalog
pulp_add_test_suite(pulp-test-design-system LIBRARIES pulp::view)

# Sampler starter — real sampler UI built from the design catalog
pulp_add_test_suite(pulp-test-sampler-starter LIBRARIES pulp::view)

# Design-system interaction — verifies the widgets are wired (knob moves, etc.)
pulp_add_test_suite(pulp-test-design-system-interaction LIBRARIES pulp::view)

# OS appearance tracking
pulp_add_test_suite(pulp-test-appearance SOURCES test_appearance_tracker.cpp LIBRARIES pulp::view)

# Splash screen lifecycle and paint behavior
pulp_add_test_suite(pulp-test-splash-screen LIBRARIES pulp::view)

# New widgets (EqCurve, MidiKeyboard, ColorPicker, FileDropZone, SplitView, PropertyList, Breadcrumb)
pulp_add_test_suite(pulp-test-phase9-widgets LIBRARIES pulp::view)

pulp_add_test_suite(pulp-test-property-list LIBRARIES pulp::view)

# Focused SplitView and ConcertinaPanel coverage
pulp_add_test_suite(pulp-test-view-layout-widgets LIBRARIES pulp::view)

# Asset manager and resource system
pulp_add_test_suite(pulp-test-asset-manager LIBRARIES pulp::view)

# Web-compat test suite — CSS parsing, layout, events, visual regression
add_subdirectory(web-compat)
