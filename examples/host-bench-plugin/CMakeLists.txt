# PulpHostBench — DAW-quirk validation bench plugin.
#
# Loads in every DAW under test and writes a tab-separated event log
# per session under `~/Library/Logs/PulpHostBench/` (macOS) or the
# equivalent on other platforms. The per-DAW manual scripts under
# `docs/validation/daw-bench/` drive the plugin; the aggregator
# `tools/scripts/promote_quirk_tiers.py` reads the logs.

if(PULP_BUILD_TESTS)
    add_executable(pulp-host-bench-test test_host_bench.cpp)
    target_link_libraries(pulp-host-bench-test
        PRIVATE pulp::format Catch2::Catch2WithMain)
    target_include_directories(pulp-host-bench-test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-host-bench-test)
endif()

set(PULP_HOST_BENCH_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND PULP_HOST_BENCH_FORMATS AU)
endif()

pulp_add_plugin(PulpHostBench
    FORMATS         ${PULP_HOST_BENCH_FORMATS}
    PLUGIN_NAME     "PulpHostBench"
    BUNDLE_ID       "com.pulp.host-bench"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PHBn"
    MANUFACTURER_CODE "Pulp"
)

# CLAP dlopen smoke (parity with PulpGain).
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpHostBench
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpHostBench.clap/Contents/MacOS/PulpHostBench'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpHostBench PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
