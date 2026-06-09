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
target_compile_definitions(pulp-test-cli-package-analyzer-descriptors PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(pulp-test-cli-package-analyzer-descriptors PRIVATE
    pulp::audio
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-analyzer-descriptors)
