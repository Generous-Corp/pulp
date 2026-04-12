# pulp CLI tool


# Generate version header from CMake project(VERSION)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pulp_version_gen.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pulp_version_gen.h"
    @ONLY)

add_executable(pulp-cli
    pulp_cli.cpp
    cli_common.cpp
    cmd_build.cpp
    cmd_create.cpp
    cmd_design.cpp
    cmd_doctor.cpp
    cmd_docs.cpp
    cmd_misc.cpp
    cmd_run.cpp
    cmd_ship.cpp
    cmd_validate.cpp
    cmd_sdk.cpp
    cmd_audio.cpp
    cmd_version.cpp
    cmd_dev.cpp
    cmd_pr.cpp
    cmd_inspect.cpp
    design_binding.cpp
    create_targets.cpp
    package_registry.cpp
    package_commands.cpp
    tool_registry.cpp)
set_target_properties(pulp-cli PROPERTIES OUTPUT_NAME "pulp")
target_compile_features(pulp-cli PRIVATE cxx_std_20)
target_link_libraries(pulp-cli PRIVATE pulp::runtime pulp::tool-audio pulp::ship pulp::view pulp::format pulp::inspect)
target_include_directories(pulp-cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# RPATH so the installed binary can find shared libraries (e.g.,
# libwgpu_native.dylib / libwgpu_native.so) inside its install prefix.
# Without this the installed CLI crashes immediately on launch with
# "Library not loaded: @rpath/libwgpu_native.dylib — no LC_RPATH's
# found" because the wgpu-native prebuilt sets @rpath but nothing
# adds an LC_RPATH entry to pulp-cli (#74).
if(APPLE)
    set_target_properties(pulp-cli PROPERTIES
        INSTALL_RPATH "@executable_path/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE
        MACOSX_RPATH TRUE)
elseif(UNIX)
    set_target_properties(pulp-cli PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()

# Install into the active prefix so SDK installs do not try to write back into
# the source checkout.
set(_pulp_cli_install_bindir "${CMAKE_INSTALL_BINDIR}")
if(NOT _pulp_cli_install_bindir)
    set(_pulp_cli_install_bindir "bin")
endif()
install(TARGETS pulp-cli RUNTIME DESTINATION "${_pulp_cli_install_bindir}")

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
        PASS_REGULAR_EXPRESSION "--in-tree")

    add_test(NAME cli-run-help
        COMMAND pulp-cli run --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-run-help PROPERTIES
        PASS_REGULAR_EXPRESSION "active project build")

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

    add_test(NAME cli-audio-help
        COMMAND pulp-cli audio
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-audio-help PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp audio")

    add_test(NAME cli-audio-excerpt-find-invalid-top
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DFLAG=--top"
            "-DVALUE=foo"
            "-DEXPECTED_EXIT=1"
            "-DEXPECTED_REGEX=Error: invalid value for --top: foo"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_error.cmake")
    set_tests_properties(cli-audio-excerpt-find-invalid-top PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI failure contract")

    add_test(NAME cli-audio-excerpt-find-invalid-min-score
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DFLAG=--min-score"
            "-DVALUE=NaNx"
            "-DEXPECTED_EXIT=1"
            "-DEXPECTED_REGEX=Error: invalid value for --min-score: NaNx"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_error.cmake")
    set_tests_properties(cli-audio-excerpt-find-invalid-min-score PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI failure contract")

    # Verify 'new' alias is removed — should print unknown command
    add_test(NAME cli-no-new-alias
        COMMAND pulp-cli new --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-no-new-alias PROPERTIES
        PASS_REGULAR_EXPRESSION "Unknown command")
endif()
