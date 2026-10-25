# CLI package registry test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# CLI package registry: pure local tests for registry parsing,
# lock-file round trips, target TOML parsing/writing, semver, quality
# scoring, unsupported-target detection, and search ranking. This stays
# off remote refresh and install/archive flows.
add_executable(pulp-test-cli-package-registry
    test_cli_package_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry_trust.cpp
)
target_include_directories(pulp-test-cli-package-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-package-registry PRIVATE
    pulp::platform
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-registry)

# Reviewed process API calls: each of these starts only system tools or a
# fork of itself, never a target this tree builds (tools/cmake/PulpTestData.cmake).
pulp_test_spawns(pulp-test-cli-package-registry NONE REASON "the registry's curl/powershell download, it runs nothing the tree builds")

# The registry owner's signing helper must produce signatures the CLI's
# verifier accepts: RFC 8032 vectors plus a sign/verify round trip over a
# registry.json, with a tampered byte refused.
add_test(NAME cli-package-registry-signing-helper
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/scripts/test_sign_package_registry.py)
