# Pulp test suite — uses Catch2
# Phase 1: stub, Catch2 integration added in Phase 2

# Placeholder test that validates the build system works
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)
