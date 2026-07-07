# CLI unit and shellout test target registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# CLI design binding tests
add_executable(pulp-test-cli-design-binding test_cli_design_binding.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/design_binding.cpp
)
target_include_directories(pulp-test-cli-design-binding PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-design-binding PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-design-binding
    PROPERTIES LABELS "parser-import")

# CLI create target selection tests
add_executable(pulp-test-cli-create-targets test_cli_create_targets.cpp ${CMAKE_SOURCE_DIR}/tools/cli/create_build_commands.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/create_targets.cpp
)
target_include_directories(pulp-test-cli-create-targets PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-create-targets PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-create-targets)

# CLI create shell-out edge tests. These launch the built CLI but stay on
# fail-fast paths that do not run doctor, configure, build, or network setup.
add_executable(pulp-test-cli-create-shellout test_cli_create_shellout.cpp)
target_link_libraries(pulp-test-cli-create-shellout PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-create-shellout pulp-cli)
    target_compile_definitions(pulp-test-cli-create-shellout PRIVATE
        PULP_CLI_BINARY="$<TARGET_FILE:pulp-cli>")
endif()
target_compile_definitions(pulp-test-cli-create-shellout PRIVATE
    PULP_BUILD_DIR="${CMAKE_BINARY_DIR}"
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-cli-create-shellout)

# CLI import substrate tests — detection engine, JSON-over-stdio SPI runner,
# install-hint path, and the vendor-agnostic source guard. Links the import
# logic directly (pure functions) and shells out to the built CLI for the
# install-hint / detect / end-to-end paths.
add_executable(pulp-test-cli-import
    test_cli_import.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_detect.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_spi.cpp
)
target_include_directories(pulp-test-cli-import PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-import PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import pulp-cli)
    target_compile_definitions(pulp-test-cli-import PRIVATE
        PULP_CLI_BINARY="$<TARGET_FILE:pulp-cli>")
endif()
target_compile_definitions(pulp-test-cli-import PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-cli-import
    PROPERTIES LABELS "parser-import")

# CLI import EMIT materialisation: manifest parse, write-plan, output denylist
# scan (all pure), plus an end-to-end shell-out through a mock emit responder.
# Links the pure import-emit logic directly.
add_executable(pulp-test-cli-import-emit
    test_cli_import_emit.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_emit.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_emit_scan.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_detect.cpp
)
target_include_directories(pulp-test-cli-import-emit PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-import-emit PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import-emit pulp-cli)
    target_compile_definitions(pulp-test-cli-import-emit PRIVATE
        PULP_CLI_BINARY="$<TARGET_FILE:pulp-cli>")
endif()
target_compile_definitions(pulp-test-cli-import-emit PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-cli-import-emit
    PROPERTIES LABELS "parser-import")

# CLI import IMPORTER_TERMS accept-gate + provenance check. Links the pure
# acceptance-store logic directly, shells out to the built CLI for the
# accept-to-run gate behaviour, and shells out to the Python provenance audit.
add_executable(pulp-test-cli-import-terms
    test_cli_import_terms.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_terms.cpp
)
target_include_directories(pulp-test-cli-import-terms PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-import-terms PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import-terms pulp-cli)
    target_compile_definitions(pulp-test-cli-import-terms PRIVATE
        PULP_CLI_BINARY="$<TARGET_FILE:pulp-cli>")
endif()
target_compile_definitions(pulp-test-cli-import-terms PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-cli-import-terms
    PROPERTIES LABELS "parser-import")

# `pulp run` flag parser unit tests. Pure parser, no I/O,
# no project resolution. The shell-out CLI behaviour for the same
# flags lives in test_cli_shellout.cpp.
add_executable(pulp-test-cli-run-options
    test_cli_run_options.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cmd_run_parse.cpp
)
target_include_directories(pulp-test-cli-run-options PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-run-options PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-run-options)

# Fixture binary for the `pulp run --headless --screenshot`
# shell-out test. Stands in for a plugin standalone: writes a tiny valid
# PNG when given --screenshot <path> (or PULP_SCREENSHOT=<path>) so the
# end-to-end CI-validation contract has a deterministic, dependency-free
# target.
add_executable(pulp-test-cli-run-fixture fixtures/cli_run_fixture.cpp)
set_target_properties(pulp-test-cli-run-fixture PROPERTIES
    OUTPUT_NAME "pulp-cli-run-fixture")

# CLI shell-out behaviour tests — launches the built `pulp` binary.
function(pulp_bind_cli_shellout_target target)
    if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU)
        set(_pulp_cli_path "${CMAKE_BINARY_DIR}/tools/cli/pulp-cpp${CMAKE_EXECUTABLE_SUFFIX}")
        if(CMAKE_CONFIGURATION_TYPES)
            set(_pulp_cli_path "${CMAKE_BINARY_DIR}/tools/cli/$<CONFIG>/pulp-cpp${CMAKE_EXECUTABLE_SUFFIX}")
        endif()
        target_compile_definitions(${target} PRIVATE
            PULP_CLI_BINARY="${_pulp_cli_path}")
    endif()
endfunction()

add_executable(pulp-test-cli-shellout test_cli_shellout.cpp test_cli_fmt_shellout.cpp)
target_link_libraries(pulp-test-cli-shellout PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-shellout)
# Depend on the fixture binary so the headless/screenshot
# end-to-end test has its target ready in the build tree.
if(TARGET pulp-test-cli-run-fixture)
    add_dependencies(pulp-test-cli-shellout pulp-test-cli-run-fixture)
endif()
if(TARGET pulp-import-design)
    add_dependencies(pulp-test-cli-shellout pulp-import-design)
endif()
catch_discover_tests(pulp-test-cli-shellout)
if(TARGET pulp::inspect)
    add_executable(pulp-test-cli-inspect-shellout test_cli_inspect_shellout.cpp)
    target_link_libraries(pulp-test-cli-inspect-shellout PRIVATE
        pulp::platform
        pulp::inspect
        Catch2::Catch2WithMain)
    pulp_bind_cli_shellout_target(pulp-test-cli-inspect-shellout)
    catch_discover_tests(pulp-test-cli-inspect-shellout)
endif()
# `pulp loop` shell-out tests. Shares test_cli_shellout_helpers.hpp
# with the parent CLI shell-out target and the scan/projects sibling.
add_executable(pulp-test-cli-shellout-loop test_cli_shellout_loop.cpp)
target_link_libraries(pulp-test-cli-shellout-loop PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-shellout-loop)
catch_discover_tests(pulp-test-cli-shellout-loop)
# `pulp scan` + `pulp projects list` shell-out tests. Shares the CLI
# shell-out helpers with the sibling shell-out targets.
add_executable(pulp-test-cli-shellout-scan-projects test_cli_shellout_scan_projects.cpp)
target_link_libraries(pulp-test-cli-shellout-scan-projects PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-shellout-scan-projects)
catch_discover_tests(pulp-test-cli-shellout-scan-projects)
# `pulp pr` shell-out tests: pr.workflow validation, Shipyard binary pin
# handling, and native fallback. Shares the helper header with the
# sibling shell-out targets.
add_executable(pulp-test-cli-shellout-pr test_cli_shellout_pr.cpp)
target_link_libraries(pulp-test-cli-shellout-pr PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-shellout-pr)
catch_discover_tests(pulp-test-cli-shellout-pr)

# CLI lifecycle-command shell-out tests: pulp doctor / pulp dev /
# pulp design / pulp sdk / pulp upgrade. These cover the lifecycle and
# diagnostics surface of the CLI.
add_executable(pulp-test-cli-shellout-lifecycle test_cli_shellout_lifecycle.cpp)
target_link_libraries(pulp-test-cli-shellout-lifecycle PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-shellout-lifecycle)
catch_discover_tests(pulp-test-cli-shellout-lifecycle)

# `pulp tweaks diff` shell-out tests. Drives the built binary against
# pulp-tweaks.json + design-snapshot fixtures and pins the clean /
# drifted / orphaned exit-code contract.
add_executable(pulp-test-cli-tweaks-shellout test_cli_tweaks_shellout.cpp)
target_link_libraries(pulp-test-cli-tweaks-shellout PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-tweaks-shellout)
catch_discover_tests(pulp-test-cli-tweaks-shellout)

# Pin shell_quote's per-platform contract so it can't
# silently regress to the pre-fix shape that broke `git fetch origin`
# on Windows by writing doubled backslashes into clone URLs.
add_executable(pulp-test-cli-shell-quote
    test_cli_shell_quote.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tartci_lease.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_redirect.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_sdk.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_doctor_helpers.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/fetchcontent_cache.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools/cli")
configure_file(
    "${CMAKE_SOURCE_DIR}/tools/cli/pulp_version_gen.h.in"
    "${CMAKE_BINARY_DIR}/tools/cli/pulp_version_gen.h"
    @ONLY)
target_include_directories(pulp-test-cli-shell-quote PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli
    ${CMAKE_BINARY_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-shell-quote PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-shell-quote)

add_executable(pulp-test-cli-tartci-lease
    test_cli_tartci_lease.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tartci_lease.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/shell_redirect.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_sdk.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_doctor_helpers.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/fetchcontent_cache.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-tartci-lease PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli
    ${CMAKE_BINARY_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-tartci-lease PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-tartci-lease)

add_executable(pulp-test-cli-docs-command
    test_cli_docs_command.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cmd_docs.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tartci_lease.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_redirect.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_sdk.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_doctor_helpers.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/fetchcontent_cache.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-docs-command PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli
    ${CMAKE_BINARY_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-docs-command PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-docs-command)

# Plugin ↔ CLI skew banner helper: shells out to bash with a
# staged `pulp` shim and asserts the banner copy + once-per-session
# semantics of `.agents/skills/_common/cli_version_check.sh`.
# The test file is POSIX-only (invokes /bin/bash) — on Windows we skip
# to avoid forcing a bash-for-Windows dependency into CI. The helper
# itself is plain POSIX shell, so wgpu/macOS don't factor in.
if(NOT WIN32)
    add_executable(pulp-test-cli-skew-banner test_cli_skew_banner.cpp)
    target_link_libraries(pulp-test-cli-skew-banner PRIVATE
        pulp::platform Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-cli-skew-banner PRIVATE
        PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    catch_discover_tests(pulp-test-cli-skew-banner
        PROPERTIES ENVIRONMENT "PULP_REPO_ROOT=${CMAKE_SOURCE_DIR}")
endif()

# CLI ship shell-out behaviour tests — launches the built `pulp` binary
# for the non-destructive ship subcommand branches.
add_executable(pulp-test-cli-ship-shellout test_cli_ship_shellout.cpp)
target_link_libraries(pulp-test-cli-ship-shellout PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-ship-shellout pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-ship-shellout)

# App Store Connect notary env-file parser. Compiles the parser source
# directly so the test target stays free of pulp::ship + pulp::runtime
# link surfaces (notary_env.cpp is intentionally pure stdlib).
add_executable(pulp-test-notary-env
    test_notary_env.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/notary_env.cpp)
target_include_directories(pulp-test-notary-env
    PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-notary-env PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-notary-env)

# `pulp build --install` validation gate. The test compiles
# install_paths_mac.cpp directly
# so it stays free of cli_common's link surface; the InstallEnv
# interface lets the scenarios assert mkdir/rm/cp order without
# touching the real ~/Library/Audio/Plug-Ins/ tree.
add_executable(pulp-test-cli-install-paths-mac
    test_cli_install_paths_mac.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/install_paths_mac.cpp)
target_include_directories(pulp-test-cli-install-paths-mac
    PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-install-paths-mac
    PRIVATE Catch2::Catch2WithMain)
if(WIN32)
    catch_discover_tests(pulp-test-cli-install-paths-mac
        PROPERTIES LABELS "windows-pr-quarantine")
else()
    catch_discover_tests(pulp-test-cli-install-paths-mac)
endif()

# CLI upgrade URL regression test: asset filename stays
# "pulp-<platform>-<arch>.<ext>" — the version sits in the release tag.
# Test includes tools/cli/upgrade_url.hpp directly, no link deps needed.
add_executable(pulp-test-cli-upgrade-url test_cli_upgrade_url.cpp)
target_include_directories(pulp-test-cli-upgrade-url PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-upgrade-url PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-upgrade-url)

# CLI upgrade install regression: a pre-cutover C++ CLI
# upgrading into a Rust archive must install sibling payloads such as
# pulp-cpp before replacing the user-facing pulp binary.
add_executable(pulp-test-cli-upgrade-install test_cli_upgrade_install.cpp)
target_include_directories(pulp-test-cli-upgrade-install PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-upgrade-install PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-upgrade-install)

add_executable(pulp-test-cli-json-parser test_cli_json_parser.cpp)
target_include_directories(pulp-test-cli-json-parser PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-json-parser PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-json-parser)

# CLI version diagnostics + checked version-write primitive:
# pure-logic, cli_common-free tests for the semver/skew analyzer and the
# `pulp version` bump file writer (header-only). Compiled straight in.
add_executable(pulp-test-cli-version-diag
    test_cli_version_diag.cpp
    test_cli_version_write.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
)
target_include_directories(pulp-test-cli-version-diag PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-version-diag PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-version-diag)

# CLI validator discovery: pure-logic tests for the
# `pulp doctor --validators` discovery + healing core, with a
# hand-rolled DiscoveryEnv so the 4 acceptance scenarios are hermetic
# (no spctl shellout, no PATH dependence). validator_discovery.cpp is
# deliberately free of cli_common link deps — same pattern as
# version_diag / projects_registry.
add_executable(pulp-test-cli-validator-discovery
    test_cli_validator_discovery.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/validator_discovery.cpp
)
target_include_directories(pulp-test-cli-validator-discovery PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-validator-discovery PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-validator-discovery)

# Mac runtime validator tests for
# `pulp validate --target {standalone|auv3|macho}`. They stub the
# shell-out and filesystem probes via MacValidatorEnv, so the tests run
# on any host (including Linux CI legs) without needing real .app /
# .appex / auval.
add_executable(pulp-test-cli-mac-runtime-validators
    test_cli_mac_runtime_validators.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/mac_runtime_validators.cpp
)
target_include_directories(pulp-test-cli-mac-runtime-validators PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-mac-runtime-validators PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-mac-runtime-validators)

# CLI projects registry: pure-logic tests for the
# ~/.pulp/projects.json read/write surface and the opt-in parent-
# directory scan. projects_registry is deliberately free of cli_common
# link deps for the same reason as version_diag.
add_executable(pulp-test-cli-projects-registry
    test_cli_projects_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
)
target_include_directories(pulp-test-cli-projects-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-projects-registry PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-projects-registry)

# CLI FetchContent cache discovery + heal: pure-logic tests for
# the four `pulp doctor --caches` acceptance scenarios — healthy /
# dangling-symlink / stale-commit / root-owned. fetchcontent_cache.cpp
# is deliberately free of cli_common link deps so the tests link it
# standalone (same pattern as projects_registry / version_diag).
add_executable(pulp-test-cli-fetchcontent-cache
    test_cli_fetchcontent_cache.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/fetchcontent_cache.cpp
)
target_include_directories(pulp-test-cli-fetchcontent-cache PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-fetchcontent-cache PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-fetchcontent-cache)

# Version-pinned SDK tarball filename helper.
# sdk_cache_paths.cpp is deliberately free of cli_common link deps so
# the tests link it standalone (same pattern as fetchcontent_cache /
# version_diag).
add_executable(pulp-test-cli-sdk-tarball-filename
    test_cli_sdk_tarball_filename.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/sdk_cache_paths.cpp
)
target_include_directories(pulp-test-cli-sdk-tarball-filename PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-sdk-tarball-filename PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-sdk-tarball-filename)

# CLI package commands: local-only tests for target/search/list/
# update/suggest/audit/add/remove behavior against staged project files
# and registry fixtures. Stays off remote fetch / archive extraction; links
# package_commands_* + package_registry.cpp directly without shelling out.
add_executable(pulp-test-cli-package-commands
    test_cli_package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_util.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_search.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_add.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
    # cmd_add routes `pulp add <importer>` through the importer install path.
    ${CMAKE_SOURCE_DIR}/tools/cli/tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/importer_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_spi.cpp
)
target_include_directories(pulp-test-cli-package-commands PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-package-commands PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-commands)
