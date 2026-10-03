# CLI package analyzer descriptors: registry/lock/target/license metadata
# conversion into core audio AnalyzerDescriptor records. Local-only; no
# package installs, remote refresh, or archive extraction.
add_executable(pulp-test-cli-package-analyzer-descriptors
    test_cli_package_analyzer_descriptors.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_analyzer_descriptors.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-package-analyzer-descriptors PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
pulp_test_data(pulp-test-cli-package-analyzer-descriptors PATHS tools/packages/registry.json)
target_link_libraries(pulp-test-cli-package-analyzer-descriptors PRIVATE
    pulp::audio
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-analyzer-descriptors)

# Reviewed process API calls: each of these starts only system tools or a
# fork of itself, never a target this tree builds (tools/cmake/PulpTestData.cmake).
pulp_test_spawns(pulp-test-cli-package-analyzer-descriptors NONE) # links the registry's curl/powershell download
