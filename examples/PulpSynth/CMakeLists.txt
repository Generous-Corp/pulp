# PulpSynth — macro oscillator synth with filter, ADSR, and presets
# Validates: pulp-signal DSP library integration, state serialization
# Uses: Oscillator, Svf, Adsr, SmoothedValue, Gain

# ── Tests ──────────────────────────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-synth-test test_pulp_synth.cpp)
    target_link_libraries(pulp-synth-test PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
    target_include_directories(pulp-synth-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-synth-test)
endif()

# ── Plugin (CLAP only for now — add VST3/AU when validated) ──────────────
pulp_add_plugin(PulpSynth
    FORMATS         CLAP
    PLUGIN_NAME     "PulpSynth"
    BUNDLE_ID       "com.pulp.synth"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PSyn"
    MANUFACTURER_CODE "Pulp"
)

# PulpSynth uses DSP library headers — link signal to Core target
target_link_libraries(PulpSynth_Core INTERFACE pulp::signal)

# ── CLAP validation ─────────────────────────────────────────────────────
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpSynth
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpSynth.clap/Contents/MacOS/PulpSynth'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpSynth PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
