# pulp-mpe-synth — MPE-aware sine synth example
# Demonstrates MPE opt-in via PluginDescriptor::supports_mpe = true and
# use of the MpeVoiceAllocator / MpeSynthVoice helpers.

if(PULP_BUILD_TESTS)
    add_executable(pulp-mpe-synth-test test_pulp_mpe_synth.cpp)
    target_link_libraries(pulp-mpe-synth-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-mpe-synth-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-mpe-synth-test)
endif()

pulp_add_plugin(PulpMpeSynth
    FORMATS         CLAP
    PLUGIN_NAME     "PulpMpeSynth"
    BUNDLE_ID       "com.pulp.mpe-synth"
    MANUFACTURER    "Pulp"
    VERSION         "0.1.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PMps"
    MANUFACTURER_CODE "Pulp"
)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpMpeSynth
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpMpeSynth.clap/Contents/MacOS/PulpMpeSynth'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpMpeSynth PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
