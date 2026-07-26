# PulpPluck — Karplus-Strong plucked-string synthesizer
# Processor source is reused by AU v2, VST3, CLAP, and the WAM browser demo

pulp_add_plugin(PulpPluck
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "PulpPluck"
    BUNDLE_ID       "com.pulp.pluck"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PPlk"
    MANUFACTURER_CODE "Pulp"
)

# Processor tests
if(PULP_BUILD_TESTS)
    add_executable(pulp-pluck-test test_pulp_pluck.cpp)
    target_link_libraries(pulp-pluck-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-pluck-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-pluck-test)
endif()

# CLAP dlopen test
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpPluck
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpPluck.clap/Contents/MacOS/PulpPluck'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpPluck PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# auval (AU v2)
if(APPLE AND PULP_HAS_AUSDK)
    add_test(
        NAME auval-PulpPluck
        COMMAND ${CMAKE_COMMAND} -E env
            bash "${CMAKE_SOURCE_DIR}/tools/ci/run-auval-component.sh"
                "${CMAKE_BINARY_DIR}/AU/PulpPluck.component" aumu PPlk Pulp
    )
    set_tests_properties(auval-PulpPluck PROPERTIES
        LABELS "validation;au"
        TIMEOUT 90
        RESOURCE_LOCK pulp_au_discovery
        ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
endif()
