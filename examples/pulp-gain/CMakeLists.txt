# PulpGain — the simplest possible Pulp plugin
# Validates: build system, processor interface, parameter system, state serialization

# Headless test executable (validates processing without UI or format wrapper)
add_executable(pulp-gain-test test_pulp_gain.cpp)
target_link_libraries(pulp-gain-test PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-gain-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(pulp-gain-test)
