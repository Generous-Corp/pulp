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
    # CI mode outputs nothing on success (all pass) or FAIL: lines
    # Just verify it doesn't crash — either exit code is acceptable
    set_tests_properties(cli-doctor-ci PROPERTIES
        WILL_FAIL false)

    add_test(NAME cli-status
        COMMAND pulp-cli status
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-status PROPERTIES
        PASS_REGULAR_EXPRESSION "Pulp Project Status")
endif()
