set(PULP_DELAY_UI_SOURCES
    pulp_delay_controls.cpp
    pulp_delay_editor.cpp
)

set(PULP_DELAY_PLUGIN_SOURCES
    delay_params.cpp
    delay_time_model.cpp
    character_engine_bank.cpp
    pulp_delay.cpp
    ${PULP_DELAY_UI_SOURCES}
    pulp_delay_view.cpp
)

if(PULP_BUILD_TESTS)
    add_executable(pulp-delay-test
        test_pulp_delay.cpp
        test_pulp_delay_ui.cpp
        ${PULP_DELAY_PLUGIN_SOURCES}
    )
    target_link_libraries(pulp-delay-test PRIVATE
        pulp::audio-analysis pulp::format pulp::signal pulp::view Catch2::Catch2WithMain)
    target_include_directories(pulp-delay-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Keep hardware-sensitive timing probes out of the required correctness
    # lane. Catch tags are not CTest labels, so discover the two groups
    # separately and label the performance cases explicitly.
    catch_discover_tests(pulp-delay-test
        TEST_SPEC "~[performance]"
        TEST_LIST PULP_DELAY_CORRECTNESS_TESTS)
    catch_discover_tests(pulp-delay-test
        TEST_SPEC "[performance]"
        TEST_PREFIX "performance: "
        PROPERTIES LABELS performance
        TEST_LIST PULP_DELAY_PERFORMANCE_TESTS)

    add_executable(pulp-delay-ui-shots
        pulp_delay_shots.cpp
        delay_params.cpp
        delay_time_model.cpp
        ${PULP_DELAY_UI_SOURCES}
    )
    target_link_libraries(pulp-delay-ui-shots PRIVATE pulp::view)
    target_include_directories(pulp-delay-ui-shots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

set(PULP_DELAY_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND PULP_DELAY_FORMATS AU)
endif()

pulp_add_plugin(PulpDelay
    FORMATS             ${PULP_DELAY_FORMATS}
    PLUGIN_NAME         "Pulp Delay"
    BUNDLE_ID           "com.pulp.delay"
    MANUFACTURER        "Pulp"
    VERSION             "1.0.0"
    CATEGORY            Effect
    PLUGIN_CODE         "PDly"
    MANUFACTURER_CODE   "Pulp"
    DESIGN_WIDTH        1120
    DESIGN_HEIGHT       740
    SOURCES
        ${PULP_DELAY_PLUGIN_SOURCES}
)

if(TARGET PulpDelay_Core)
    target_link_libraries(PulpDelay_Core PUBLIC pulp::signal pulp::view)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PulpDelay
        COMMAND bash -c
            "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Pulp Delay.clap/Contents/MacOS/Pulp Delay'); print('OK')\"")
    set_tests_properties(clap-dlopen-PulpDelay PROPERTIES
        LABELS "validation;clap"
        TIMEOUT 10)
endif()
