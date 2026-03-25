# Pulp test suite — Catch2

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

# Platform tests
add_executable(pulp-test-platform test_platform.cpp)
target_link_libraries(pulp-test-platform PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform)

# Runtime tests
add_executable(pulp-test-runtime test_runtime.cpp)
target_link_libraries(pulp-test-runtime PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-runtime)

# Events tests
add_executable(pulp-test-events test_events.cpp)
target_link_libraries(pulp-test-events PRIVATE pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-events)

# Audio tests
add_executable(pulp-test-audio test_audio.cpp)
target_link_libraries(pulp-test-audio PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio)

# MIDI tests
add_executable(pulp-test-midi test_midi.cpp)
target_link_libraries(pulp-test-midi PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi)

# State tests
add_executable(pulp-test-state test_state.cpp)
target_link_libraries(pulp-test-state PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-state)

# Binding tests
add_executable(pulp-test-binding test_binding.cpp)
target_link_libraries(pulp-test-binding PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-binding)

# Headless adapter tests
add_executable(pulp-test-headless test_headless.cpp)
target_link_libraries(pulp-test-headless PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-headless)

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-clap-entry)
endif()

# Golden-file audio tests (uses HeadlessHost + PulpGain)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-golden)
