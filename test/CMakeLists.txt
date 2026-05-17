# Pulp test suite — Catch2

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

# iOS AUv3 helper configure smoke (macOS host only; skipped when iOS SDK absent).
# A fresh checkout pays the FetchContent bootstrap cost (~5–7 min on CI macs).
# Local self-hosted runners can exceed that during cache churn, so leave
# enough headroom for the Xcode configure to finish instead of racing CTest.
if(APPLE AND NOT PULP_IOS)
    add_test(NAME cmake-ios-auv3-configure
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_ios_auv3_configure.sh
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(cmake-ios-auv3-configure PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 1800
        # Tagged `slow` for fast-CI exclusion (#1589): a fresh-cache
        # configure spends ~3 minutes downloading + unpacking iOS-leg
        # FetchContent sources before the actual smoke fires. Full-CI
        # (push to main / nightly / workflow_dispatch) still runs it.
        LABELS slow)
endif()

# AU v2 component-type selection smoke — hardens the aufx→aumf flip
# for descriptor.accepts_midi=true. Runs under `cmake -P` so it works
# on every platform without building the Pulp tree.
add_test(NAME cmake-au-v2-type-selection
    COMMAND ${CMAKE_COMMAND} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_au_v2_type_selection.cmake)
set_tests_properties(cmake-au-v2-type-selection PROPERTIES
    LABELS "cmake;au;au-v2;midi"
    TIMEOUT 30)

# pulp_add_binary_data Python encoder smoke — guards the issue #898 fix
# (legacy CMake hex loop was O(n²) and pinned cmake configure for 10–22
# minutes on a 1 MB asset). Verifies the encoder's output ABI, mtime
# advance on every run (required for `add_custom_command` build-system
# correctness), and regeneration on input change. Runs under `cmake -P`
# so it works on every platform without building the Pulp tree.
add_test(NAME cmake-pulp-add-binary-data-encoder
    COMMAND ${CMAKE_COMMAND} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_add_binary_data_encoder.cmake)
set_tests_properties(cmake-pulp-add-binary-data-encoder PROPERTIES
    # `slow` (#1589): runs `cmake -P` which spawns a CMake interpreter
    # to encode a synthetic 1 MB asset; consistently 4-5 sec wall on
    # all three platforms. Excluded from PR fast-CI.
    LABELS "cmake;binary-data;issue-898;slow"
    TIMEOUT 60)

# Install-layout regression for issue-905 follow-up: when the SDK is
# installed via `cmake --install`, the Python encoder MUST be bundled
# alongside PulpUtils.cmake so find_package(Pulp) consumers can call
# pulp_add_binary_data without seeing a missing-script error. Runs
# `cmake --install` against this build into a temp prefix and asserts
# both PulpUtils.cmake and scripts/encode_binary_data.py are present.
add_test(NAME cmake-pulp-install-layout
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_install_layout.cmake)
set_tests_properties(cmake-pulp-install-layout PROPERTIES
    # `slow` (#1589): runs `cmake --install` from this build into a
    # temp prefix and inspects the staged layout. Cheap on warm builds
    # but pays a configure-amortised cost — pull it out of PR fast-CI.
    LABELS "cmake;binary-data;issue-905;slow"
    TIMEOUT 120)

# PULP_REQUIRE_GPU_FOR_SDK gate smoke (pulp #1817). Two full configures
# in a tmpdir: ON+missing-skia must FAIL, OFF+missing-skia must SUCCEED.
# Tagged `slow` because each configure pays the SDL3 / FetchContent cost.
if(UNIX)
    add_test(NAME cmake-require-gpu-for-sdk
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_require_gpu_for_sdk.sh
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(cmake-require-gpu-for-sdk PROPERTIES
        LABELS "cmake;sdk;issue-1817;slow"
        TIMEOUT 600)
endif()

# retry_git_clone unit test (setup.sh helper). Guards the #425 P1
# Codex fix — a partial clone target is scrubbed between retries so
# attempt 2+ can actually re-run the network fetch.
if(UNIX)
    add_test(NAME retry-git-clone
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_retry_git_clone.sh)
    set_tests_properties(retry-git-clone PROPERTIES TIMEOUT 30)
endif()

# Launcher unit test (pulp #1821) — verifies tools/mcp/pulp-mcp-launcher
# resolves the source-tree build, falls back to $PATH, and emits a
# readable diagnostic when neither is present. Hermetic (stages a
# tempdir layout), no real binary required.
if(UNIX)
    add_test(NAME pulp-mcp-launcher
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_pulp_mcp_launcher.sh)
    set_tests_properties(pulp-mcp-launcher PROPERTIES TIMEOUT 15)
endif()

# Setup-hook unit test — verifies hooks/scripts/check-pulp-cli.sh
# handles the three "is pulp available" states (on PATH / source-tree
# build / nothing) plus three invariants (always exits 0, banner goes
# to stderr, stale non-executable build/pulp doesn't false-positive).
# Hermetic (stages a tempdir layout per case), no real binary required.
if(UNIX)
    add_test(NAME check-pulp-cli-hook
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_check_pulp_cli_hook.sh)
    set_tests_properties(check-pulp-cli-hook PROPERTIES TIMEOUT 15)
endif()

# pulp #2087 (slice 1) — install.sh must install CLI + matching SDK in one
# shot, and the closing message must document upgrade/pinning. Without
# this, fresh-install users silently end up with a current CLI on top of
# whatever ancient SDK was previously cached at ~/.pulp/sdk/.
if(UNIX)
    add_test(NAME install-sh-runs-sdk-install
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_install_sh_runs_sdk_install.sh)
    set_tests_properties(install-sh-runs-sdk-install
        PROPERTIES TIMEOUT 10
        LABELS "tooling")
endif()

# Window-only capture invariants (pulp-internal task #81) —
# spectr-roundtrip.sh must not silently fall back to full-screen
# screencapture, because terminal / desktop background then poisons
# the histogram diff against the REFERENCE render. Static-content
# regression pins the structural fix.
if(UNIX)
    add_test(NAME spectr-roundtrip-window-only-capture
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_spectr_roundtrip_window_only_capture.sh)
    set_tests_properties(spectr-roundtrip-window-only-capture
        PROPERTIES TIMEOUT 10
        LABELS "parser-import")
endif()

# Workspace freshness guard (pulp #2087 lesson) — refuse to run
# validation harnesses on a checkout behind origin/main. Test
# exercises 7 scenarios across enforce/warn/max-behind/bypass/json
# modes against synthetic git repos.
if(UNIX)
    add_test(NAME workspace-freshness
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_workspace_freshness.sh)
    set_tests_properties(workspace-freshness
        PROPERTIES TIMEOUT 30
        LABELS "tooling")
endif()

# Debug-SDK perf-killer guard smoke (pulp-internal task #35).
# Hermetic — runs the guard against a tempdir-fabricated SDK install,
# exercises the three branches (Debug-no-override → FATAL_ERROR,
# Debug-with-override → WARNING+proceed, Release → silent). Runs under
# `cmake -P` so it works on every platform without building the Pulp
# tree.
foreach(_pulp_debug_sdk_scenario
        "debug-no-override:fail"
        "debug-with-override:warn"
        "debug-lowercase:fail"
        "debug-mixed-case:fail"
        "release:silent")
    string(REPLACE ":" ";" _scenario_pair "${_pulp_debug_sdk_scenario}")
    list(GET _scenario_pair 0 _scenario_name)
    list(GET _scenario_pair 1 _scenario_outcome)
    add_test(NAME cmake-debug-sdk-guard-${_scenario_name}
        COMMAND ${CMAKE_COMMAND}
            -DPULP_SRC_DIR=${CMAKE_SOURCE_DIR}
            -DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/debug-sdk-guard-${_scenario_name}
            -DSCENARIO=${_scenario_name}
            -DEXPECTED_OUTCOME=${_scenario_outcome}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_debug_sdk_guard.cmake)
    set_tests_properties(cmake-debug-sdk-guard-${_scenario_name}
        PROPERTIES TIMEOUT 30)
endforeach()
unset(_pulp_debug_sdk_scenario)
unset(_scenario_pair)
unset(_scenario_name)
unset(_scenario_outcome)

# Android end-to-end smoke (issue #337). Runs the tools/scripts/android_smoke.sh
# against a locally attached emulator or device. Skipped by default because
# the smoke requires an already-booted AVD/device and an arm64-v8a APK —
# set `PULP_ANDROID_SMOKE_ENABLED=1` in the environment to opt in. The
# script returns 77 when the env var is unset (ctest SKIP_RETURN_CODE).
# See docs/guides/platforms/android.md for the prereq list.
if(UNIX)
    add_test(NAME android-smoke
        COMMAND bash ${CMAKE_SOURCE_DIR}/tools/scripts/android_smoke.sh
                --skip-build --allow-no-gpu)
    set_tests_properties(android-smoke PROPERTIES
        ENVIRONMENT "PULP_ANDROID_SMOKE_FORCE_CTEST_SKIP=1"
        SKIP_RETURN_CODE 77
        TIMEOUT 600
        LABELS "android;smoke;manual")
endif()

# Validation contract tests (Phase 1 — schema and reality snapshot)
add_executable(pulp-test-validation-contract test_validation_contract.cpp)
target_link_libraries(pulp-test-validation-contract PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-validation-contract)

# CLI design binding tests
add_executable(pulp-test-cli-design-binding test_cli_design_binding.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/design_binding.cpp
)
target_include_directories(pulp-test-cli-design-binding PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-design-binding PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-design-binding
    PROPERTIES LABELS "parser-import")

# CLI create target selection tests
add_executable(pulp-test-cli-create-targets test_cli_create_targets.cpp
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

# pulp #914 — `pulp run` flag parser unit tests. Pure parser, no I/O,
# no project resolution. The shell-out CLI behaviour for the same
# flags lives in test_cli_shellout.cpp.
add_executable(pulp-test-cli-run-options
    test_cli_run_options.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cmd_run_parse.cpp
)
target_include_directories(pulp-test-cli-run-options PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-run-options PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-run-options)

# pulp #914 — fixture binary for the `pulp run --headless --screenshot`
# shell-out test. Stands in for a plugin standalone: writes a tiny valid
# PNG when given --screenshot <path> (or PULP_SCREENSHOT=<path>) so the
# end-to-end CI-validation contract has a deterministic, dependency-free
# target.
add_executable(pulp-test-cli-run-fixture fixtures/cli_run_fixture.cpp)
set_target_properties(pulp-test-cli-run-fixture PROPERTIES
    OUTPUT_NAME "pulp-cli-run-fixture")

# CLI shell-out behaviour tests — launches the built `pulp` binary.
# Depends on pulp-cli so the binary exists at test time.
add_executable(pulp-test-cli-shellout test_cli_shellout.cpp)
target_link_libraries(pulp-test-cli-shellout PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-shellout pulp-cli)
endif()
# pulp #914 — depend on the fixture binary so the headless/screenshot
# end-to-end test has its target ready in the build tree.
if(TARGET pulp-test-cli-run-fixture)
    add_dependencies(pulp-test-cli-shellout pulp-test-cli-run-fixture)
endif()
if(TARGET pulp-import-design)
    add_dependencies(pulp-test-cli-shellout pulp-import-design)
endif()
catch_discover_tests(pulp-test-cli-shellout)

# pulp #776 — pin shell_quote's per-platform contract so it can't
# silently regress to the pre-fix shape that broke `git fetch origin`
# on Windows by writing doubled backslashes into clone URLs.
add_executable(pulp-test-cli-shell-quote
    test_cli_shell_quote.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp
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

# Plugin ↔ CLI skew banner helper (#551 Slice 6): shells out to bash
# with a staged `pulp` shim and asserts the banner copy + once-per-
# session semantics of `.agents/skills/_common/cli_version_check.sh`.
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

# CLI upgrade URL regression test (#352): asset filename stays
# "pulp-<platform>-<arch>.<ext>" — the version sits in the release tag.
# Test includes tools/cli/upgrade_url.hpp directly, no link deps needed.
add_executable(pulp-test-cli-upgrade-url test_cli_upgrade_url.cpp)
target_include_directories(pulp-test-cli-upgrade-url PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-cli-upgrade-url PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-upgrade-url)

# CLI upgrade install regression (#1673): a pre-cutover C++ CLI
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

# CLI version diagnostics (#499 Slice 1): pure-logic tests for the
# semver parser, plugin.json scraper, and skew analyzer that sit
# behind `pulp doctor --versions`. version_diag is deliberately free
# of cli_common link deps so we can compile it straight into the test.
add_executable(pulp-test-cli-version-diag
    test_cli_version_diag.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/version_diag.cpp
)
target_include_directories(pulp-test-cli-version-diag PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-version-diag PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-version-diag)

# CLI validator discovery (#743): pure-logic tests for the
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

# CLI projects registry (#499 / #552 Slice 1b): pure-logic tests for
# the ~/.pulp/projects.json read/write surface and the opt-in
# parent-directory scan. projects_registry is deliberately free of
# cli_common link deps for the same reason as version_diag.
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

# CLI FetchContent cache discovery + heal (#744): pure-logic tests for
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

# pulp #1814 — version-pinned SDK tarball filename helper.
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

# CLI package commands (#643): local-only tests for target/search/list/
# update/suggest/audit/add/remove behavior against staged project files
# and registry fixtures. Stays off remote fetch / archive extraction /
# install branches; links package_commands.cpp + package_registry.cpp
# directly so the command surface can be exercised without shelling out
# to the built binary.
add_executable(pulp-test-cli-package-commands
    test_cli_package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-package-commands PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-package-commands PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-commands)

# CLI package registry (#643): pure local tests for registry parsing,
# lock-file round trips, target TOML parsing/writing, semver, quality
# scoring, unsupported-target detection, and search ranking. This stays
# off remote refresh and install/archive flows.
add_executable(pulp-test-cli-package-registry
    test_cli_package_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_registry.cpp
)
target_include_directories(pulp-test-cli-package-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-package-registry PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-package-registry)

# CLI versioned import-design detection (#1031): unit + fixture-driven
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

# CLI tool registry (#643): local-only coverage for registry parsing,
# managed binary / Python wrapper discovery, deterministic install exits,
# uninstall, and command dispatch branches. Avoids downloads, archive
# extraction, Python package installs, and executing registry tools.
add_executable(pulp-test-cli-tool-registry
    test_cli_tool_registry.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/tool_registry.cpp
)
target_include_directories(pulp-test-cli-tool-registry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-cli-tool-registry PRIVATE
    pulp::platform
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-cli-tool-registry)

# CLI project bump (#499 Slice 7 / #564): pure-logic tests for the
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

# CLI project bump --all (#499 Slice 7 / #564): batch-level
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

# CLI project command (#244): direct command-level coverage for
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
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp
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

# CLI update-check (#499 Slice 2 / #547): pure-logic tests for the
# ~/.pulp/update-cache.json round-trip, is_cache_stale boundary,
# semver comparison, banner composition, TOML writer, and the
# Fetcher injection. update_check.cpp is deliberately free of
# cli_common link deps so the tests compile standalone. The #547
# PR added this test source but forgot the CMake target — Slice 5
# (#550) wires it in as part of the mode-enforcement work since
# the two modules share the cache file + banner hooks.
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

# CLI update-mode (#499 Slice 5 / #550): pure-logic tests for the
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

# CLI migration index (#499 Slice 3 / #548): pure-logic tests for the
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

# Child process tests
add_executable(pulp-test-child-process test_child_process.cpp)
target_link_libraries(pulp-test-child-process PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-child-process)

# Progress parser tests
add_executable(pulp-test-progress-parser test_progress_parser.cpp)
target_link_libraries(pulp-test-progress-parser PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-progress-parser)

# Stream tests (unified I/O interface)
add_executable(pulp-test-stream test_stream.cpp)
target_link_libraries(pulp-test-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-stream)

# #334: AudioFocusRegistry — cross-platform observer for OS audio-focus
# signals (Android AudioManager.OnAudioFocusChangeListener,
# AVAudioSession interruptions). Pure C++ unit tests — no platform deps.
add_executable(pulp-test-audio-focus test_audio_focus.cpp)
target_link_libraries(pulp-test-audio-focus PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-focus)

# #244: zero-fill helper for the Oboe input-frame-read short-read path.
# Header-only math, so no library link required beyond Catch2.
add_executable(pulp-test-frame-fill test_frame_fill.cpp)
target_link_libraries(pulp-test-frame-fill PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-frame-fill)

# #86: SysExAccumulator — cross-platform MIDI SysEx aggregation state
# machine shared by Android/ALSA/Windows/iOS MIDI backends. Header-only.
add_executable(pulp-test-sysex-accumulator test_sysex_accumulator.cpp)
target_link_libraries(pulp-test-sysex-accumulator PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sysex-accumulator)

# AsyncStream tests (Phase 2)
add_executable(pulp-test-async-stream test_async_stream.cpp)
target_link_libraries(pulp-test-async-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-async-stream)

# Network Stream tests (Phase 3)
add_executable(pulp-test-network-stream test_network_stream.cpp)
target_link_libraries(pulp-test-network-stream PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-network-stream)

# MessageChannel tests (Phase 4)
add_executable(pulp-test-memory-message-channel test_memory_message_channel.cpp)
target_link_libraries(pulp-test-memory-message-channel PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-memory-message-channel)

add_executable(pulp-test-websocket-channel test_websocket_channel.cpp)
target_link_libraries(pulp-test-websocket-channel PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-websocket-channel)

add_executable(pulp-test-osc-channel test_osc_channel.cpp)
target_link_libraries(pulp-test-osc-channel PRIVATE pulp::osc Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-osc-channel)

add_executable(pulp-test-json-rpc test_json_rpc.cpp)
target_link_libraries(pulp-test-json-rpc PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-json-rpc)

# Validation harness tests (Phase 2)
add_executable(pulp-test-validation-harness test_validation_harness.cpp)
target_link_libraries(pulp-test-validation-harness PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-validation-harness)

add_executable(pulp-test-plugin-state-io test_plugin_state_io.cpp)
target_link_libraries(pulp-test-plugin-state-io PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-plugin-state-io)

# AAX metadata/model tests
add_executable(pulp-test-aax-model test_aax_model.cpp)
target_link_libraries(pulp-test-aax-model PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-aax-model)

# Appcast/auto-update tests
add_executable(pulp-test-appcast test_appcast.cpp)
target_link_libraries(pulp-test-appcast PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-appcast)

# Android package/signing tests (host-side fake SDK/toolchain only)
add_executable(pulp-test-android-package test_android_package.cpp)
target_link_libraries(pulp-test-android-package PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-android-package)

# V3 gaps tests (Bias, MidiSequence, SubsectionReader, HostType, Primes, Expression)
add_executable(pulp-test-v3-gaps test_v3_gaps.cpp)
target_link_libraries(pulp-test-v3-gaps PRIVATE pulp::signal pulp::midi pulp::audio pulp::format pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-v3-gaps)

# Codesign tests
add_executable(pulp-test-codesign test_codesign.cpp)
target_link_libraries(pulp-test-codesign PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-codesign)

# Sync primitives tests (SeqLock, TripleBuffer)
add_executable(pulp-test-sync test_sync_primitives.cpp)
target_link_libraries(pulp-test-sync PRIVATE pulp::runtime Catch2::Catch2WithMain)
# `slow` (#1589): hammer tests run a writer + readers for 500 ms each
# to surface ordering bugs. Sanitizers.yml exercises the same code
# under TSan; PR fast-CI can skip the redundant smoke.
catch_discover_tests(pulp-test-sync PROPERTIES LABELS slow)

# License and BigInteger tests
add_executable(pulp-test-license test_license.cpp)
target_link_libraries(pulp-test-license PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-license)

# MIDI CI tests
add_executable(pulp-test-midi-ci test_midi_ci.cpp)
target_link_libraries(pulp-test-midi-ci PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-ci)

# AU v2 effect adapter MIDI-input tests — guards the 2026-04-22 fix that
# wired HandleMIDIEvent / HandleSysEx into `PulpAUEffect` and flipped the
# component type from aufx to aumf for descriptor.accepts_midi=true.
# Apple-only because the adapter header pulls in AudioUnitSDK (which is
# not linked on non-Apple lanes).
if(APPLE AND NOT PULP_IOS AND PULP_HAS_AUSDK)
    add_executable(pulp-test-au-v2-effect test_au_v2_effect.cpp)
    target_link_libraries(pulp-test-au-v2-effect PRIVATE
        pulp::format pulp::midi Catch2::Catch2WithMain)
    # pulp::format on Apple compiles at C++23 (AudioUnitSDK 1.4 requires
    # std::expected). Match it here so the adapter header parses.
    set_target_properties(pulp-test-au-v2-effect PROPERTIES CXX_STANDARD 23)
    catch_discover_tests(pulp-test-au-v2-effect)
endif()

# iOS AVAudioSession bridge (workstream 05 slice 5.2)
add_executable(pulp-test-ios-audio-session test_ios_audio_session.cpp)
target_link_libraries(pulp-test-ios-audio-session PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ios-audio-session)
if(NOT PULP_IOS)
    add_executable(pulp-test-pulp-bridge test_pulp_bridge.cpp)
    target_link_libraries(pulp-test-pulp-bridge PRIVATE
        pulp::format
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-pulp-bridge)
endif()
# ARA scaffolding (workstream 06 slice 6.1)
add_executable(pulp-test-ara test_ara.cpp)
target_link_libraries(pulp-test-ara PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ara)
# CLAP ARA extension end-to-end (workstream 06 A2a)
add_executable(pulp-test-clap-ara-extension test_clap_ara_extension.cpp)
target_link_libraries(pulp-test-clap-ara-extension PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clap-ara-extension)
# Processor ARA hook (workstream 06 slice 6.3a)
add_executable(pulp-test-processor-ara-hook test_processor_ara_hook.cpp)
target_link_libraries(pulp-test-processor-ara-hook PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-processor-ara-hook)
# Processor and descriptor default contracts
add_executable(pulp-test-processor-defaults test_processor_defaults.cpp)
target_link_libraries(pulp-test-processor-defaults PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-processor-defaults)
# TableModel data/sort layer (workstream 07 slice 7.3)
add_executable(pulp-test-table-model test_table_model.cpp)
target_link_libraries(pulp-test-table-model PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-table-model)
# ModulationMatrix data model (workstream 07 slice 7.1)
add_executable(pulp-test-modulation-matrix test_modulation_matrix.cpp)
target_link_libraries(pulp-test-modulation-matrix PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-modulation-matrix)
# ModulationMatrixWidget canvas widget (workstream 07 slice B5)
add_executable(pulp-test-modulation-matrix-widget test_modulation_matrix_widget.cpp)
target_link_libraries(pulp-test-modulation-matrix-widget PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-modulation-matrix-widget)
# PluginDescriptor vendor_url + vendor_email (workstream 01 slice 1.8)
add_executable(pulp-test-vendor-url test_vendor_url.cpp)
target_link_libraries(pulp-test-vendor-url PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-vendor-url)
# Image cache (workstream 07 slice 7.4)
add_executable(pulp-test-image-cache test_image_cache.cpp)
target_link_libraries(pulp-test-image-cache PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-cache)
# ImageView ↔ ImageCache wiring (workstream 07 B4)
add_executable(pulp-test-image-view-cache test_image_view_cache.cpp)
target_link_libraries(pulp-test-image-view-cache PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-view-cache)

# Accessibility tree snapshot (workstream 04 slice 4.4)
add_executable(pulp-test-accessibility-tree test_accessibility_tree.cpp)
target_link_libraries(pulp-test-accessibility-tree PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-accessibility-tree)
# A/B compare (workstream 07 slice 7.2)
add_executable(pulp-test-ab-compare test_ab_compare.cpp)
target_link_libraries(pulp-test-ab-compare PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ab-compare)
# ViewSize::aspect_ratio field (workstream 07 slice 7.5b)
add_executable(pulp-test-view-size-aspect test_view_size_aspect.cpp)
target_link_libraries(pulp-test-view-size-aspect PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-size-aspect)

# pulp #59/#63/#64/#65 — WindowHost::set_design_viewport pure-math
# coverage. Locks down the scale + letterbox transform that the mac
# GPU host (and any other future platform host) uses to fit
# fixed-design content into the current window.
add_executable(pulp-test-view-design-viewport test_view_design_viewport.cpp)
target_link_libraries(pulp-test-view-design-viewport PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-design-viewport)

# View host bridges — screenshot/window/plugin-view (#299)
add_executable(pulp-test-view-host-bridge test_view_host_bridge.cpp)
if(APPLE AND NOT PULP_IOS)
    target_sources(pulp-test-view-host-bridge PRIVATE test_view_host_bridge_mac.mm)
endif()
target_link_libraries(pulp-test-view-host-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-host-bridge)
# Scan blacklist (workstream 03 slice 3.3a)
add_executable(pulp-test-scan-blacklist test_scan_blacklist.cpp)
target_link_libraries(pulp-test-scan-blacklist PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow` (#1589): file-mtime sleep loops (rebuilt-plugin-not-blacklisted
# flow waits on filesystem timestamp resolution). Each test ~1s.
catch_discover_tests(pulp-test-scan-blacklist PROPERTIES LABELS slow)
# Hosted editor API (workstream 03 slice 3.4)
add_executable(pulp-test-hosted-editor test_hosted_editor.cpp)
target_link_libraries(pulp-test-hosted-editor PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-hosted-editor)
# Accessibility announcements (workstream 04 slice 4.3)
add_executable(pulp-test-announce test_announce.cpp)
target_link_libraries(pulp-test-announce PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-announce)
# ARA core type layer (workstream 06 slice 6.2)
add_executable(pulp-test-ara-types test_ara_types.cpp)
target_link_libraries(pulp-test-ara-types PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ara-types)
# Processor on_host_transport_changed hook (workstream 01 slice 1.11)
add_executable(pulp-test-transport-hook test_transport_hook.cpp)
target_link_libraries(pulp-test-transport-hook PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-transport-hook)
# Scan cache (workstream 03 slice 3.1)
add_executable(pulp-test-scan-cache test_scan_cache.cpp)
target_link_libraries(pulp-test-scan-cache PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow` (#1589): each scenario waits for filesystem mtime resolution
# (~1s each on every platform). Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scan-cache PROPERTIES LABELS slow)
# Processor memory-pressure hook (workstream 05 slice 5.3)
add_executable(pulp-test-memory-pressure test_memory_pressure.cpp)
target_link_libraries(pulp-test-memory-pressure PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-memory-pressure)
# iOS background-audio descriptor flag (workstream 05 slice 5.5)
add_executable(pulp-test-ios-background-audio-flag test_ios_background_audio_flag.cpp)
target_link_libraries(pulp-test-ios-background-audio-flag PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ios-background-audio-flag)

# MidiBuffer UMP sidecar (workstream 02 slice 2.6a)
add_executable(pulp-test-midi-buffer-ump test_midi_buffer_ump.cpp)
target_link_libraries(pulp-test-midi-buffer-ump PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-buffer-ump)
# MidiBuffer SysEx sidecar (workstream 01 — full MIDI vocabulary)
add_executable(pulp-test-midi-buffer-sysex test_midi_buffer_sysex.cpp)
target_link_libraries(pulp-test-midi-buffer-sysex PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-buffer-sysex)
# Raw MIDI byte-stream parser — shared by ALSA raw-midi and any future
# raw-byte MIDI transport. Tests aborted-sysex recovery (#406 codex P2),
# realtime-inside-sysex passthrough (#239), runaway cap, and multi-read
# accumulator state.
add_executable(pulp-test-raw-midi-parser test_raw_midi_parser.cpp)
target_link_libraries(pulp-test-raw-midi-parser PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-raw-midi-parser)
# Win MIDI MIM_LONGDATA SysEx + QPC timestamps (#19 / #245 partial).
# Cross-platform — compiles everywhere; Windows-specific cases are
# guarded behind #ifdef _WIN32.
add_executable(pulp-test-winmidi-sysex test_winmidi_sysex.cpp)
target_link_libraries(pulp-test-winmidi-sysex PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-winmidi-sysex)
# Accessibility provider cross-platform entry (workstream 04 slices 4.1 + 4.2)
add_executable(pulp-test-accessibility-provider test_accessibility_provider.cpp)
target_link_libraries(pulp-test-accessibility-provider PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-accessibility-provider)
# PluginDescriptor validation (workstream 01 slice 1.9)
add_executable(pulp-test-descriptor-validation test_descriptor_validation.cpp)
target_link_libraries(pulp-test-descriptor-validation PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-descriptor-validation)
# PluginInfo metadata shape (workstream 03 slice 3.7)
add_executable(pulp-test-plugin-info-metadata test_plugin_info_metadata.cpp)
target_link_libraries(pulp-test-plugin-info-metadata PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-plugin-info-metadata)
# UIA mapping (workstream 04 slice 4.1a)
add_executable(pulp-test-uia-mapping test_uia_mapping.cpp)
target_link_libraries(pulp-test-uia-mapping PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-uia-mapping)

# AudioSystem hotplug base plumbing (workstream 02 slice 2.7)
add_executable(pulp-test-audio-system-hotplug test_audio_system_hotplug.cpp)
target_link_libraries(pulp-test-audio-system-hotplug PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-system-hotplug)

# WASAPI capture path (#243). Windows-only; cross-platform stub keeps
# CI green elsewhere. Skips at runtime on hosts without a default
# capture endpoint.
add_executable(pulp-test-wasapi-input test_wasapi_input.cpp)
target_link_libraries(pulp-test-wasapi-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-input)

# ALSA capture + real device metadata (#20 / #215). Linux-only;
# cross-platform stub keeps CI green elsewhere. Skips at runtime on
# hosts without an ALSA-routable default device.
add_executable(pulp-test-alsa-input test_alsa_input.cpp)
target_link_libraries(pulp-test-alsa-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-alsa-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-alsa-input)
# Processor on_host_tempo_changed hook (workstream 01 slice 1.10)
add_executable(pulp-test-tempo-hook test_tempo_hook.cpp)
target_link_libraries(pulp-test-tempo-hook PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-tempo-hook)
# Resizable plugin shell (workstream 07 slice 7.5)
add_executable(pulp-test-resizable-shell test_resizable_shell.cpp)
target_link_libraries(pulp-test-resizable-shell PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-resizable-shell)
# ATK / AT-SPI mapping (workstream 04 slice 4.2a)
add_executable(pulp-test-atk-mapping test_atk_mapping.cpp)
target_link_libraries(pulp-test-atk-mapping PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-atk-mapping)
# Running-status MIDI parser (workstream 02 slice 2.5)
add_executable(pulp-test-running-status test_running_status.cpp)
target_link_libraries(pulp-test-running-status PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-running-status)
# Background scanner (workstream 03 slice 3.2)
add_executable(pulp-test-background-scanner test_background_scanner.cpp)
target_link_libraries(pulp-test-background-scanner PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-background-scanner)

# TextShaper (PreText-style measure-once-reflow-forever)
add_executable(pulp-test-text-shaper test_text_shaper.cpp)
target_link_libraries(pulp-test-text-shaper PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-shaper)

# Analytics tests
add_executable(pulp-test-analytics test_analytics.cpp)
target_link_libraries(pulp-test-analytics PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-analytics)

# Crypto tests (SHA-256, MD5, AES, machine ID)
add_executable(pulp-test-crypto test_crypto.cpp)
target_link_libraries(pulp-test-crypto PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-crypto)

# IPC (InterprocessConnection) tests
add_executable(pulp-test-ipc test_ipc.cpp)
target_link_libraries(pulp-test-ipc PRIVATE pulp::events pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ipc)

add_executable(pulp-test-ipc-endpoints test_ipc_endpoints.cpp)
target_link_libraries(pulp-test-ipc-endpoints PRIVATE pulp::events pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ipc-endpoints)

# AttributedString and TextLayout tests
add_executable(pulp-test-attributed-string test_attributed_string.cpp)
target_link_libraries(pulp-test-attributed-string PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-attributed-string)

# i18n translation tests
add_executable(pulp-test-i18n test_i18n.cpp)
target_link_libraries(pulp-test-i18n PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-i18n)

# StateTree and ObservableValue tests
add_executable(pulp-test-state-tree test_state_tree.cpp)
target_link_libraries(pulp-test-state-tree PRIVATE pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-state-tree)

# GUI component tests (table, toolbar, concertina, buttons, lasso)
add_executable(pulp-test-gui-components test_gui_components.cpp)
target_link_libraries(pulp-test-gui-components PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gui-components)

add_executable(pulp-test-table-list-box test_table_list_box.cpp)
target_link_libraries(pulp-test-table-list-box PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-table-list-box)

# CodeEditor / FileBasedDocument / RecentlyOpenedFilesList tests
add_executable(pulp-test-code-editor test_code_editor.cpp)
target_link_libraries(pulp-test-code-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-code-editor)

# PropertiesFile JSON persistence tests
add_executable(pulp-test-properties test_properties_file.cpp)
target_link_libraries(pulp-test-properties PRIVATE pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-properties)

# DSP enhancement tests (dry/wet mixer, processor duplicator, matrix, special functions)
add_executable(pulp-test-dsp-enhancements test_dsp_enhancements.cpp)
target_link_libraries(pulp-test-dsp-enhancements PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsp-enhancements)

# Animation & 3D math tests
add_executable(pulp-test-animation-3d test_animation_3d.cpp)
target_link_libraries(pulp-test-animation-3d PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animation-3d)

# AnimatorSet / 3D types tests
add_executable(pulp-test-animator-set test_animator_set.cpp)
target_link_libraries(pulp-test-animator-set PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animator-set)

# Runtime utility tests (mmap, temp file, dynlib, base64, range, child process)
add_executable(pulp-test-runtime-utils test_runtime_utils.cpp)
target_link_libraries(pulp-test-runtime-utils PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-runtime-utils)

# XML and ZIP/GZIP compression tests
add_executable(pulp-test-xml-zip test_xml_zip.cpp)
target_link_libraries(pulp-test-xml-zip PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-xml-zip)

# SIMD operations and aligned buffer tests
add_executable(pulp-test-simd test_simd.cpp)
target_link_libraries(pulp-test-simd PRIVATE pulp::runtime pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-simd)

# Drag-and-drop tests
add_executable(pulp-test-dnd test_drag_drop.cpp)
target_link_libraries(pulp-test-dnd PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dnd)

# OSC tests
add_executable(pulp-test-osc test_osc.cpp)
target_link_libraries(pulp-test-osc PRIVATE pulp::osc Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-osc)

# ImageConvolutionKernel tests
add_executable(pulp-test-image-convolution test_image_convolution.cpp)
target_link_libraries(pulp-test-image-convolution PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-convolution)

# RectangleList geometry tests
add_executable(pulp-test-rectangle-list test_rectangle_list.cpp)
target_link_libraries(pulp-test-rectangle-list PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-rectangle-list)

# OSC Bundle serialization tests
add_executable(pulp-test-osc-bundle test_osc_bundle.cpp)
target_link_libraries(pulp-test-osc-bundle PRIVATE pulp::osc Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-osc-bundle)

# SVG tests
add_executable(pulp-test-svg test_svg.cpp)
target_link_libraries(pulp-test-svg PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-svg)

# Effects tests
add_executable(pulp-test-effects test_effects.cpp)
target_link_libraries(pulp-test-effects PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-effects)

# Signal/DSP tests
add_executable(pulp-test-signal test_signal.cpp)
target_link_libraries(pulp-test-signal PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-signal)

# Biquad filter tests
add_executable(pulp-test-biquad test_biquad.cpp)
target_link_libraries(pulp-test-biquad PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-biquad)

# Multi-channel meter edge tests (#645)
add_executable(pulp-test-multi-channel-meter test_multi_channel_meter.cpp)
target_link_libraries(pulp-test-multi-channel-meter PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-multi-channel-meter)

# DSL processor contract tests (FaustProcessor + PulpFaustUI + PulpFaustMeta)
add_executable(pulp-test-dsl-processor test_dsl_processor.cpp)
target_link_libraries(pulp-test-dsl-processor PRIVATE
    pulp::dsl pulp::format pulp::state pulp::audio pulp::midi
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsl-processor)

# Convolution engine tests
add_executable(pulp-test-convolver test_convolver.cpp)
target_link_libraries(pulp-test-convolver PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-convolver)

# Test signal source (sine tone, file playback)
add_executable(pulp-test-test-signal test_test_signal.cpp)
target_link_libraries(pulp-test-test-signal PRIVATE pulp::standalone Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-test-signal)

# Standalone editor chrome helpers
add_executable(pulp-test-standalone-editor-chrome test_standalone_editor_chrome.cpp)
target_link_libraries(pulp-test-standalone-editor-chrome PRIVATE pulp::standalone Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-standalone-editor-chrome)

# Headless screenshot capture state machine (pulp #468)
add_executable(pulp-test-screenshot-capture test_screenshot_capture.cpp)
target_link_libraries(pulp-test-screenshot-capture PRIVATE pulp::standalone Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-screenshot-capture)

# STFT and audio visualization signal tests
add_executable(pulp-test-stft test_stft.cpp)
target_link_libraries(pulp-test-stft PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-stft)

# Visualization bridge and widget tests
add_executable(pulp-test-visualization test_visualization.cpp)
target_link_libraries(pulp-test-visualization PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-visualization)

# Clipboard tests
add_executable(pulp-test-clipboard test_clipboard.cpp)
target_link_libraries(pulp-test-clipboard PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clipboard
    PROPERTIES RESOURCE_LOCK system-clipboard)

# FileDialog backend-registration tests (#301)
add_executable(pulp-test-file-dialog test_file_dialog.cpp)
target_link_libraries(pulp-test-file-dialog PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-file-dialog)

# Platform tests
add_executable(pulp-test-platform test_platform.cpp)
target_link_libraries(pulp-test-platform PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform)

# Permissions tests
add_executable(pulp-test-permissions test_permissions.cpp)
target_link_libraries(pulp-test-permissions PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-permissions)

# Environment API tests (#342)
add_executable(pulp-test-environment test_environment.cpp)
target_link_libraries(pulp-test-environment PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-environment)

# Runtime tests
add_executable(pulp-test-runtime test_runtime.cpp)
target_link_libraries(pulp-test-runtime PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-runtime)

# Sync-primitive hammer (TSan-focused, #412 step 4).
# Runs under the tag-scoped TSan subset via the [concurrent][race]
# tags embedded in each TEST_CASE (matches the 'Race|Concurrent'
# alternation in sanitizers.yml's --tests-regex).
add_executable(pulp-test-sync-race-hammer test_sync_race_hammer.cpp)
target_link_libraries(pulp-test-sync-race-hammer PRIVATE pulp::runtime Catch2::Catch2WithMain)
# `slow` (#1589): N-thread race smoke. Sanitizer builds also pick this
# up via the dedicated sanitizers.yml workflow, so PR fast-CI doesn't
# need to re-run it.
catch_discover_tests(pulp-test-sync-race-hammer PROPERTIES LABELS slow)

# Events tests
add_executable(pulp-test-events test_events.cpp)
target_link_libraries(pulp-test-events PRIVATE pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-events)

add_executable(pulp-test-events-async-helpers test_events_async_helpers.cpp)
target_link_libraries(pulp-test-events-async-helpers PRIVATE pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-events-async-helpers)

add_executable(pulp-test-events-timer-helpers test_events_timer_helpers.cpp)
target_link_libraries(pulp-test-events-timer-helpers PRIVATE pulp::events Catch2::Catch2WithMain)
# `slow` (#1589): Timer hammer + UAF-free destroy-while-dispatched
# tests deliberately exercise message-loop pacing (~0.3-1.5 sec each
# depending on platform). Excluded from PR fast-CI; sanitizers.yml
# still covers Timer under TSan.
catch_discover_tests(pulp-test-events-timer-helpers PROPERTIES LABELS slow)

# Audio file I/O tests
add_executable(pulp-test-audio-file test_audio_file.cpp)
target_link_libraries(pulp-test-audio-file PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-file)

add_executable(pulp-test-offline-processor-edges test_offline_processor_edges.cpp)
target_link_libraries(pulp-test-offline-processor-edges PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-offline-processor-edges)

add_executable(pulp-test-ogg-reader test_ogg_reader.cpp)
target_link_libraries(pulp-test-ogg-reader PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ogg-reader)

# Audio tests
add_executable(pulp-test-audio test_audio.cpp)
target_link_libraries(pulp-test-audio PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio)

add_executable(pulp-test-system-volume test_system_volume.cpp)
target_link_libraries(pulp-test-system-volume PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-system-volume)

# Excerpt window enumeration tests
add_executable(pulp-test-audio-excerpt test_audio_excerpt.cpp)
target_link_libraries(pulp-test-audio-excerpt PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-excerpt)

# Repo-level audio tooling tests
add_executable(pulp-test-audio-tools test_audio_tools.cpp)
target_link_libraries(pulp-test-audio-tools PRIVATE pulp::tool-audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-tools)

# MCP server protocol tests
add_executable(pulp-test-mcp-server test_mcp_server.cpp)
target_link_libraries(pulp-test-mcp-server PRIVATE pulp::tool-audio Catch2::Catch2WithMain)
# test_mcp_server.cpp #include's ../tools/mcp/pulp_mcp.cpp directly (via
# `#define main pulp_mcp_main_for_test`), so it picks up the same
# generated version header pulp-mcp itself sees. The generated header
# lives under the binary tree's tools/mcp/ directory.
target_include_directories(pulp-test-mcp-server PRIVATE
    "${CMAKE_BINARY_DIR}/tools/mcp")
add_dependencies(pulp-test-mcp-server pulp-mcp)
catch_discover_tests(pulp-test-mcp-server)

# MIDI tests
add_executable(pulp-test-midi test_midi.cpp)
target_link_libraries(pulp-test-midi PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi)

add_executable(pulp-test-midi-file test_midi_file.cpp)
target_link_libraries(pulp-test-midi-file PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-file)

# State tests
add_executable(pulp-test-state test_state.cpp)
target_link_libraries(pulp-test-state PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-state)

# Binding tests
add_executable(pulp-test-binding test_binding.cpp)
target_link_libraries(pulp-test-binding PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-binding)

# Headless adapter tests
add_executable(pulp-test-headless test_headless.cpp)
target_link_libraries(pulp-test-headless PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-headless)

# Diagnostic reporter tests
add_executable(pulp-test-diagnostic test_diagnostic_reporter.cpp)
target_link_libraries(pulp-test-diagnostic PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-diagnostic)

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-clap-entry)

    # CLAP inbound/outbound MIDI event coverage — pins the CLAP_EVENT_MIDI,
    # CLAP_EVENT_NOTE_EXPRESSION, CLAP_EVENT_NOTE_CHOKE, CLAP_EVENT_MIDI2
    # handlers added alongside feature/clap-midi-cc-coverage.
    #
    # Unlike `pulp-test-clap-entry` above, this test DOES NOT re-compile
    # clap_adapter.cpp into its own TU. The test drives the adapter via
    # its public header surface (clap_init / clap_activate / clap_process
    # — see core/format/include/pulp/format/clap_adapter.hpp), so linking
    # against pulp::format is sufficient. Keeping the direct compilation
    # out means the coverage artifact reports exactly one TU for
    # clap_adapter.cpp (the library's), which is what CI's diff-cover
    # gate reads — avoids the split-TU problem that produced "96% local
    # / 62% CI" reports on PR #627.
    add_executable(pulp-test-clap-midi-events test_clap_midi_events.cpp)
    target_link_libraries(pulp-test-clap-midi-events PRIVATE pulp::format clap Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-clap-midi-events)
endif()

if(PULP_HAS_VST3)
    add_executable(pulp-test-vst3-plugin-state
        test_vst3_plugin_state.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp
    )
    target_link_libraries(pulp-test-vst3-plugin-state PRIVATE pulp::format Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-vst3-plugin-state PRIVATE
        PULP_VST3=1
        PULP_VST3_GUI=1
    )
    catch_discover_tests(pulp-test-vst3-plugin-state)
endif()

if(APPLE AND PULP_HAS_AUSDK)
    add_executable(pulp-test-au-plugin-state
        test_au_plugin_state.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_adapter.mm
    )
    target_link_libraries(pulp-test-au-plugin-state PRIVATE
        pulp::format
        Catch2::Catch2WithMain
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework Foundation"
    )
    set_target_properties(pulp-test-au-plugin-state PROPERTIES
        CXX_STANDARD 23
        OBJCXX_STANDARD 23
    )
    catch_discover_tests(pulp-test-au-plugin-state)
endif()

# LV2 adapter tests
if(PULP_HAS_LV2)
    add_executable(pulp-test-lv2-adapter test_lv2_adapter.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-test-lv2-adapter PRIVATE pulp::format lv2-headers Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-lv2-adapter)

    add_executable(pulp-test-lv2-host-discovery test_lv2_host_discovery.cpp)
    target_include_directories(pulp-test-lv2-host-discovery PRIVATE
        ${CMAKE_SOURCE_DIR}/core/host/src)
    target_link_libraries(pulp-test-lv2-host-discovery PRIVATE
        pulp::host
        lv2-headers
        Catch2::Catch2WithMain)

    if(UNIX)
        set(_pulp_lv2_slot_probe_bundle
            "${CMAKE_CURRENT_BINARY_DIR}/Lv2SlotProbe.lv2")
        set(_pulp_lv2_slot_probe_binary
            "${_pulp_lv2_slot_probe_bundle}/lv2-slot-probe${CMAKE_SHARED_MODULE_SUFFIX}")

        add_library(pulp-test-lv2-slot-probe MODULE
            fixtures/lv2_slot_probe.cpp)
        target_link_libraries(pulp-test-lv2-slot-probe PRIVATE lv2-headers)
        set_target_properties(pulp-test-lv2-slot-probe PROPERTIES
            PREFIX ""
            OUTPUT_NAME "lv2-slot-probe"
            LIBRARY_OUTPUT_DIRECTORY "${_pulp_lv2_slot_probe_bundle}")

        file(MAKE_DIRECTORY "${_pulp_lv2_slot_probe_bundle}")
        string(CONCAT _pulp_lv2_slot_probe_ttl [=[
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .

<http://example.com/pulp/lv2-slot-probe>
    a lv2:Plugin ;
    lv2:binary <lv2-slot-probe]=]
    "${CMAKE_SHARED_MODULE_SUFFIX}"
    [=[> ;
    lv2:port
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 0 ;
        lv2:name "Input L"
    ] ,
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 1 ;
        lv2:name "Input R"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 2 ;
        lv2:name "Output L"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 3 ;
        lv2:name "Output R"
    ] ,
    [
        a lv2:InputPort , lv2:ControlPort ;
        lv2:index 4 ;
        lv2:name "Probe Gain" ;
        lv2:default 0.5 ;
        lv2:minimum -2.0 ;
        lv2:maximum 4.0
    ] .
]=])
        file(WRITE "${_pulp_lv2_slot_probe_bundle}/plugin.ttl"
            "${_pulp_lv2_slot_probe_ttl}")

        add_dependencies(pulp-test-lv2-host-discovery
            pulp-test-lv2-slot-probe)
        target_compile_definitions(pulp-test-lv2-host-discovery PRIVATE
            PULP_TEST_LV2_SLOT_PROBE_BUNDLE="${_pulp_lv2_slot_probe_bundle}"
            PULP_TEST_LV2_SLOT_PROBE_BINARY="${_pulp_lv2_slot_probe_binary}")
    endif()

    catch_discover_tests(pulp-test-lv2-host-discovery)
endif()

# NSIS installer script generation tests (cross-platform — pure string output)
add_executable(pulp-test-nsis-installer test_nsis_installer.cpp
    ${CMAKE_SOURCE_DIR}/ship/platform/win/installer_win.cpp
)
target_link_libraries(pulp-test-nsis-installer PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-nsis-installer)

# Image decode / glTF texture pipeline tests
add_executable(pulp-test-image-decode test_image_decode.cpp)
target_link_libraries(pulp-test-image-decode PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-decode)

# GPU surface tests
if(PULP_ENABLE_GPU)
    set(PULP_GPU_TEST_DISCOVERY_ARGS)
    if(WIN32 AND TARGET webgpu)
        list(APPEND PULP_GPU_TEST_DISCOVERY_ARGS
            DISCOVERY_MODE PRE_TEST
            DL_PATHS "$<TARGET_FILE_DIR:webgpu>"
        )
    endif()

    add_executable(pulp-test-gpu-surface test_gpu_surface.cpp)
    target_link_libraries(pulp-test-gpu-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # Skia surface test
    add_executable(pulp-test-skia-surface test_skia_surface.cpp)
    target_link_libraries(pulp-test-skia-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-skia-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU compute tests (Phase 11 feasibility)
    add_executable(pulp-test-gpu-compute test_gpu_compute.cpp)
    target_link_libraries(pulp-test-gpu-compute PRIVATE pulp::render pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-compute ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # GPU compute staging-buffer pool tests (zero-copy Slice 1).
    # Reaches into core/render/src/ for the internal pool header — the pool
    # is an implementation detail, not a public API, so the test directly
    # includes the source header rather than going through a public link.
    add_executable(pulp-test-gpu-compute-pool test_gpu_compute_pool.cpp)
    target_link_libraries(pulp-test-gpu-compute-pool
        PRIVATE pulp::render Catch2::Catch2WithMain)
    # Pool tests need Dawn headers directly (webgpu/webgpu_cpp.h,
    # dawn/native/DawnNative.h). pulp::render pulls these in privately,
    # so we mirror the include paths here. `SKIA_INCLUDE_DIRS` is set
    # by FindSkia.cmake; the `/dawn` suffix targets the generated-
    # header directory where `webgpu/webgpu_cpp.h` lives.
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-gpu-compute-pool
            PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-gpu-compute-pool PRIVATE
            ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-test-gpu-compute-pool PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
    endif()
    catch_discover_tests(pulp-test-gpu-compute-pool ${PULP_GPU_TEST_DISCOVERY_ARGS})
endif()

# Animation tests
add_executable(pulp-test-animation test_animation.cpp)
target_link_libraries(pulp-test-animation PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-animation)

# FrameClock tests
add_executable(pulp-test-frame-clock test_frame_clock.cpp)
target_link_libraries(pulp-test-frame-clock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-frame-clock)

# Motion (agent-first motion observability) — Phase 0 bedrock tests
add_executable(pulp-test-motion test_motion.cpp)
target_link_libraries(pulp-test-motion PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion)

# MotionPreferences — Phase 8 reduced-motion policy + animation honoring
add_executable(pulp-test-motion-preferences test_motion_preferences.cpp)
target_link_libraries(pulp-test-motion-preferences PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-preferences)

# Motion inspector — Phase 1 Motion.* protocol-domain tests
if(TARGET pulp::inspect)
    add_executable(pulp-test-motion-inspector test_motion_inspector.cpp)
    target_link_libraries(pulp-test-motion-inspector PRIVATE
        pulp::view pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-inspector)
endif()

# Motion end-to-end animation smoke — mirrors what a plugin author
# would write: drive a real Tween, publish_value each tick, read the
# fixture back, assert the shape matches the configured animation.
add_executable(pulp-test-motion-animation-smoke test_motion_animation_smoke.cpp)
target_link_libraries(pulp-test-motion-animation-smoke PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-animation-smoke)

# Motion visual-analysis self-check — Phase 4 pipeline smoke.
# Skips cleanly (exit 3) when Python deps are absent so CI without
# numpy/Pillow/scikit-image doesn't false-fail.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME pulp-motion-visual-self-check
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_self_check.py
    )
    set_tests_properties(pulp-motion-visual-self-check PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
    )
endif()

# Widget animation behavior tests
add_executable(pulp-test-widget-animation test_widget_animation.cpp)
target_link_libraries(pulp-test-widget-animation PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-animation)

# Auto UI tests
add_executable(pulp-test-auto-ui test_auto_ui.cpp)
target_link_libraries(pulp-test-auto-ui PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-auto-ui)

# ViewBridge lifecycle tests
add_executable(pulp-test-view-bridge test_view_bridge.cpp)
target_link_libraries(pulp-test-view-bridge PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-bridge)

# Remote View Protocol loopback tests (Feature 1, Phase 4)
add_executable(pulp-test-remote-view test_remote_view.cpp)
target_link_libraries(pulp-test-remote-view PRIVATE pulp::format pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-remote-view)

# Script engine tests (legacy API)
add_executable(pulp-test-script test_script_engine.cpp)
target_link_libraries(pulp-test-script PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-script)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow` (#1589): ScriptedUiSession reload tests sleep on file-watcher
# debounce + filesystem mtime; ~0.5-1 sec each. Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scripted-ui PROPERTIES LABELS slow)

# JS engine abstraction tests (shared across all backends)
add_executable(pulp-test-js-engine test_js_engine.cpp)
target_link_libraries(pulp-test-js-engine PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-js-engine)

# Theme/design token tests
add_executable(pulp-test-theme test_theme.cpp)
target_link_libraries(pulp-test-theme PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme)

# Screenshot tests (macOS only — uses CoreGraphics bitmap context)
if(APPLE)
    add_executable(pulp-test-screenshot test_screenshot.cpp)
    target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot)
endif()

# Canvas tests
add_executable(pulp-test-canvas test_canvas.cpp)
target_link_libraries(pulp-test-canvas PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    # CoreGraphicsCanvas tests (#929) build a CGBitmapContext directly to
    # verify CGContextClearRect actually clears destination pixels.
    # pulp #1524 adds ImageIO for CGImageDestinationCreateWithURL (PNG
    # encode of a pattern fixture) and CoreServices for the public.png UTI.
    target_link_libraries(pulp-test-canvas PRIVATE
        "-framework CoreGraphics"
        "-framework ImageIO"
        "-framework CoreServices"
    )
endif()
# pulp #1150 — public font-registration API tests use a bundled .ttf as a
# fixture. external/fonts/Inter-Regular.ttf is the canonical "always
# available" font in the repo (also baked into pulp-canvas via
# pulp_add_binary_data). Pass the absolute path through a compile def so
# the test binary can find it regardless of CWD when run via ctest.
target_compile_definitions(pulp-test-canvas PRIVATE
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\"")
catch_discover_tests(pulp-test-canvas)

# Emoji-parity tests — cover the segmenter + cluster-aware shaping +
# letter-spacing + cache invalidation paths. Also writes PNGs to
# `$PULP_EMOJI_TEST_PNG_DIR` (default /tmp/pulp-emoji-validation/) for
# visual inspection of real-emoji rendering through Skia.
add_executable(pulp-test-canvas-emoji test_canvas_emoji.cpp)
target_link_libraries(pulp-test-canvas-emoji PRIVATE
    pulp::canvas
    Catch2::Catch2WithMain
)
catch_discover_tests(pulp-test-canvas-emoji)

# SDF glyph atlas exploration (#76)
add_executable(pulp-test-sdf-atlas test_sdf_atlas.cpp)
target_link_libraries(pulp-test-sdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-atlas)

# MSDF glyph atlas (Phase 2 scaffold)
add_executable(pulp-test-msdf-atlas test_msdf_atlas.cpp)
target_link_libraries(pulp-test-msdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-msdf-atlas)

# PSDF glyph atlas + vector-fallback (Phase 3 scaffold)
add_executable(pulp-test-psdf-atlas test_psdf_atlas.cpp)
target_link_libraries(pulp-test-psdf-atlas PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-psdf-atlas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
add_executable(pulp-test-sdf-text test_sdf_text.cpp)
target_link_libraries(pulp-test-sdf-text PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-text)

# SDF effects layer (Phase 4 — host-side API + presets)
add_executable(pulp-test-sdf-effects test_sdf_effects.cpp)
target_link_libraries(pulp-test-sdf-effects PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-effects)

# Runtime SDF atlas cache (Phase 5 — LRU + frame-based eviction)
add_executable(pulp-test-sdf-atlas-cache test_sdf_atlas_cache.cpp)
target_link_libraries(pulp-test-sdf-atlas-cache PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-atlas-cache)

# Software SDF renderer (end-to-end headless raster)
add_executable(pulp-test-sdf-software-renderer test_sdf_software_renderer.cpp)
target_link_libraries(pulp-test-sdf-software-renderer PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sdf-software-renderer)

# Runtime path → SDF conversion (Phase 5 — on-device SDF generation)
add_executable(pulp-test-path-to-sdf test_path_to_sdf.cpp)
target_link_libraries(pulp-test-path-to-sdf PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-path-to-sdf)

# View and layout tests
add_executable(pulp-test-view test_view.cpp)
target_link_libraries(pulp-test-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view)

# DirtyTracker, GpuGraphRenderer, and RenderLoop timer-state tests
# are pure helpers; keep them available in GPU-off sanitizer and coverage builds.
add_executable(pulp-test-dirty-tracker test_dirty_tracker.cpp)
target_include_directories(pulp-test-dirty-tracker PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-dirty-tracker PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dirty-tracker)

# Partial-rendering POC test. Pins the DirtyTracker contract the
# macOS GPU host's tracker wiring relies on.
# The host integration itself needs Skia + a Metal device for end-to-end
# coverage, so that lives in the follow-up partial-rendering slice.
add_executable(pulp-test-partial-rendering-poc test_partial_rendering_poc.cpp)
target_include_directories(pulp-test-partial-rendering-poc PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-partial-rendering-poc PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-partial-rendering-poc)

add_executable(pulp-test-gpu-graph test_gpu_graph.cpp)
target_include_directories(pulp-test-gpu-graph PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-graph PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-graph)

add_executable(pulp-test-render-loop
    test_render_loop.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/render_loop.cpp)
target_compile_definitions(pulp-test-render-loop PRIVATE
    PULP_RENDER_LOOP_FORCE_TIMER=1)
target_include_directories(pulp-test-render-loop PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include
    ${CMAKE_SOURCE_DIR}/core/render/src)
target_link_libraries(pulp-test-render-loop PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-render-loop
    PROPERTIES
        LABELS coverage)

add_executable(pulp-test-sdl3-surface
    test_sdl3_surface.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/sdl3_surface.cpp)
target_include_directories(pulp-test-sdl3-surface PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-sdl3-surface PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-test-sdl3-surface PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>)
    target_compile_definitions(pulp-test-sdl3-surface PRIVATE PULP_HAS_SDL3=1)
endif()
catch_discover_tests(pulp-test-sdl3-surface
    PROPERTIES
        LABELS coverage)

# GPU-stack tests — pulp::render only exists when PULP_ENABLE_GPU=ON.
# The sanitizer matrix builds with GPU off; without the guard configure
# fails with "Target ... links to: pulp::render but the target was not
# found". The earlier GPU-test block (test_gpu_surface, test_skia_surface,
# test_gpu_compute) already gates on the same condition.
if(PULP_ENABLE_GPU)
    # Rendering integration tests (effects, dimensions, text direction, render passes)
    add_executable(pulp-test-rendering-integration test_rendering_integration.cpp)
    target_link_libraries(pulp-test-rendering-integration PRIVATE pulp::view pulp::render pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-rendering-integration)

endif()

# Texture atlas helpers are header-only; keep this pure test available in
# GPU-off builds so sanitizer and local coverage lanes do not fetch GPU assets.
add_executable(pulp-test-texture-atlas test_texture_atlas.cpp)
target_include_directories(pulp-test-texture-atlas PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-texture-atlas PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-texture-atlas)

# Sprite strip tests
add_executable(pulp-test-sprite-strip test_sprite_strip.cpp)
target_link_libraries(pulp-test-sprite-strip PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-sprite-strip)

# Edit history (global undo) tests
add_executable(pulp-test-edit-history test_edit_history.cpp)
target_link_libraries(pulp-test-edit-history PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-edit-history)

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
add_executable(pulp-test-platform-maturity test_platform_maturity.cpp)
target_link_libraries(pulp-test-platform-maturity PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform-maturity)

# Audio bridge tests
add_executable(pulp-test-audio-bridge test_audio_bridge.cpp)
target_link_libraries(pulp-test-audio-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-bridge)

# Widget tests
add_executable(pulp-test-widgets test_widgets.cpp)
target_link_libraries(pulp-test-widgets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widgets)

# Hot-reload tests
add_executable(pulp-test-hot-reload test_hot_reload.cpp)
target_link_libraries(pulp-test-hot-reload PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow` (#1589): each HotReloader scenario sleeps on file-watcher
# debounce + filesystem mtime resolution (~1-1.5 sec each).
catch_discover_tests(pulp-test-hot-reload
    PROPERTIES
        RESOURCE_LOCK hot-reload-file-watcher
        LABELS slow)

# Inspector tests — only when GPU is enabled (pulp-inspect requires GPU stack).
if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
    add_executable(pulp-test-inspector test_inspector.cpp)
    target_link_libraries(pulp-test-inspector PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector)
endif()

# Widget bridge tests
add_executable(pulp-test-widget-bridge test_widget_bridge.cpp)
target_link_libraries(pulp-test-widget-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge)

# Widget bridge — runtime-import handlers (pulp #468 follow-up).
# Split out of test_widget_bridge.cpp in the Phase 5 P5-1 first cut so
# the 14k-line god-test file shrinks toward per-surface modules.
add_executable(pulp-test-widget-bridge-runtime-import test_widget_bridge_runtime_import.cpp)
target_link_libraries(pulp-test-widget-bridge-runtime-import PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-runtime-import)

# Autonomous Spectr regression suite (pulp-internal task #63, follow-up to
# pulp #1792). Composes the six [contract] invariants from
# test_widget_bridge.cpp into a Spectr-shaped mini-scenario so a future
# PR that keeps each unit-level invariant passing but breaks their
# *interaction* still fails first.
add_executable(pulp-test-spectr-regression test_spectr_regression.cpp)
target_link_libraries(pulp-test-spectr-regression PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-spectr-regression)

# Typography inheritance — pulp #969 (CSS-style cascade)
add_executable(pulp-test-typography-inheritance test_typography_inheritance.cpp)
target_link_libraries(pulp-test-typography-inheritance PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-typography-inheritance)

# Text-overflow ellipsis helper — pulp #1407
add_executable(pulp-test-text-overflow test_text_overflow.cpp)
target_link_libraries(pulp-test-text-overflow PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-overflow)

# Editor bridge tests (pulp #709 — renderer-agnostic envelope/dispatcher)
add_executable(pulp-test-editor-bridge test_editor_bridge.cpp)
target_link_libraries(pulp-test-editor-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-editor-bridge)

# Input events tests
add_executable(pulp-test-input-events test_input_events.cpp)
target_link_libraries(pulp-test-input-events PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-input-events)

# Text editor tests
add_executable(pulp-test-text-editor test_text_editor.cpp)
target_link_libraries(pulp-test-text-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-editor)

# TextEditor multi-line coverage (audit P3: wrap, click-to-caret in
# wrapped rows, caret_rect pixel positioning, single-vs-multi Enter
# contract). Keeps the original test_text_editor.cpp file unchanged
# so the existing single-line surface stays pinned in isolation.
add_executable(pulp-test-text-editor-multiline test_text_editor_multiline.cpp)
target_link_libraries(pulp-test-text-editor-multiline PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-editor-multiline)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
add_executable(pulp-test-text-input test_text_input.cpp)
target_link_libraries(pulp-test-text-input PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-input)

# W3C Layout parity tests (flexbox, box model, visual properties)
add_executable(pulp-test-layout-w3c test_layout_w3c.cpp)
target_link_libraries(pulp-test-layout-w3c PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-layout-w3c)

# Visual semantic snapshot harness. This is a custom-main Catch2 binary:
# --self-test runs its focused tests, while --fixture emits a stable JSON
# snapshot for tools/harness/visual/runner.py.
add_executable(pulp-test-visual visual/pulp-test-visual.cpp)
target_link_libraries(pulp-test-visual PRIVATE pulp::view Catch2::Catch2)
add_test(NAME visual-harness-self-test COMMAND pulp-test-visual --self-test)
set_tests_properties(visual-harness-self-test PROPERTIES
    LABELS "visual;harness;yoga"
    TIMEOUT 30)

# CanvasWidget tests (JS-driven custom drawing)
add_executable(pulp-test-canvas-widget test_canvas_widget.cpp)
target_link_libraries(pulp-test-canvas-widget PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-canvas-widget)

# pulp #964 — Canvas2D JS-shim coverage. Drives the full
# web-compat-canvas.js → bridge → CanvasWidget path so a regression
# that drops a save/restore/setTransform/createLinearGradient method
# (and silently aborts FilterBank-style frame renders) gets caught.
add_executable(pulp-test-canvas2d-shim test_canvas2d_shim.cpp)
target_link_libraries(pulp-test-canvas2d-shim PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-canvas2d-shim)

# SvgPathWidget tests (issue-965)
add_executable(pulp-test-svg-path-widget test_svg_path_widget.cpp)
target_link_libraries(pulp-test-svg-path-widget PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-svg-path-widget)

# SvgRect + SvgLine widget tests (issue-1416)
add_executable(pulp-test-svg-rect-widget test_svg_rect_widget.cpp)
target_link_libraries(pulp-test-svg-rect-widget PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-svg-rect-widget)

# ScrollView tests
add_executable(pulp-test-scroll-view test_scroll_view.cpp)
target_link_libraries(pulp-test-scroll-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-scroll-view)

# ComboBox dropdown interaction tests
add_executable(pulp-test-combo-dropdown test_combo_dropdown.cpp)
target_link_libraries(pulp-test-combo-dropdown PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-combo-dropdown)

# pulp #1148 — generalized overlay-click routing (View::active_overlay_)
add_executable(pulp-test-overlay-routing test_overlay_routing.cpp)
target_link_libraries(pulp-test-overlay-routing PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-overlay-routing)

# pulp #1708 — auto-clearing input-focus slot (View::focused_input_)
add_executable(pulp-test-focused-input test_focused_input.cpp)
target_link_libraries(pulp-test-focused-input PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-focused-input)

# pulp #1148 (slice b) — auto-claim active_overlay_ from CSS shape
# (`position:absolute` + `z-index >= 10`) or `data-overlay` author hint
# detected by the web-compat layer (web-compat-style-decl.js +
# web-compat-element.js). Uses the real WidgetBridge so the heuristic
# runs against the same prelude stack the runtime ships.
add_executable(pulp-test-web-compat-overlay test_web_compat_overlay.cpp)
target_link_libraries(pulp-test-web-compat-overlay PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-web-compat-overlay)

# Panel widget tests (styled containers)
add_executable(pulp-test-panel test_panel.cpp)
target_link_libraries(pulp-test-panel PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-panel)

# UI components tests (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox)
add_executable(pulp-test-ui-components test_ui_components.cpp)
target_link_libraries(pulp-test-ui-components PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ui-components)

# GraphEditorView tests
add_executable(pulp-test-graph-editor-view test_graph_editor_view.cpp)
target_link_libraries(pulp-test-graph-editor-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-graph-editor-view)

# Modal overlay tests
add_executable(pulp-test-modal test_modal.cpp)
target_link_libraries(pulp-test-modal PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-modal)

# Design export tests
add_executable(pulp-test-design-export test_design_export.cpp)
target_link_libraries(pulp-test-design-export PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-export
    PROPERTIES LABELS "parser-import")

# Design import tests (Figma/Stitch/v0/Pencil → Pulp, W3C tokens)
add_executable(pulp-test-design-import test_design_import.cpp)
target_link_libraries(pulp-test-design-import PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import
    PROPERTIES LABELS "parser-import")

# DESIGN.md import (Google design.md, Apache-2.0 — pulp #1434 follow-up)
# Compiles import_detect.cpp directly into the test target so the
# detector tests do not require linking the whole pulp-import-design CLI.
add_executable(pulp-test-design-import-designmd
    test_design_import_designmd.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/import_detect.cpp)
target_include_directories(pulp-test-design-import-designmd PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/import-design
    ${CMAKE_SOURCE_DIR}/tools/cli)
target_link_libraries(pulp-test-design-import-designmd PRIVATE
    pulp::view
    Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-designmd PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-designmd
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Post-parse widget promotion: <div onClick> / role=button / cursor:pointer
# → button. Closure of pulp-internal task #84 / follow-up on #1814.
# Links the importer-side .cpp directly (it's not in pulp::view) so the
# test exercises the same code path the CLI uses.
add_executable(pulp-test-widget-promotion
    test_widget_promotion.cpp
    ${CMAKE_SOURCE_DIR}/tools/import-design/widget_promotion.cpp)
target_include_directories(pulp-test-widget-promotion PRIVATE
    ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-widget-promotion PRIVATE
    pulp::view
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-promotion
    PROPERTIES LABELS "parser-import")

# pulp #468 — web-compat preludes shipped for bundled-React imports
# (nodeType / nodeName, observer no-ops, scheduler shims). Uses
# WidgetBridge to evaluate the same prelude stack the runtime ships.
add_executable(pulp-test-web-compat-react-shims test_web_compat_react_shims.cpp)
target_link_libraries(pulp-test-web-compat-react-shims PRIVATE pulp::view Catch2::Catch2WithMain)
# issue #902 bound-pump test runs the 1M-job cap; budget headroom for slow
# CI runners and sanitizer builds. Default discover timeout is too tight
# when a regression of the bound would hang indefinitely. Other shim
# scenarios in this binary stay in PR fast-CI — they're cheap and
# important — so this binary is *not* labelled `slow` even though one
# of its tests pays a 1-3 sec wall cost.
catch_discover_tests(pulp-test-web-compat-react-shims
    PROPERTIES TIMEOUT 180
)

# pulp #468 — Claude Design bundle envelope parser (base64+gzip JSON
# envelope unpacking, template script-order resolution).
add_executable(pulp-test-design-import-claude-bundle test_design_import_claude_bundle.cpp)
target_link_libraries(pulp-test-design-import-claude-bundle
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-claude-bundle
    PROPERTIES LABELS "parser-import")

# Keyboard-shortcut extraction from React source (UX best-practice
# default — design-import emits a shortcuts manifest the runtime can
# auto-register).
add_executable(pulp-test-design-import-shortcuts test_design_import_shortcuts.cpp)
target_link_libraries(pulp-test-design-import-shortcuts
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-shortcuts
    PROPERTIES LABELS "parser-import")

# pulp #2128 follow-up — design-tool platform key wire-up E2E. Pins the
# `WindowHost → root.on_global_key → bridge.forward_key_event →
# registerShortcut + __dispatch__('__global__','keydown',...)` chain that
# every auto-bound default chord (and Spectr's mode-switch) depends on.
add_executable(pulp-test-platform-key-wireup test_platform_key_wireup.cpp)
target_link_libraries(pulp-test-platform-key-wireup
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform-key-wireup
    PROPERTIES LABELS "parser-import")

# pulp #2128 follow-up — pin WidgetBridge::dispatch_global_key fan-out
# across the static all_bridges_ registry. Every live bridge gets the
# key without any per-app on_global_key wiring; this is the framework
# contract platform hosts depend on.
add_executable(pulp-test-widget-bridge-dispatch-global-key
    test_widget_bridge_dispatch_global_key.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-global-key
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-global-key)

# pulp #2128 follow-up — pin WidgetBridge::dispatch_document_event +
# the real document.addEventListener. Platform hosts fire synthetic
# outside-click events on Esc through this path so React popovers
# (Spectr's PickerDropdown, ContextMenu, etc.) close automatically.
add_executable(pulp-test-widget-bridge-dispatch-document-event
    test_widget_bridge_dispatch_document_event.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-document-event
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-document-event)

# pulp #1035 — `pulp import-design --from claude` classnames.json
# emission. Combines library-level fixture coverage (no binary
# dependency) with a shell-out against the built CLI when available.
add_executable(pulp-test-cli-import-design test_cli_import_design.cpp)
target_link_libraries(pulp-test-cli-import-design
    PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-cli-import-design PRIVATE
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-import-design pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-import-design
    PROPERTIES
        ENVIRONMENT "PULP_REPO_ROOT=${CMAKE_SOURCE_DIR}"
        LABELS "parser-import")

# Direct shell-out coverage for the standalone import-design tool
# executable. These tests hit tools/import-design/pulp_import_design.cpp
# without going through the top-level pulp CLI delegate.
add_executable(pulp-test-import-design-tool test_import_design_tool.cpp)
target_link_libraries(pulp-test-import-design-tool
    PRIVATE pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-import-design-tool PRIVATE
    PULP_IMPORT_DESIGN_TOOL_PATH="$<TARGET_FILE:pulp-import-design>")
add_dependencies(pulp-test-import-design-tool pulp-import-design)
catch_discover_tests(pulp-test-import-design-tool
    PROPERTIES LABELS "parser-import")

# pulp #468 — `--execute-bundle` harness end-to-end. Boots the
# ScriptEngine + WidgetBridge, builds a synthetic React-like bundle on
# the fly, asserts the materialized DOM walker produces an IR deeper
# than the loader-shell baseline. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (Spectr's editor.html).
add_executable(pulp-test-design-import-claude-runtime test_design_import_claude_runtime.cpp)
target_link_libraries(pulp-test-design-import-claude-runtime
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-claude-runtime
    PROPERTIES LABELS "parser-import")

# pulp #758 — inline `<script>` evaluation in the `--execute-bundle`
# harness. Asserts that inline `text/javascript`, inline `text/babel`
# (via a Babel-standalone shim), DOMContentLoaded dispatch, and the
# layered async drain all fire. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (canonical Spectr
# editor.html — should now produce >20 nodes, not the 11-element
# loader shell that pulp #758 was filed to fix).
add_executable(pulp-test-design-import-inline-babel test_design_import_inline_babel.cpp)
target_link_libraries(pulp-test-design-import-inline-babel
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-inline-babel
    PROPERTIES LABELS "parser-import")

# Screenshot comparison tests (visual diff for design import validation)
if(APPLE)
    add_executable(pulp-test-screenshot-compare test_screenshot_compare.cpp)
    target_link_libraries(pulp-test-screenshot-compare PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-compare
        PROPERTIES LABELS "parser-import")
endif()

# Parameter attachment tests
add_executable(pulp-test-param-attachment test_param_attachment.cpp)
target_link_libraries(pulp-test-param-attachment PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-param-attachment)

# App framework tests (KeyMapping, MenuBar, Toolbar, AppSettings)
add_executable(pulp-test-app-framework test_app_framework.cpp)
target_link_libraries(pulp-test-app-framework PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-app-framework)

# Preset manager tests
add_executable(pulp-test-preset-manager test_preset_manager.cpp)
target_link_libraries(pulp-test-preset-manager PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-preset-manager)

# Waveform editor tests
add_executable(pulp-test-waveform-editor test_waveform_editor.cpp)
target_link_libraries(pulp-test-waveform-editor PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-waveform-editor)

# FastMath approximation tests
add_executable(pulp-test-fast-math test_fast_math.cpp)
target_link_libraries(pulp-test-fast-math PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-fast-math)

# Polynomial/Matrix math tests
add_executable(pulp-test-poly-math test_poly_math.cpp)
target_link_libraries(pulp-test-poly-math PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-poly-math)

# Preset browser UI tests
add_executable(pulp-test-preset-browser test_preset_browser.cpp)
target_link_libraries(pulp-test-preset-browser PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-preset-browser)

# Plugin manager panel UI tests (issue #494)
add_executable(pulp-test-plugin-manager-panel test_plugin_manager_panel.cpp)
target_link_libraries(pulp-test-plugin-manager-panel PRIVATE
    pulp::view pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-plugin-manager-panel)

# DSP expansion tests (FIR, Ballistics, LogRamp, ProcessorChain, LookupTable, TPT)
add_executable(pulp-test-dsp-expansion test_dsp_expansion.cpp)
target_link_libraries(pulp-test-dsp-expansion PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsp-expansion)

# Interpolator tests (Hermite, Lagrange, windowed-sinc)
add_executable(pulp-test-interpolator test_interpolator.cpp)
target_link_libraries(pulp-test-interpolator PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-interpolator)

# MIDI expansion tests (RPN/NRPN parser, MidiKeyboardState)
add_executable(pulp-test-midi-expansion test_midi_expansion.cpp)
target_link_libraries(pulp-test-midi-expansion PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-midi-expansion)

# Audio process load measurer tests
add_executable(pulp-test-load-measurer test_load_measurer.cpp)
target_link_libraries(pulp-test-load-measurer PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-load-measurer)

# FilterDesign tests
add_executable(pulp-test-filter-design test_filter_design.cpp)
target_link_libraries(pulp-test-filter-design PRIVATE pulp::signal Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-filter-design)

# TreeView tests
add_executable(pulp-test-tree-view test_tree_view.cpp)
target_link_libraries(pulp-test-tree-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-tree-view)

# File browser, file tree, and multi-document panel tests
add_executable(pulp-test-file-browser test_file_browser.cpp)
target_link_libraries(pulp-test-file-browser PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-file-browser)

# Undo manager tests
add_executable(pulp-test-undo-manager test_undo_manager.cpp)
target_link_libraries(pulp-test-undo-manager PRIVATE pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-undo-manager)

# BufferingReader tests
add_executable(pulp-test-buffering-reader test_buffering_reader.cpp)
target_link_libraries(pulp-test-buffering-reader PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-buffering-reader)

# AudioWorkgroup tests
add_executable(pulp-test-workgroup test_workgroup.cpp)
target_link_libraries(pulp-test-workgroup PRIVATE pulp::audio Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-workgroup)

# Plugin matrix tests (sample rate/buffer size/edge case sweep)
add_executable(pulp-test-matrix test_plugin_matrix.cpp)
target_link_libraries(pulp-test-matrix PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-matrix)

# Per-plugin matrix tests (separate TUs to avoid enum collisions)
add_executable(pulp-test-matrix-effect test_matrix_effect.cpp)
target_link_libraries(pulp-test-matrix-effect PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-matrix-effect)

add_executable(pulp-test-matrix-compressor test_matrix_compressor.cpp)
target_link_libraries(pulp-test-matrix-compressor PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-compressor PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-compressor)
catch_discover_tests(pulp-test-matrix-compressor)

add_executable(pulp-test-matrix-tone test_matrix_tone.cpp)
target_link_libraries(pulp-test-matrix-tone PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-tone PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-matrix-tone)

add_executable(pulp-test-matrix-drums test_matrix_drums.cpp)
target_link_libraries(pulp-test-matrix-drums PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-drums PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpDrums)
catch_discover_tests(pulp-test-matrix-drums)

add_executable(pulp-test-matrix-synth test_matrix_synth.cpp)
target_link_libraries(pulp-test-matrix-synth PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-synth PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSynth)
catch_discover_tests(pulp-test-matrix-synth)

add_executable(pulp-test-matrix-sampler test_matrix_sampler.cpp)
target_link_libraries(pulp-test-matrix-sampler PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-sampler PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSampler)
catch_discover_tests(pulp-test-matrix-sampler)

# Golden-file audio tests (uses HeadlessHost + PulpGain + PulpTone)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
    ${CMAKE_SOURCE_DIR}/examples/pulp-tone
)
catch_discover_tests(pulp-test-golden)

# Audio determinism matrix (#48 / #356): SR × block-size sweep
# asserting framework-layer invariants (unity pass-through, silence,
# block-size invariance, re-prepare cleanliness) across the examples
# that are known SR/block-independent today.
add_executable(pulp-test-audio-matrix test_audio_determinism_matrix.cpp)
target_link_libraries(pulp-test-audio-matrix PRIVATE
    pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-matrix PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
    ${CMAKE_SOURCE_DIR}/examples/pulp-tone
)
catch_discover_tests(pulp-test-audio-matrix)

# Audio edge-case corpus (#49 / #356): zero-length blocks, denormals,
# impulse position, DC extremes, param change between blocks,
# re-prepare idempotency. Framework-layer contract only.
add_executable(pulp-test-audio-edge-cases test_audio_edge_cases.cpp)
target_link_libraries(pulp-test-audio-edge-cases PRIVATE
    pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-edge-cases PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
)
catch_discover_tests(pulp-test-audio-edge-cases)

# Golden-file tests for PulpEffect (separate to avoid enum collision with PulpGain)
add_executable(pulp-test-golden-effect test_golden_effect.cpp)
target_link_libraries(pulp-test-golden-effect PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-golden-effect)

# Negative-path tests (NaN, Inf, extreme sample rates, edge cases)
add_executable(pulp-test-negative-path test_negative_path.cpp)
target_link_libraries(pulp-test-negative-path PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-negative-path PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/pulp-gain
    ${CMAKE_SOURCE_DIR}/examples/pulp-tone
)
catch_discover_tests(pulp-test-negative-path)

# iOS foundation tests (platform detection, safe area geometry, touch events)
add_executable(pulp-test-ios-foundation test_ios_foundation.cpp)
target_link_libraries(pulp-test-ios-foundation PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ios-foundation)

# Identity/UUID tests
add_executable(pulp-test-identity test_identity.cpp)
target_link_libraries(pulp-test-identity PRIVATE pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-identity)

# WebView tests (requires PULP_BUILD_WEBVIEW — WebViewPanel::create is only compiled when ON)
if(PULP_BUILD_WEBVIEW)
    add_executable(pulp-test-webview test_web_view.cpp)
    target_link_libraries(pulp-test-webview PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-webview)
endif()

# MIDI 2.0 UMP/MPE tests
add_executable(pulp-test-ump-mpe test_ump_mpe.cpp)
target_link_libraries(pulp-test-ump-mpe PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ump-mpe)

# MPE voice tracker tests
add_executable(pulp-test-mpe-voice-tracker test_mpe_voice_tracker.cpp)
target_link_libraries(pulp-test-mpe-voice-tracker PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-voice-tracker)

# UMP buffer + MIDI 1.0 <-> UMP conversion + UMP-aware MpeVoiceTracker
add_executable(pulp-test-ump-buffer-conversion test_ump_buffer_conversion.cpp)
target_link_libraries(pulp-test-ump-buffer-conversion PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ump-buffer-conversion)

# MPE buffer + Processor sidecar tests
add_executable(pulp-test-mpe-buffer test_mpe_buffer.cpp)
target_link_libraries(pulp-test-mpe-buffer PRIVATE pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-buffer)

# MPE synth voice helpers (voice, allocator, glide detector)
add_executable(pulp-test-mpe-synth-voice test_mpe_synth_voice.cpp)
target_link_libraries(pulp-test-mpe-synth-voice PRIVATE pulp::midi Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-mpe-synth-voice)

# Plugin hosting tests (scanner, signal graph)
add_executable(pulp-test-host test_host.cpp)
target_link_libraries(pulp-test-host PRIVATE pulp::host Catch2::Catch2WithMain)
# Point the integration test at the locally-built PulpGain.clap so the CLAP
# loader runs against a known real plugin.
if(TARGET PulpGain_CLAP)
    target_compile_definitions(pulp-test-host PRIVATE
        PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
    add_dependencies(pulp-test-host PulpGain_CLAP)
endif()
catch_discover_tests(pulp-test-host)

# Host regression coverage (#52) — end-to-end scan / load / process /
# unload lifecycle, scanner failure modes (moduleinfo.json + manifest.ttl),
# state round-trip, and hot-reload race-cleanliness. Separate binary so a
# failing regression case fingers the guilty surface without clouding
# pulp-test-host's scope.
add_executable(pulp-test-host-regression test_host_regression.cpp)
target_link_libraries(pulp-test-host-regression
    PRIVATE pulp::host Catch2::Catch2WithMain)
if(TARGET PulpGain_CLAP)
    target_compile_definitions(pulp-test-host-regression PRIVATE
        PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
    add_dependencies(pulp-test-host-regression PulpGain_CLAP)
endif()
catch_discover_tests(pulp-test-host-regression)

# Stress test for the SignalGraph hot-reload race (pulp#669). Runs the
# specific Catch2 case 100 times in a row via a shell wrapper, exiting
# at the first failure. Gated behind PULP_STRESS_FLAKY_TESTS so per-PR
# CI doesn't pay the ~5s cost; nightly cron / on-demand runs flip the
# flag on. Repeated failures under stress would prove the race is real
# (vs once-in-50-runs Namespace timing-noise) and unblock a
# deterministic-repro fix.
#
# Why a wrapper instead of CTest's REPEAT property: REPEAT only takes
# effect when ctest is invoked with `--repeat until-fail:N` on the
# command line — a property alone is silently a no-op. A shell loop
# inside the test command guarantees repetition without depending on
# how the caller invokes ctest.
if(DEFINED ENV{PULP_STRESS_FLAKY_TESTS} AND NOT "$ENV{PULP_STRESS_FLAKY_TESTS}" STREQUAL "")
    if(WIN32)
        # PowerShell branch already shows full stdout; that's intentional —
        # the failing iteration's Catch2 assertion context + RNG seed need
        # to land in the ctest log so the failure is replayable. Per-
        # success noise is the cost.
        add_test(NAME pulp-test-signal-graph-hot-reload-stress
            COMMAND powershell -Command
                "for ($i = 1; $i -le 100; $i++) { & '$<TARGET_FILE:pulp-test-host-regression>' 'SignalGraph hot-reload mid-audio is race-free via snapshot publish' --rng-seed=time; if ($LASTEXITCODE -ne 0) { Write-Host \"stress failed on iteration $i\"; exit 1 } } Write-Host 'stress ok (100/100)'")
    else()
        # POSIX branch buffers each iteration's stdout into a temp file
        # and only prints it on failure. Codex P2 review on PR #674
        # caught the original `>/dev/null` shape that discarded the
        # failure diagnostic — the whole point of the stress harness is
        # to capture a replayable seed when an iteration fails, so
        # silently dropping Catch2's assertion context defeats it.
        # On success, the per-iteration buffer is overwritten on the
        # next loop; only the final iteration's (or first failure's)
        # contents survive in /tmp, with the loop's own "stress ok"
        # / "stress failed on iteration N" line as the visible signal.
        add_test(NAME pulp-test-signal-graph-hot-reload-stress
            COMMAND bash -c
                "set -e; out=$(mktemp); trap 'rm -f \"$out\"' EXIT; for i in $(seq 1 100); do if ! '$<TARGET_FILE:pulp-test-host-regression>' 'SignalGraph hot-reload mid-audio is race-free via snapshot publish' --rng-seed=time >\"$out\" 2>&1; then echo \"stress failed on iteration $i — Catch2 output:\"; cat \"$out\"; exit 1; fi; done; echo 'stress ok (100/100)'")
    endif()
    set_tests_properties(pulp-test-signal-graph-hot-reload-stress PROPERTIES
        LABELS "host;graph;hot-reload;race;stress;issue-669"
        TIMEOUT 600)
endif()

# GraphSerializer round-trip tests
add_executable(pulp-test-graph-serializer test_graph_serializer.cpp)
target_link_libraries(pulp-test-graph-serializer PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-graph-serializer)

# NetworkServiceDiscovery backend-dispatch tests (#302)
add_executable(pulp-test-network-service-discovery test_network_service_discovery.cpp)
target_link_libraries(pulp-test-network-service-discovery PRIVATE pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-network-service-discovery)

# WAMv2 + WebCLAP format adapter tests
add_executable(pulp-test-wam-wclap test_wam_wclap.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/wam_adapter.cpp
)
target_link_libraries(pulp-test-wam-wclap PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-wam-wclap PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-wam-wclap)

# CLAP webview extension tests
add_executable(pulp-test-clap-webview test_clap_webview.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/clap_webview.cpp
)
target_link_libraries(pulp-test-clap-webview PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-clap-webview PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-clap-webview)

# Web demo plugin tests (PulpPluck, PulpChorus)
add_executable(pulp-test-web-demos test_web_demos.cpp)
target_link_libraries(pulp-test-web-demos PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-demos PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/web-demos
)
catch_discover_tests(pulp-test-web-demos)

# AI Designer: design tool layout/parity tests
add_executable(pulp-test-design-tool-layout test_design_tool_layout.cpp)
target_link_libraries(pulp-test-design-tool-layout PRIVATE pulp::view pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-tool-layout)

# AI Designer: style pack tests
add_executable(pulp-test-style-pack test_style_pack.cpp)
target_link_libraries(pulp-test-style-pack PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-style-pack)

# Removed: pulp-test-token-diff (test_token_diff.cpp) and pulp-test-showcase
# (test_showcase.cpp) were placeholder TEST_CASE("...") { REQUIRE(true); }
# stubs for design-tool C++ classes that never shipped. The design tool
# landed as JS via core/view/js/web-compat* and widget_bridge's
# applyTokenDiff JS binding; there is no C++ surface to verify here.
# Removed in PR #290/#357 follow-up; file a new focused test if a C++
# token-diff or Showcase class is ever added.

# WindowManager multi-window framework tests (Phase 6)
add_executable(pulp-test-window-manager test_window_manager.cpp)
target_link_libraries(pulp-test-window-manager PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-window-manager)

# ── Phase 9: Theme management, widgets, asset system ────────────────────────

# Theme contrast utilities (WCAG AA)
add_executable(pulp-test-theme-contrast test_theme_contrast.cpp)
target_link_libraries(pulp-test-theme-contrast PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme-contrast)

# Theme preset library (38 tweakcn + derivation)
add_executable(pulp-test-theme-presets test_theme_presets.cpp)
target_link_libraries(pulp-test-theme-presets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-theme-presets)

# OS appearance tracking
add_executable(pulp-test-appearance test_appearance_tracker.cpp)
target_link_libraries(pulp-test-appearance PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-appearance)

# Splash screen lifecycle and paint behavior
add_executable(pulp-test-splash-screen test_splash_screen.cpp)
target_link_libraries(pulp-test-splash-screen PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-splash-screen)

# New widgets (EqCurve, MidiKeyboard, ColorPicker, FileDropZone, SplitView, PropertyList, Breadcrumb)
add_executable(pulp-test-phase9-widgets test_phase9_widgets.cpp)
target_link_libraries(pulp-test-phase9-widgets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-phase9-widgets)

add_executable(pulp-test-property-list test_property_list.cpp)
target_link_libraries(pulp-test-property-list PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-property-list)

# Focused SplitView and ConcertinaPanel coverage
add_executable(pulp-test-view-layout-widgets test_view_layout_widgets.cpp)
target_link_libraries(pulp-test-view-layout-widgets PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-layout-widgets)

# Asset manager and resource system
add_executable(pulp-test-asset-manager test_asset_manager.cpp)
target_link_libraries(pulp-test-asset-manager PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-asset-manager)

# Web-compat test suite (Phase 6: CSS parsing, layout, events, visual regression)
add_subdirectory(web-compat)

# ── Zero-copy benchmark (Slice 0 of #516) ──────────────────────────────────
#
# Tooling-only tests, gated on PULP_BENCHMARK. The perf-counter unit
# test links pulp::render, which is only added to the build when
# PULP_ENABLE_GPU=ON (see root CMakeLists.txt). Gating the test on the
# render target's presence lets -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF
# configure cleanly without a missing-target error (Codex P2 on PR #526
# / #500). The integration test shell-outs to pulp-ui-preview which is
# currently Apple-desktop-only (same guard as the target).
if(PULP_BENCHMARK AND TARGET pulp-render)
    add_executable(pulp-test-bench-perf-counters test_bench_perf_counters.cpp)
    target_link_libraries(pulp-test-bench-perf-counters PRIVATE
        pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-bench-perf-counters)

    if(APPLE AND NOT PULP_IOS)
        add_executable(pulp-test-bench-integration test_bench_integration.cpp)
        target_link_libraries(pulp-test-bench-integration PRIVATE
            pulp::platform Catch2::Catch2WithMain)
        if(TARGET pulp-ui-preview)
            add_dependencies(pulp-test-bench-integration pulp-ui-preview)
        endif()
        catch_discover_tests(pulp-test-bench-integration)
    endif()
endif()

# ── Mac platform-test harness (issue #2001) ────────────────────────────────
#
# Catch2 fixture that brings up a hidden NSWindow + CAMetalLayer host
# WITHOUT calling makeKeyAndOrderFront / activateIgnoringOtherApps, so
# tests can exercise window_host_mac.mm code paths (set_design_viewport,
# paint_scene, mouse-coordinate inverse mapping, resize → contentsScale)
# that today are structurally untestable from plain Catch2.
#
# Phase B-1 (this PR): ships the harness scaffold + a smoke test proving
#   the hidden window / capture_back_buffer_png contract.
# Phase B-2 (next PR): migrates one PR-#1984 invariant as the first real
#   consumer, and adds simulate_mouse exercise.
#
# Plan: planning/2026-05-14-mac-platform-test-harness.md (planning sub).
if(APPLE AND NOT PULP_IOS AND PULP_HAS_SKIA)
    add_executable(pulp-test-mac-platform-harness
        test_mac_platform_harness.cpp
        mac_window_harness.mm
    )
    target_link_libraries(pulp-test-mac-platform-harness PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
        "-framework Metal"
        "-framework QuartzCore"
    )
    catch_discover_tests(pulp-test-mac-platform-harness)

    # pulp #2088 — pin the invariant -[PulpView liveFocusedView] depends on:
    # ~View() must auto-clear focused_input_ (#1708 contract) so the
    # accessor can safely re-sync the ivar to nullptr before any deref.
    add_executable(pulp-test-mac-mousedown-stale-focus
        test_mac_mousedown_stale_focus.mm
    )
    target_link_libraries(pulp-test-mac-mousedown-stale-focus PRIVATE
        pulp::view
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-mac-mousedown-stale-focus)

    # pulp #2128 follow-up — pin the macOS performKeyEquivalent: route that
    # delivers Cmd-modified chords into rootView->on_global_key. Without
    # the override, every auto-bound default chord (Cmd+, Cmd+? Cmd+S) is
    # silently consumed by NSResponder's first-responder chain.
    add_executable(pulp-test-mac-perform-key-equivalent
        test_mac_perform_key_equivalent.mm
    )
    target_link_libraries(pulp-test-mac-perform-key-equivalent PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
    )
    # PulpView's Obj-C class metadata only registers when the linker keeps
    # window_host_mac.mm.o — but no C++ symbol the test references lives in
    # that .o, so static-lib pruning drops it. Force-load the one specific
    # object to register the class without dragging in every other .mm
    # (which would duplicate-symbol on PulpAccessibilityElement).
    target_link_options(pulp-test-mac-perform-key-equivalent PRIVATE
        "LINKER:-force_load,${CMAKE_BINARY_DIR}/core/view/CMakeFiles/pulp-view.dir/platform/mac/window_host_mac.mm.o"
    )
    catch_discover_tests(pulp-test-mac-perform-key-equivalent)
endif()
