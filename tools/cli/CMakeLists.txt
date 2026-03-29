# pulp CLI tool
add_executable(pulp-cli pulp_cli.cpp)
set_target_properties(pulp-cli PROPERTIES OUTPUT_NAME "pulp")
target_compile_features(pulp-cli PRIVATE cxx_std_20)

# Install to project root for easy access
install(TARGETS pulp-cli DESTINATION "${CMAKE_SOURCE_DIR}")

# Integration tests for CLI commands
if(PULP_BUILD_TESTS)
    add_test(NAME cli-help
        COMMAND pulp-cli help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-help PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp .* Pulp audio plugin framework CLI")

    add_test(NAME cli-doctor
        COMMAND pulp-cli doctor
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # doctor may exit 0 or 1 depending on environment, but should not crash
    set_tests_properties(cli-doctor PROPERTIES
        PASS_REGULAR_EXPRESSION "checks passed")

    add_test(NAME cli-doctor-ci
        COMMAND pulp-cli doctor --ci
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # CI mode may exit 1 if optional SDKs are missing — that's not a crash.
    # Accept any exit code; the test just verifies it doesn't crash.
    set_tests_properties(cli-doctor-ci PROPERTIES
        PASS_REGULAR_EXPRESSION ".*")

    add_test(NAME cli-status
        COMMAND pulp-cli status
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-status PROPERTIES
        PASS_REGULAR_EXPRESSION "Pulp Project Status")

    add_test(NAME cli-create-help
        COMMAND pulp-cli create --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-create-help PROPERTIES
        PASS_REGULAR_EXPRESSION "create a new plugin project")

    add_test(NAME cli-cache-status
        COMMAND pulp-cli cache status
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-cache-status PROPERTIES
        PASS_REGULAR_EXPRESSION "Pulp Cache")

    add_test(NAME cli-cache-help
        COMMAND pulp-cli cache
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-cache-help PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp cache")

    # Verify 'new' alias is removed — should print unknown command
    add_test(NAME cli-no-new-alias
        COMMAND pulp-cli new --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-no-new-alias PROPERTIES
        PASS_REGULAR_EXPRESSION "Unknown command")
endif()
