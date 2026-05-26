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
    cli_sdk.cpp
    cli_doctor_helpers.cpp
    cmd_build.cpp
    cmd_config.cpp
    cmd_coverage.cpp
    cmd_create.cpp
    cmd_macos.cpp
    cmd_overflow.cpp
    cmd_design.cpp
    cmd_doctor.cpp
    cmd_docs.cpp
    cmd_misc.cpp
    cmd_run.cpp
    cmd_run_parse.cpp
    cmd_ship.cpp
    cmd_upgrade.cpp
    cmd_validate.cpp
    cmd_sdk.cpp
    cmd_audio.cpp
    cmd_version.cpp
    cmd_dev.cpp
    cmd_loop.cpp
    cmd_host.cpp
    cmd_pr.cpp
    cmd_inspect.cpp
    cmd_tweaks.cpp
    design_binding.cpp
    create_targets.cpp
    package_registry.cpp
    package_commands.cpp
    package_commands_util.cpp
    package_commands_search.cpp
    package_commands_add.cpp
    tool_registry.cpp
    version_diag.cpp
    projects_registry.cpp
    fetchcontent_cache.cpp
    sdk_cache_paths.cpp
    cmd_projects.cpp
    project_bump.cpp
    cmd_project.cpp
    cmd_project_common.cpp
    cmd_project_bump.cpp
    cmd_project_undo.cpp
    update_check.cpp
    update_mode.cpp
    migration_runtime.cpp
    validator_discovery.cpp
    mac_runtime_validators.cpp
    install_paths_mac.cpp
    "${_pulp_migration_index_src}")
# Phase 8 binary swap (#767 / #686): the C++ CLI was historically the
# user-facing `pulp` binary. With the Rust binary now ready, the C++
# side renames to `pulp-cpp` and the Rust binary takes the `pulp`
# slot. Rollback for users: `PULP_USE_CPP=1 pulp …` execs into
# `pulp-cpp` (handled at the top of `experimental/pulp-rs/src/main.rs`).
# The CMake target name (`pulp-cli`) does NOT change — only the file
# basename — so all ctest entries below keep working.
set_target_properties(pulp-cli PROPERTIES OUTPUT_NAME "pulp-cpp")
target_compile_features(pulp-cli PRIVATE cxx_std_20)
target_link_libraries(pulp-cli PRIVATE pulp::runtime pulp::tool-audio pulp::ship pulp::view pulp::format pulp::inspect pulp::host)

# Link-order fix for libskia.a → fontconfig on Linux+GNU ld (#1962).
#
# `libskia.a` (pulled in by `pulp::canvas`'s PUBLIC `skia::skia` dep)
# contains `SkFontMgr_New_FontConfig` which references `FcInit*`,
# `FcConfig*`, `FcPattern*`, `FcGetVersion`, `FcFontSet*` etc. GNU ld
# processes static archives left-to-right and only pulls in symbols
# that are unresolved AT THE TIME the archive is processed; symbols
# referenced AFTER the archive go unresolved even if a later library
# would have satisfied them. Adding `fontconfig` again here ensures it
# appears AFTER `libskia.a` in the final link line. (`PkgConfig::FONTCONFIG`
# is already wired PUBLIC on `pulp-canvas` per pulp #1962 follow-up, so
# the include dirs and -L flags are already correct — this just nails
# the link-line position down for ld.)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(_PULP_CLI_FONTCONFIG IMPORTED_TARGET fontconfig)
        if(_PULP_CLI_FONTCONFIG_FOUND)
            target_link_libraries(pulp-cli PRIVATE PkgConfig::_PULP_CLI_FONTCONFIG)
        endif()
    endif()
endif()

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
    set(_pulp_cli_doctor_ctest_env
        "PULP_UPDATE_CHECK_DISABLED=1"
        "PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-doctor")
    set(_pulp_cli_caches_ctest_env
        "PULP_UPDATE_CHECK_DISABLED=1"
        "PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-caches"
        "PULP_SHARED_FETCHCONTENT_SOURCE_DIR=${CMAKE_BINARY_DIR}/test-tmp/fcc-empty-744")

    add_test(NAME cli-help
        COMMAND pulp-cli help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-help PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp .* Pulp audio plugin framework CLI")

    add_test(NAME cli-doctor
        COMMAND pulp-cli doctor
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # doctor may exit 0 or 1 depending on environment, but should not
    # crash and must always print the "Pulp Doctor" banner. Codex P2
    # on PR #1278 — we previously gated this with SKIP_RETURN_CODE 1,
    # but that turns the test into "Not Run" on exit-1 and silently
    # bypasses PASS_REGULAR_EXPRESSION; a regression that swallowed
    # the banner while still returning 1 (a common path on hosts with
    # missing optional SDKs) would look like a skip in CI rather than
    # a failure. PASS_REGULAR_EXPRESSION already wins over the exit
    # code check, so dropping SKIP_RETURN_CODE re-enables real
    # regression detection without breaking exit-1 hosts.
    set_tests_properties(cli-doctor PROPERTIES
        ENVIRONMENT "${_pulp_cli_doctor_ctest_env}"
        PASS_REGULAR_EXPRESSION "Pulp Doctor")

    # Issue #499 Slice 1: `pulp doctor --versions` must print the
    # version diagnostics header and exit 0 regardless of skew — skew
    # warnings are advisory, not failures.
    add_test(NAME cli-doctor-versions
        COMMAND pulp-cli doctor --versions
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-doctor-versions PROPERTIES
        ENVIRONMENT "${_pulp_cli_doctor_ctest_env}"
        PASS_REGULAR_EXPRESSION "Pulp Version Diagnostics")

    add_test(NAME cli-doctor-ci
        COMMAND pulp-cli doctor --ci
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # CI mode may exit 1 if optional SDKs are missing — that's not a
    # crash. Pin the contract to "prints the Pulp Doctor banner" so a
    # regression that swallows output but still returns 1 fails the
    # test rather than getting silently skipped (Codex P2 on PR #1278).
    # PASS_REGULAR_EXPRESSION wins over the exit code check, so the
    # test passes on exit 0 OR 1 as long as the banner is rendered.
    set_tests_properties(cli-doctor-ci PROPERTIES
        ENVIRONMENT "${_pulp_cli_doctor_ctest_env}"
        PASS_REGULAR_EXPRESSION "Pulp Doctor")

    # Issue #743: `pulp doctor --validators` always renders the
    # "Validators" header and exits 0/1 (never 2 = unknown flag).
    # Per-tool verdicts depend on host install state, so the regex only
    # asserts the section banner. Exit-code-not-2 protection lives in
    # test_cli_shellout.cpp ([cli][shellout][doctor][issue-743]).
    add_test(NAME cli-doctor-validators
        COMMAND pulp-cli doctor --validators
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # See the cli-doctor / cli-doctor-ci notes above — drop
    # SKIP_RETURN_CODE so the "Validators" banner contract is enforced
    # even when an optional validator is missing on the host (exit 1).
    # Codex P2 on PR #1278.
    set_tests_properties(cli-doctor-validators PROPERTIES
        ENVIRONMENT "${_pulp_cli_doctor_ctest_env}"
        PASS_REGULAR_EXPRESSION "Validators")

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

    add_test(NAME cli-host-help
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DHELP_FLAG=--help"
            "-DEXPECTED_EXIT=0"
            "-DEXPECTED_REGEX=Usage: pulp host"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_host_error.cmake")
    set_tests_properties(cli-host-help PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI host contract")

    add_test(NAME cli-host-missing-args
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DEXPECTED_EXIT=1"
            "-DEXPECTED_REGEX=Usage: pulp host"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_host_error.cmake")
    set_tests_properties(cli-host-missing-args PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI host contract")

    foreach(_pulp_host_format IN ITEMS clap vst3 au auv3 lv2)
        add_test(NAME "cli-host-missing-path-${_pulp_host_format}"
            COMMAND "${CMAKE_COMMAND}"
                "-DCLI=$<TARGET_FILE:pulp-cli>"
                "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
                "-DPLUGIN_PATH=${CMAKE_BINARY_DIR}/test-tmp/missing-host-plugin-${_pulp_host_format}"
                "-DFORMAT=${_pulp_host_format}"
                "-DEXPECTED_EXIT=1"
                "-DEXPECTED_REGEX=pulp host: failed to load"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_host_error.cmake")
        set_tests_properties("cli-host-missing-path-${_pulp_host_format}" PROPERTIES
            PASS_REGULAR_EXPRESSION "verified CLI host contract")
    endforeach()

    add_test(NAME cli-host-missing-path-auv3-short-format
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DPLUGIN_PATH=${CMAKE_BINARY_DIR}/test-tmp/missing-host-plugin-auv3-short-format"
            "-DFORMAT_FLAG=-f"
            "-DFORMAT=AUv3"
            "-DEXPECTED_EXIT=1"
            "-DEXPECTED_REGEX=pulp host: failed to load"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_host_error.cmake")
    set_tests_properties(cli-host-missing-path-auv3-short-format PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI host contract")

    add_test(NAME cli-host-missing-path-with-id
        COMMAND "${CMAKE_COMMAND}"
            "-DCLI=$<TARGET_FILE:pulp-cli>"
            "-DWORKING_DIR=${CMAKE_SOURCE_DIR}"
            "-DPLUGIN_PATH=${CMAKE_BINARY_DIR}/test-tmp/missing-host-plugin-with-id"
            "-DUNIQUE_ID=com.example.Missing"
            "-DFORMAT=clap"
            "-DEXPECTED_EXIT=1"
            "-DEXPECTED_REGEX=pulp host: failed to load"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli_host_error.cmake")
    set_tests_properties(cli-host-missing-path-with-id PROPERTIES
        PASS_REGULAR_EXPRESSION "verified CLI host contract")

    # Exercise the new `pulp coverage` dispatch (cmd_coverage.cpp).
    # `pulp coverage` with no args prints the usage banner and exits 0;
    # `pulp coverage bogus` prints the unknown-subcommand error and exits
    # 1. Together these cover every branch in cmd_coverage's dispatch
    # except `pulp coverage diff`, which would shell out to
    # tools/scripts/local_diff_cover.sh — that is exercised separately by
    # tools/scripts/test_local_diff_cover.py and (in CI) by the Coverage
    # workflow itself, so the slow shell-out doesn't run in ctest.
    add_test(NAME cli-coverage-help
        COMMAND pulp-cli coverage
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-coverage-help PROPERTIES
        PASS_REGULAR_EXPRESSION "pulp coverage")

    add_test(NAME cli-coverage-help-flag
        COMMAND pulp-cli coverage --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-coverage-help-flag PROPERTIES
        PASS_REGULAR_EXPRESSION "diff \\[TARGET")

    # `pulp coverage <bogus>` exits non-zero on the unknown-subcommand path.
    # WILL_FAIL inverts the exit-code expectation — pass when the CLI exits
    # with a non-zero code (which it does after printing the diagnostic and
    # the usage banner).
    add_test(NAME cli-coverage-unknown-subcommand
        COMMAND pulp-cli coverage bogus-subcommand
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-coverage-unknown-subcommand PROPERTIES
        WILL_FAIL TRUE)

    # `pulp coverage diff` exercises the run_coverage_diff branch which
    # shells out to tools/scripts/local_diff_cover.sh. Setting
    # PULP_SKIP_DIFF_COVER=1 makes the script exit 0 immediately, so this
    # test covers the dispatch + shell-out wiring without paying the
    # configure+build cost.
    add_test(NAME cli-coverage-diff-skip
        COMMAND pulp-cli coverage diff
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-coverage-diff-skip PROPERTIES
        ENVIRONMENT "PULP_SKIP_DIFF_COVER=1"
        PASS_REGULAR_EXPRESSION "skipped via PULP_SKIP_DIFF_COVER=1")

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

    # Issue #940 Slice 1: `pulp loop --help` always renders the focus-mode
    # help banner and exits 0. Behaviour-level tests for --status / --off
    # / --platform live in test/test_cli_shellout.cpp under [issue-940].
    add_test(NAME cli-loop-help
        COMMAND pulp-cli loop --help
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-loop-help PROPERTIES
        PASS_REGULAR_EXPRESSION "leveraged-prototype focus mode")

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
        ENVIRONMENT "PULP_UPDATE_CHECK_DISABLED=1;PULP_HOME=${CMAKE_BINARY_DIR}/test-tmp/pulp-home-json"
        PASS_REGULAR_EXPRESSION "\"projects\":")

    # `pulp doctor --caches` (#744): on a fresh empty cache root the
    # command should exit 0 and report "no entries". Using a brand-new
    # tempdir as the cache root via the documented env-var override
    # means the test never touches the developer's real
    # ~/Library/Caches/Pulp/.
    add_test(NAME cli-doctor-caches-empty
        COMMAND pulp-cli doctor --caches
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-doctor-caches-empty PROPERTIES
        ENVIRONMENT "${_pulp_cli_caches_ctest_env}"
        PASS_REGULAR_EXPRESSION "Pulp FetchContent cache")

    # `pulp doctor --caches --json` should emit the documented JSON
    # shape so scripts can parse it. Also exercises the JSON renderer
    # on the empty path.
    add_test(NAME cli-doctor-caches-json
        COMMAND pulp-cli doctor --caches --json
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(cli-doctor-caches-json PROPERTIES
        ENVIRONMENT "${_pulp_cli_caches_ctest_env}"
        PASS_REGULAR_EXPRESSION "\"healthy\":")
endif()
