# CLI import-tool and analysis test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# CLI versioned import-design detection: unit + fixture-driven
# tests for the three-layer (parser-version, format-version,
# compat-schema-version) detector behind `pulp import-design
# --detect-only`. The detector module is deliberately self-contained
# (no pulp::view / pulp::state link deps) so the tests compile fast
# and don't drag the full design-import pipeline along.
add_executable(pulp-test-cli-import-detect
    test_cli_import_detect.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/import_detect.cpp
)
target_include_directories(pulp-test-cli-import-detect PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/import-design
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-import-detect PRIVATE
    Catch2::Catch2WithMain)
# The fixture loop walks up from cwd to find compat.json — start the
# tests from the repo root so the discovery works regardless of build
# directory shape.
catch_discover_tests(pulp-test-cli-import-detect
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    PROPERTIES LABELS "parser-import")

# CLI tool registry: local-only coverage for registry parsing,
# managed binary / Python wrapper discovery, deterministic install exits,
# uninstall, and command dispatch branches. Avoids downloads, archive
# extraction, Python package installs, and executing registry tools.
add_executable(pulp-test-cli-tool-registry
    test_cli_tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/importer_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_spi.cpp
)
target_include_directories(pulp-test-cli-tool-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-tool-registry PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-tool-registry)

# Importer install mechanism: version-window enforcement, sha256
# verification, skill install/uninstall, install records, and the `pulp tool
# install` / `pulp add` alias dispatch. Mock-local-package driven — no network.
add_executable(pulp-test-cli-importer-install
    test_cli_importer_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/importer_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_spi.cpp
)
target_include_directories(pulp-test-cli-importer-install PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-importer-install PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-importer-install)

# URL-driven importer install: clones a REAL LOCAL git repo (no network, no
# shipped registry) and drives the tool.json read, SPI-window enforcement, the
# accept-to-run terms gate, install/uninstall, and the privacy invariant (an
# unreachable URL leaks nothing). Links the git-install path plus the terms gate,
# the compat check (importer_install.cpp), and the registry helpers it reuses.
add_executable(pulp-test-cli-import-install
    test_cli_import_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/importer_git_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_terms.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/importer_install.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/import_spi.cpp
)
target_include_directories(pulp-test-cli-import-install PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-import-install PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-import-install)

# CLI project bump: pure-logic tests for the
# CMakeLists pin parser, rewriter, refuse-dynamic-pin gate, semver /
# downgrade guard, and the undo-batch JSON round-trip. project_bump.cpp
# is deliberately free of cli_common link deps so the tests link it
# standalone (same pattern as projects_registry / update_check /
# update_mode).
add_executable(pulp-test-cli-project-bump
    test_cli_project_bump.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/project_bump.cpp
)
target_include_directories(pulp-test-cli-project-bump PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-project-bump PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-project-bump)

# CLI project bump --all: batch-level
# invariants — partial failure isolation, missing-project reporting,
# undo round-trip across mixed-status batches, and the --allow-downgrade
# gate. Links both project_bump.cpp and projects_registry.cpp so the
# batch simulation can drive a real ~/.pulp/projects.json roundtrip.
add_executable(pulp-test-cli-project-bump-all
    test_cli_project_bump_all.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/project_bump.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
)
target_include_directories(pulp-test-cli-project-bump-all PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-project-bump-all PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-project-bump-all)

# CLI project command: direct command-level coverage for
# cmd_project.cpp's standalone worktree/root/dirty/redundant guards
# plus the shared standalone SDK resolution/doctor helpers in
# cli_common.cpp. The test includes cmd_project.cpp directly so the
# anonymous-namespace helpers can be exercised without shelling out.
# The desktop CLI subdirectory is skipped for PULP_ENABLE_GPU=OFF, but
# this test still compiles cli_common.cpp in sanitizer/IWYU configurations,
# so generate the shared version header here too.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tools/cli")
configure_file(
    "${CMAKE_SOURCE_DIR}/tools/cli/pulp_version_gen.h.in"
    "${CMAKE_BINARY_DIR}/tools/cli/pulp_version_gen.h"
    @ONLY)
add_executable(pulp-test-cli-project-command
    test_cli_project_command.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tartci_lease.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp ${CMAKE_SOURCE_DIR}/tools/cli/cli_delegate.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_redirect.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_sdk.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_doctor_helpers.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/fetchcontent_cache.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/project_bump.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/projects_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/migration_runtime.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
)
target_include_directories(pulp-test-cli-project-command PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli
    ${CMAKE_BINARY_DIR}/tools/cli)
target_compile_definitions(pulp-test-cli-project-command PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(pulp-test-cli-project-command PRIVATE
    pulp::runtime
    pulp::tool-audio
    pulp::ship
    pulp::view
    pulp::format
    pulp::host
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-project-command)

# CLI update-check: pure-logic tests for the
# ~/.pulp/update-cache.json round-trip, is_cache_stale boundary,
# semver comparison, banner composition, TOML writer, and the
# Fetcher injection. update_check.cpp is deliberately free of
# cli_common link deps so the tests compile standalone. Keep this
# target registered with update_mode because the two modules share the
# cache file and banner hooks.
add_executable(pulp-test-cli-update-check
    test_cli_update_check.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
)
target_include_directories(pulp-test-cli-update-check PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-update-check PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-update-check)

# CLI update-mode: pure-logic tests for the
# auto/prompt/manual/off state machine, 24h snooze expiration,
# pending-upgrade marker JSON round-trip, and Windows tombstone
# cleanup. Filesystem is mocked via per-test tmp dirs; time enters
# via explicit epoch-seconds arguments. update_mode.cpp depends on
# update_check.cpp for is_newer(), so both TUs are linked in.
add_executable(pulp-test-cli-update-mode
    test_cli_update_mode.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_mode.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/update_check.cpp
)
target_include_directories(pulp-test-cli-update-mode PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-update-mode PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-update-mode)

# CLI migration index: pure-logic tests for the
# applies_if expression evaluator, hop filter, text/JSON renderers,
# and the Python codegen round-trip. The generated translation unit
# `migration_index.cpp` is deliberately NOT linked into this test —
# the test provides its own empty index so the helpers stay link-safe,
# and the real embedded index is exercised by the shell-out tests.
add_executable(pulp-test-cli-migration-index
    test_cli_migration_index.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/migration_runtime.cpp
)
target_include_directories(pulp-test-cli-migration-index PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-migration-index PRIVATE
    Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-cli-migration-index PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-cli-migration-index
    PROPERTIES ENVIRONMENT "PULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
