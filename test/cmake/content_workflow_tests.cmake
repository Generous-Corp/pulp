add_test(NAME cmake-plugin-runtime-manifest-layout
    COMMAND ${CMAKE_COMMAND}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_plugin_runtime_manifest_layout.cmake)
set_tests_properties(cmake-plugin-runtime-manifest-layout PROPERTIES
    LABELS "cmake;content;auv3"
    TIMEOUT 30)

# CLI kit commands: Phase 1 metadata-only package manifest validation.
add_executable(pulp-test-cli-kit-commands
    test_cli_kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp)
target_include_directories(pulp-test-cli-kit-commands PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tools/cli ${CMAKE_BINARY_DIR}/tools/cli)
target_compile_definitions(pulp-test-cli-kit-commands PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(pulp-test-cli-kit-commands PRIVATE
    pulp::platform pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-kit-commands)

# CLI content commands: data-only desktop content-pack validation/install.
add_executable(pulp-test-cli-content-commands
    test_cli_content_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/content_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp)
target_include_directories(pulp-test-cli-content-commands PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tools/cli ${CMAKE_BINARY_DIR}/tools/cli)
target_compile_definitions(pulp-test-cli-content-commands PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(pulp-test-cli-content-commands PRIVATE
    pulp::platform pulp::runtime pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-content-commands)
