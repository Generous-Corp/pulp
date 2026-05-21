# PulpTone — oscillator synth with MIDI input
# Validates: audio output, MIDI handling, note-on/off, polyphony, envelopes
# Uses pulp_add_plugin() for all format targets

# ── Tests ──────────────────────────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-tone-test test_pulp_tone.cpp)
    target_link_libraries(pulp-tone-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-tone-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-tone-test)
endif()

# ── Plugin (all formats) ──────────────────────────────────────────────────
pulp_add_plugin(PulpTone
    FORMATS         VST3 AU CLAP Standalone
    PLUGIN_NAME     "PulpTone"
    BUNDLE_ID       "com.pulp.tone"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PTon"
    MANUFACTURER_CODE "Pulp"
)

# ── Validation Tests ──────────────────────────────────────────────────────

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpTone
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpTone.clap/Contents/MacOS/PulpTone'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpTone PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

if(APPLE AND PULP_HAS_AUSDK)
    add_test(
        NAME auval-PulpTone
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "cp -R \"${CMAKE_BINARY_DIR}/AU/PulpTone.component\" \"$HOME/Library/Audio/Plug-Ins/Components/\" && auval -v aumu PTon Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'"
    )
    set_tests_properties(auval-PulpTone PROPERTIES
        LABELS "validation;au"
        TIMEOUT 30
        ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
endif()

if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(
            NAME pluginval-PulpTone-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpTone.vst3 --strictness-level 5 --timeout-ms 10000
        )
        set_tests_properties(pluginval-PulpTone-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
