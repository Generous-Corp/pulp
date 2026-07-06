# CLI package registry test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# CLI package registry: pure local tests for registry parsing,
# lock-file round trips, target TOML parsing/writing, semver, quality
# scoring, unsupported-target detection, and search ranking. This stays
# off remote refresh and install/archive flows.
add_executable(pulp-test-cli-package-registry
    test_cli_package_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-package-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-package-registry PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-registry)
