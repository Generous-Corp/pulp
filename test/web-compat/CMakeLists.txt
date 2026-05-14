# Web-compat test suite — CSS parsing, layout, events, visual regression

set(WEBCOMPAT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# CSS value parser tests
add_executable(pulp-test-web-compat-parser
    test_css_value_parser.cpp
    test_css_color_parser.cpp
    test_css_shorthand.cpp
    test_css_calc.cpp
)
target_link_libraries(pulp-test-web-compat-parser PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-compat-parser PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-web-compat-parser PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
catch_discover_tests(pulp-test-web-compat-parser)

# Computed style + layout tests
add_executable(pulp-test-web-compat-layout
    test_computed_style.cpp
    test_layout_flex_row.cpp
    test_layout_flex_column.cpp
    test_layout_flex_wrap.cpp
    test_layout_grid.cpp
    test_layout_position.cpp
    test_layout_nested.cpp
    test_layout_aspect_ratio.cpp
)
target_link_libraries(pulp-test-web-compat-layout PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-compat-layout PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-web-compat-layout PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
# pulp #1434 — test_layout_aspect_ratio.cpp exercises the CSS shim via
# document.createElement + appendChild, which (like the events / prelude
# targets) deep-recurses through QuickJS↔C++ DOM ops. Match the prelude
# stack size so the CSS-shim end-to-end tests don't blow the default.
if(APPLE)
    target_link_options(pulp-test-web-compat-layout PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-web-compat-layout PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-web-compat-layout)

# Event tests
add_executable(pulp-test-web-compat-events
    test_events_click.cpp
    test_events_hover.cpp
    test_events_keyboard.cpp
    test_events_focus.cpp
    test_events_bubbling.cpp
    test_selector_matching.cpp
    test_element_api.cpp
    test_css_hover_translation.cpp
)
target_link_libraries(pulp-test-web-compat-events PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-compat-events PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-web-compat-events PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
# pulp #1323 — CSS-hover tests do appendChild + StyleSheet attach;
# match the prelude target's 16 MB stack so QuickJS↔C++ interleaving
# inside _processStyleElement → _applyTo doesn't blow the default.
if(APPLE)
    target_link_options(pulp-test-web-compat-events PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-web-compat-events PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-web-compat-events)

# Web-compat prelude tests (document, Element, style, parser, StyleSheet)
add_executable(pulp-test-web-compat-prelude
    test_web_compat_prelude.cpp
)
target_link_libraries(pulp-test-web-compat-prelude PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-compat-prelude PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-web-compat-prelude PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
# Increase stack size to 16MB for deep QuickJS↔C++ interleaving in DOM tests
if(APPLE)
    target_link_options(pulp-test-web-compat-prelude PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-web-compat-prelude PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-web-compat-prelude)

# Visual reftest runner (screenshot comparison against baselines)
if(APPLE)
    add_executable(pulp-test-web-compat-reftest
        test_reftests.cpp
    )
    target_link_libraries(pulp-test-web-compat-reftest PRIVATE pulp::view Catch2::Catch2WithMain)
    target_include_directories(pulp-test-web-compat-reftest PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
    target_compile_definitions(pulp-test-web-compat-reftest PRIVATE
        PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
    )
    catch_discover_tests(pulp-test-web-compat-reftest)

    # Screenshot regression + integration fixtures + portability tests
    add_executable(pulp-test-web-compat-fixture
        test_screenshot_regression.cpp
        test_fixtures.cpp
        test_portability.cpp
    )
    target_link_libraries(pulp-test-web-compat-fixture PRIVATE pulp::view Catch2::Catch2WithMain)
    target_include_directories(pulp-test-web-compat-fixture PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
    target_compile_definitions(pulp-test-web-compat-fixture PRIVATE
        PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
    )
    catch_discover_tests(pulp-test-web-compat-fixture)
endif()

# Minimal appendChild reproduction test
add_executable(pulp-test-minimal-append test_minimal_append.cpp)
target_link_libraries(pulp-test-minimal-append PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-minimal-append PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
if(APPLE)
    target_link_options(pulp-test-minimal-append PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-minimal-append PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-minimal-append)

# #542 regression: headless loader loops must flush requestAnimationFrame
# callbacks, not just microtasks, or three.webgpu.js-style demos hang at
# status: 'starting' on the --capture path.
add_executable(pulp-test-headless-frame-pump test_headless_frame_pump.cpp)
target_link_libraries(pulp-test-headless-frame-pump PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-headless-frame-pump PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
if(APPLE)
    target_link_options(pulp-test-headless-frame-pump PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-headless-frame-pump PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-headless-frame-pump)

# pulp #1434 batch 3 — CSS translator gap fills: backdropFilter,
# textDecoration{Line,Color,Style}, fontWeight keyword translation.
# Exercises the JS shim → bridge round trip via
# document.createElement / appendChild / style.X = ... — same DOM-shim
# stack-depth profile as the prelude / aspect-ratio targets, so match
# the 16 MB stack the rest of the web-compat suite uses.
add_executable(pulp-test-css-translator-gaps-1434 test_css_translator_gaps_1434.cpp)
target_link_libraries(pulp-test-css-translator-gaps-1434 PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-css-translator-gaps-1434 PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-css-translator-gaps-1434 PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
if(APPLE)
    target_link_options(pulp-test-css-translator-gaps-1434 PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-css-translator-gaps-1434 PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-css-translator-gaps-1434)

# #1147 regression: nested-flex popover row template — inline <svg> with
# width/height attributes must reserve layout space, and a <span> with
# nested flex children must lay out those children instead of clobbering
# them with the parent's text-only intrinsic measurement.
add_executable(pulp-test-popover-row-template-1147 test_popover_row_template_1147.cpp)
target_link_libraries(pulp-test-popover-row-template-1147 PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-popover-row-template-1147 PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-popover-row-template-1147 PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
if(APPLE)
    target_link_options(pulp-test-popover-row-template-1147 PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-popover-row-template-1147 PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-popover-row-template-1147)

# pulp #1926 — SVG primitives other than <path> (rect / line / circle)
# routing via the web-compat shim + __domAppend fast path + attribute
# replay. Exercises document.createElement('rect'|'line'|'circle')
# round-trips through the bridge — same DOM-shim stack-depth profile as
# the rest of the web-compat suite, so match the 16 MB stack.
add_executable(pulp-test-svg-primitives-1926 test_svg_primitives_1926.cpp)
target_link_libraries(pulp-test-svg-primitives-1926 PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-svg-primitives-1926 PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-svg-primitives-1926 PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
if(APPLE)
    target_link_options(pulp-test-svg-primitives-1926 PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-svg-primitives-1926 PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-svg-primitives-1926)

# #1899 regression: Spectr import-flow gaps #4 + #5 — `<path>` must
# route to SvgPathWidget with d / stroke / stroke-width / fill /
# viewBox replay, and `<input type=range>` must default to horizontal.
add_executable(pulp-test-svg-path-range-orientation-1899 test_svg_path_range_orientation_1899.cpp)
target_link_libraries(pulp-test-svg-path-range-orientation-1899 PRIVATE pulp::view Catch2::Catch2WithMain)
target_include_directories(pulp-test-svg-path-range-orientation-1899 PRIVATE ${WEBCOMPAT_INCLUDE_DIR})
target_compile_definitions(pulp-test-svg-path-range-orientation-1899 PRIVATE
    PULP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}/test"
)
if(APPLE)
    target_link_options(pulp-test-svg-path-range-orientation-1899 PRIVATE -Wl,-stack_size,0x2000000)
elseif(UNIX)
    target_link_options(pulp-test-svg-path-range-orientation-1899 PRIVATE -Wl,-z,stacksize=16777216)
endif()
catch_discover_tests(pulp-test-svg-path-range-orientation-1899)
