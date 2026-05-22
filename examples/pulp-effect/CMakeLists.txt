# PulpEffect — biquad filter with frequency, resonance, type, mix
# Validates: diverse parameter types, DSP, state serialization

# Tests
if(PULP_BUILD_TESTS)
    add_executable(pulp-effect-test test_pulp_effect.cpp)
    target_link_libraries(pulp-effect-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-effect-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-effect-test)
endif()

# Plugin (all formats)
pulp_add_plugin(PulpEffect
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "PulpEffect"
    BUNDLE_ID       "com.pulp.effect"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PEfx"
    MANUFACTURER_CODE "Pulp"
)

# Validation
if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PulpEffect
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpEffect.clap/Contents/MacOS/PulpEffect'); print('OK')\"")
    set_tests_properties(clap-dlopen-PulpEffect PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

if(APPLE AND PULP_HAS_AUSDK)
    add_test(NAME auval-PulpEffect
        COMMAND bash -c "d=\"$HOME/Library/Audio/Plug-Ins/Components/PulpEffect.auvaltest.component\"; rm -rf \"$d\"; cp -R \"${CMAKE_BINARY_DIR}/AU/PulpEffect.component\" \"$d\" && auval -v aufx PEfx Pulp 2>&1 | tee /dev/stderr | grep -q 'PASS'; rc=$?; rm -rf \"$d\"; exit $rc")
    set_tests_properties(auval-PulpEffect PROPERTIES
        LABELS "validation;au"
        TIMEOUT 30
        ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
endif()

if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(NAME pluginval-PulpEffect-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpEffect.vst3 --strictness-level 5 --timeout-ms 10000)
        set_tests_properties(pluginval-PulpEffect-VST3 PROPERTIES
            LABELS "validation;vst3"
            TIMEOUT 30
            ENVIRONMENT "PULP_DISABLE_PLUGIN_EDITOR=1;PULP_HEADLESS=1;PULP_TEST_MODE=1")
    endif()
endif()
