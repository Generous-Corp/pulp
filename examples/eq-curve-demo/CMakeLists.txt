# EqCurveDemo — four-band parametric EQ with the interactive EqCurveView editor.
# Showcases that the drawn response curve and the processed audio share one
# description of the filter.

if(PULP_BUILD_TESTS)
    add_executable(eq-curve-demo-test test_eq_curve_demo.cpp)
    # pulp::view (with a live editor, so PULP_HEADLESS is OFF) lets the test drive
    # the actual editor and confirm a drag writes the plugin's parameters.
    target_link_libraries(eq-curve-demo-test PRIVATE pulp::format pulp::signal pulp::view Catch2::Catch2WithMain)
    target_include_directories(eq-curve-demo-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(eq-curve-demo-test)
endif()

set(EQ_CURVE_DEMO_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND EQ_CURVE_DEMO_FORMATS AU)
endif()

pulp_add_plugin(EqCurveDemo
    FORMATS           ${EQ_CURVE_DEMO_FORMATS}
    PLUGIN_NAME       "EQ Curve Demo"
    BUNDLE_ID         "com.pulp.examples.eq-curve-demo"
    MANUFACTURER      "Pulp Examples"
    VERSION           "0.1.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PEqC"
    MANUFACTURER_CODE "PlpX"
)
