# PulpGain — the simplest possible Pulp plugin
# Validates: build system, processor interface, parameter system, state serialization
# Uses pulp_add_plugin() for all format targets

# ── Tests ──────────────────────────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    add_executable(pulp-gain-test test_pulp_gain.cpp)
    target_link_libraries(pulp-gain-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-gain-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-gain-test)
endif()

# ── Plugin (all formats) ──────────────────────────────────────────────────
set(PULP_GAIN_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND PULP_GAIN_FORMATS AU AAX)
elseif(WIN32)
    list(APPEND PULP_GAIN_FORMATS AAX)
endif()

pulp_add_plugin(PulpGain
    FORMATS         ${PULP_GAIN_FORMATS}
    PLUGIN_NAME     "PulpGain"
    BUNDLE_ID       "com.pulp.gain"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PGan"
    MANUFACTURER_CODE "Pulp"
    AAX_PRODUCT_CODE "PGAi"
    AAX_NATIVE_CODE "PGan"
)

# ── Validation Tests ──────────────────────────────────────────────────────

# CLAP dlopen test
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpGain
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap/Contents/MacOS/PulpGain'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpGain PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

# auval (AU v2)
if(APPLE AND PULP_HAS_AUSDK)
    add_test(
        NAME auval-PulpGain
        COMMAND ${CMAKE_COMMAND} -E env
            bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/PulpGain.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/PulpGain.component\" \"$d\" && { killall -KILL AudioComponentRegistrar 2>/dev/null || true; sleep 1; auval -v aufx PGan Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; }; rc=$?; rm -rf \"$d\"; exit $rc"
    )
    set_tests_properties(auval-PulpGain PROPERTIES
        LABELS "validation;au"
        TIMEOUT 30
        ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
endif()

# pluginval (VST3)
if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(
            NAME pluginval-PulpGain-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpGain.vst3 --strictness-level 5 --timeout-ms 10000
        )
        set_tests_properties(pluginval-PulpGain-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
