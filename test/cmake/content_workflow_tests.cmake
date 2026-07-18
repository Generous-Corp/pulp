add_test(NAME cmake-plugin-runtime-manifest-layout
    COMMAND ${CMAKE_COMMAND}
        -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_plugin_runtime_manifest_layout.cmake)
set_tests_properties(cmake-plugin-runtime-manifest-layout PROPERTIES
    LABELS "cmake;content;auv3"
    TIMEOUT 30)

# Compiled stand-in for `pulp-screenshot`, used by the kit-verify
# `--execute-screenshots` tests. A real executable instead of a generated
# .cmd/.sh shim so the screenshot-execution path behaves identically on every
# platform (the shell shims were a recurring Windows-only failure source).
add_executable(pulp-fake-screenshot-tool
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/fake_screenshot_tool.cpp)
set_target_properties(pulp-fake-screenshot-tool PROPERTIES
    OUTPUT_NAME "pulp-fake-screenshot-tool")

# CLI kit commands: metadata-only package manifest validation.
add_executable(pulp-test-cli-kit-commands
    test_cli_kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_manifest_validation.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_fs_util.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp)
target_include_directories(pulp-test-cli-kit-commands PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tools/cli ${CMAKE_BINARY_DIR}/tools/cli)
target_compile_definitions(pulp-test-cli-kit-commands PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PULP_FAKE_SCREENSHOT_TOOL="$<TARGET_FILE:pulp-fake-screenshot-tool>")
add_dependencies(pulp-test-cli-kit-commands pulp-fake-screenshot-tool)
target_link_libraries(pulp-test-cli-kit-commands PRIVATE
    pulp::platform pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-kit-commands)

# CLI content commands: data-only desktop content-pack validation/install.
add_executable(pulp-test-cli-content-commands
    test_cli_content_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/content_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/kit_manifest_validation.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_fs_util.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp)
target_include_directories(pulp-test-cli-content-commands PRIVATE
    ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tools/cli ${CMAKE_BINARY_DIR}/tools/cli)
target_compile_definitions(pulp-test-cli-content-commands PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(pulp-test-cli-content-commands PRIVATE
    pulp::platform pulp::runtime pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-content-commands)
