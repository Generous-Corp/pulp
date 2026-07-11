# pulp-multi-out — an 8-voice instrument that renders each voice to its own
# stereo output bus (1 main + 7 aux). Demonstrates Pulp's multi-bus output path
# across VST3, CLAP, and AU v2 (aumu / MusicDeviceBase). The AU adapter turns the
# descriptor's eight output buses into eight AU output ELEMENTS, so Logic/Live/
# Cubase/Reaper list the aux outputs and route each voice to its own channel.

# ── Headless routing test ────────────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-multi-out-test test_pulp_multi_out.cpp)
    target_link_libraries(pulp-multi-out-test PRIVATE
        pulp::format pulp::audio pulp::midi pulp::state Catch2::Catch2WithMain)
    target_include_directories(pulp-multi-out-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-multi-out-test)
endif()

# ── Plugin ───────────────────────────────────────────────────────────────────
set(PMO_FORMATS VST3 CLAP)
if(APPLE)
    list(APPEND PMO_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()
pulp_add_plugin(PulpMultiOut
    FORMATS           ${PMO_FORMATS}
    PLUGIN_NAME       "PulpMultiOut"
    BUNDLE_ID         "com.pulp.multi-out"
    MANUFACTURER      "Pulp"
    VERSION           "0.1.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PMOt"
    MANUFACTURER_CODE "Pulp"
)

# ── CLAP dlopen smoke ────────────────────────────────────────────────────────
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpMultiOut
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpMultiOut.clap/Contents/MacOS/PulpMultiOut'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpMultiOut PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# ── auval (AU v2, aumu multi-output instrument) ──────────────────────────────
# Proves the multi-output aumu passes Apple's Audio Unit validation, including
# the "N output elements" negotiation the multi-bus lift enables. auval renders
# each output bus, so a broken multi-out render path fails here.
if(PULP_HAS_AUSDK AND APPLE AND NOT PULP_IOS)
    add_test(
        NAME auval-PulpMultiOut
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/PulpMultiOut.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/PulpMultiOut.component\" \"$d\" && { killall -KILL AudioComponentRegistrar 2>/dev/null || true; sleep 1; auval -v aumu PMOt Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; }; rc=$?; rm -rf \"$d\"; exit $rc"
    )
    set_tests_properties(auval-PulpMultiOut PROPERTIES
        LABELS "validation;au"
        TIMEOUT 120)
endif()
