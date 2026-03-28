# PulpDrums — generative drum sequencer MIDI effect
# Validates: MIDI output, pattern sequencing, Phase 6 GPU UI

if(PULP_BUILD_TESTS)
    add_executable(pulp-drums-test test_pulp_drums.cpp)
    target_link_libraries(pulp-drums-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-drums-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-drums-test)
endif()

pulp_add_plugin(PulpDrums
    FORMATS         CLAP
    PLUGIN_NAME     "PulpDrums"
    BUNDLE_ID       "com.pulp.drums"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PDrm"
    MANUFACTURER_CODE "Pulp"
)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpDrums
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpDrums.clap/Contents/MacOS/PulpDrums'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpDrums PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
