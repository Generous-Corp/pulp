if(PULP_BUILD_TESTS)
    add_executable(timeline-plugin-proof-test test_timeline_plugin_proof.cpp)
    target_link_libraries(timeline-plugin-proof-test PRIVATE
        pulp::format pulp::timeline pulp::timeline-editor pulp::timeline-view
        pulp::view Catch2::Catch2WithMain)
    target_include_directories(timeline-plugin-proof-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(timeline-plugin-proof-test)
endif()

pulp_add_plugin(TimelinePluginProof
    FORMATS           CLAP
    PLUGIN_NAME       "Timeline Plugin Proof"
    BUNDLE_ID         "com.pulp.examples.timeline-plugin-proof"
    MANUFACTURER      "Pulp Examples"
    VERSION           "0.1.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PTpf"
    MANUFACTURER_CODE "PlpX"
)
target_link_libraries(TimelinePluginProof_Core INTERFACE
    pulp::timeline pulp::timeline-editor pulp::timeline-view pulp::view)

if(TARGET TimelinePluginProof_CLAP AND COMMAND pulp_assert_link_floor)
    pulp_report_link_closure(TimelinePluginProof_CLAP)
    pulp_assert_link_floor(TimelinePluginProof_CLAP
        TIER    sequencer-plugin-editor
        REQUIRE format timeline timeline_editor timeline_view)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-TimelinePluginProof
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Timeline Plugin Proof.clap/Contents/MacOS/Timeline Plugin Proof'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-TimelinePluginProof PROPERTIES
        LABELS "validation;clap" TIMEOUT 10)
endif()
