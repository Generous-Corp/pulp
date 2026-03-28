# PulpCompressor — sidechain compressor validating multi-bus architecture
# First plugin with 2 input buses (main + optional sidechain)

# Tests
if(PULP_BUILD_TESTS)
    add_executable(pulp-compressor-test test_pulp_compressor.cpp)
    target_link_libraries(pulp-compressor-test PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-compressor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-compressor-test)
endif()

# Plugin (all formats except standalone — no standalone sidechain routing)
pulp_add_plugin(PulpCompressor
    FORMATS         VST3 AU CLAP
    PLUGIN_NAME     "PulpCompressor"
    BUNDLE_ID       "com.pulp.compressor"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PCmp"
    MANUFACTURER_CODE "Pulp"
)

# Validation
if(PULP_HAS_CLAP AND APPLE)
    add_test(NAME clap-dlopen-PulpCompressor
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpCompressor.clap/Contents/MacOS/PulpCompressor'); print('OK')\"")
    set_tests_properties(clap-dlopen-PulpCompressor PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()

if(PULP_HAS_VST3)
    find_program(PLUGINVAL pluginval)
    if(PLUGINVAL)
        add_test(NAME pluginval-PulpCompressor-VST3
            COMMAND ${PLUGINVAL} --validate ${CMAKE_BINARY_DIR}/VST3/PulpCompressor.vst3 --strictness-level 5 --timeout-ms 10000)
        set_tests_properties(pluginval-PulpCompressor-VST3 PROPERTIES LABELS "validation;vst3" TIMEOUT 30)
    endif()
endif()
