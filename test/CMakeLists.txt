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
