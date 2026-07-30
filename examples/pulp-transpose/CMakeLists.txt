# pulp-transpose — a MIDI effect (notes in, transposed notes out, no audio).
# Exercises the AU v2 MIDI-processor path: CATEGORY MidiEffect resolves to the
# `aumi` component type (kAudioUnitType_MIDIProcessor), and the AU entry binds
# PulpAUMidiProcessor through AUMIDIEffectFactory.

# ── Headless MIDI-transform test ─────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-transpose-test test_pulp_transpose.cpp)
    target_link_libraries(pulp-transpose-test PRIVATE
        pulp::format pulp::audio pulp::midi pulp::state Catch2::Catch2WithMain)
    target_include_directories(pulp-transpose-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-transpose-test)
endif()

set(PTR_FORMATS CLAP)
if(APPLE)
    list(APPEND PTR_FORMATS AU)   # aumi (MIDI processor), via CATEGORY MidiEffect
endif()
pulp_add_plugin(PulpTranspose
    FORMATS           ${PTR_FORMATS}
    PLUGIN_NAME       "PulpTranspose"
    BUNDLE_ID         "com.pulp.transpose"
    MANUFACTURER      "Pulp"
    VERSION           "0.1.0"
    CATEGORY          MidiEffect
    PLUGIN_CODE       "PTrn"
    MANUFACTURER_CODE "Pulp"
)

# ── CLAP dlopen smoke ────────────────────────────────────────────────────────
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpTranspose
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpTranspose.clap/Contents/MacOS/PulpTranspose'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpTranspose PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# ── auval (AU v2, aumi MIDI processor) ───────────────────────────────────────
# Proves the `aumi` component Apple's validator accepts: component discovery,
# open/initialize, scope formats, required + recommended properties, the
# parameter surface, and parameter persistence across reset. The helper runs
# in the auto-login user's GUI bootstrap so it sees the same component registry
# as AU hosts instead of the SSH runner's bootstrap.
if(PULP_HAS_AUSDK AND APPLE AND NOT PULP_IOS)
    add_test(
        NAME auval-PulpTranspose
        COMMAND ${CMAKE_COMMAND} -E env
            PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1
            bash "${CMAKE_SOURCE_DIR}/tools/ci/run-auval-component.sh"
                "${CMAKE_BINARY_DIR}/AU/PulpTranspose.component" aumi PTrn Pulp
    )
    set_tests_properties(auval-PulpTranspose PROPERTIES
        LABELS "validation;au"
        TIMEOUT 90
        RESOURCE_LOCK pulp_au_discovery
        SKIP_RETURN_CODE 77)
endif()
