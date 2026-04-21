# pulp CLI tool


# Generate version header from CMake project(VERSION)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pulp_version_gen.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pulp_version_gen.h"
    @ONLY)

# ── Migration index codegen (release-discovery Slice 3, #548) ───────────────
#
# Scan docs/migrations/*.md at configure time, parse the TOML
# frontmatter, and emit a C++ source file holding the embedded
# migration index. The generated TU is compiled into pulp-cli so
# `pulp upgrade --notes` never needs a network or filesystem round-trip
# to decide which release notes apply to the user's upgrade hop.
#
# The DEPENDS list ensures reconfigure / rebuild when either the
# generator script changes or any migration doc is added/modified.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_pulp_migration_index_src "${CMAKE_CURRENT_BINARY_DIR}/generated/migration_index.cpp")
file(GLOB _pulp_migration_docs
     LIST_DIRECTORIES FALSE
     CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/docs/migrations/*.md")

add_custom_command(
    OUTPUT  "${_pulp_migration_index_src}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/scripts/build_migration_index.py"
            --docs-dir "${CMAKE_SOURCE_DIR}/docs/migrations"
            --out      "${_pulp_migration_index_src}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/scripts/build_migration_index.py"
            ${_pulp_migration_docs}
    COMMENT "Generating migration_index.cpp from docs/migrations/*.md"
    VERBATIM)

add_executable(pulp-cli
    pulp_cli.cpp
    cli_common.cpp
    cmd_build.cpp
    cmd_config.cpp
    cmd_create.cpp
    cmd_design.cpp
    cmd_doctor.cpp
    cmd_docs.cpp
    cmd_misc.cpp
    cmd_run.cpp
    cmd_ship.cpp
    cmd_upgrade.cpp
    cmd_validate.cpp
    cmd_sdk.cpp
    cmd_audio.cpp
    cmd_version.cpp
    cmd_dev.cpp
    cmd_host.cpp
    cmd_pr.cpp
    cmd_inspect.cpp
    design_binding.cpp
    create_targets.cpp
    package_registry.cpp
    package_commands.cpp
    tool_registry.cpp
    version_diag.cpp
    projects_registry.cpp
    cmd_projects.cpp
    project_bump.cpp
    cmd_project.cpp
    update_check.cpp
    update_mode.cpp
    migration_runtime.cpp
    "${_pulp_migration_index_src}")
set_target_properties(pulp-cli PROPERTIES OUTPUT_NAME "pulp")
target_compile_features(pulp-cli PRIVATE cxx_std_20)
target_link_libraries(pulp-cli PRIVATE pulp::runtime pulp::tool-audio pulp::ship pulp::view pulp::format pulp::inspect pulp::host)
target_include_directories(pulp-cli PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})

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

    # Issue #499 Slice 1: `pulp doctor --versions` must print the
    # version diagnostics header and exit 0 regardless of skew — skew
    # warnings are advisory, not failures.
    add_test(NAME cli-doctor-versions
        COMMAND pulp-cli doctor --versions
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-doctor-versions PROPERTIES
        PASS_REGULAR_EXPRESSION "Pulp Version Diagnostics")

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

    # Issue #552 Slice 1b: `pulp projects` help path should always
    # print a usage banner listing list/add/remove. The test uses a
    # throwaway PULP_HOME so it doesn't mutate the developer's real
    # registry when invoked from `ctest`.
    add_test(NAME cli-projects-help
        COMMAND pulp-cli projects --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-projects-help PROPERTIES
        ENVIRONMENT "PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-help"
        PASS_REGULAR_EXPRESSION "pulp projects list")

    add_test(NAME cli-projects-list-empty
        COMMAND pulp-cli projects list
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-projects-list-empty PROPERTIES
        ENVIRONMENT "PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-empty"
        PASS_REGULAR_EXPRESSION "no projects registered")

    # `pulp doctor --versions --json` should emit a JSON object with a
    # projects array (empty is fine — the array is always present).
    add_test(NAME cli-doctor-versions-json
        COMMAND pulp-cli doctor --versions --json
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-doctor-versions-json PROPERTIES
        ENVIRONMENT "PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-json"
        PASS_REGULAR_EXPRESSION "\"projects\":")
endif()
