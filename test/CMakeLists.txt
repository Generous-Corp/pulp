# Pulp test suite — Catch2

include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpTestSuite.cmake)
find_package(Python3 COMPONENTS Interpreter QUIET)

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

# Install-layout regression for format helper sources used by
# PulpAuv3.cmake / PulpPluginFormats.cmake from _PULP_FORMAT_SOURCE_DIR.
# Without au_view_controller_mac.mm in the installed SDK, downstream
# pulp_add_plugin(FORMATS AUv3 ...) builds on macOS fail with a
# missing-source error. Tracked by the Linux/macOS Chainer gap-closure
# plan (planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md,
# Phase 0/3).
add_test(NAME cmake-pulp-install-format-sources
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_install_format_sources.cmake)
set_tests_properties(cmake-pulp-install-format-sources PROPERTIES
    LABELS "cmake;sdk;auv3;chainer-cross;slow"
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
    set_tests_properties(pulp-mcp-launcher PROPERTIES TIMEOUT 45)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME pulp-mcp-binary-smoke
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pulp_mcp_binary_smoke.py
                $<TARGET_FILE:pulp-mcp>)
    set_tests_properties(pulp-mcp-binary-smoke PROPERTIES TIMEOUT 90)
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

# P5-4 — `pulp loop` shell-out tests, extracted from test_cli_shellout.cpp
# to bring the parent TU below the 2k line target. Shares
# test_cli_shellout_helpers.hpp with the parent and its sibling
# test_cli_shellout_scan_projects.cpp.
add_executable(pulp-test-cli-shellout-loop test_cli_shellout_loop.cpp)
target_link_libraries(pulp-test-cli-shellout-loop PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-shellout-loop pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-shellout-loop)

# P5-4 — `pulp scan` + `pulp projects list` shell-out tests, extracted
# from test_cli_shellout.cpp. Shares the helpers header with its
# siblings.
add_executable(pulp-test-cli-shellout-scan-projects test_cli_shellout_scan_projects.cpp)
target_link_libraries(pulp-test-cli-shellout-scan-projects PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-shellout-scan-projects pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-shellout-scan-projects)

# P5-4 follow-up — `pulp pr` shell-out tests, extracted from
# test_cli_shellout.cpp. Two contiguous clusters (pr.workflow validation
# + Shipyard binary pin handling + native fallback). Shares the helpers
# header with its siblings.
add_executable(pulp-test-cli-shellout-pr test_cli_shellout_pr.cpp)
target_link_libraries(pulp-test-cli-shellout-pr PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-shellout-pr pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-shellout-pr)

# P5-4 follow-up — CLI lifecycle-command shell-out tests, extracted
# from test_cli_shellout.cpp. Covers pulp doctor / pulp dev /
# pulp design / pulp sdk / pulp upgrade — the lifecycle / diagnostics
# surface of the CLI. Brings the parent TU under the < 2,000-line
# P5-4 target.
add_executable(pulp-test-cli-shellout-lifecycle test_cli_shellout_lifecycle.cpp)
target_link_libraries(pulp-test-cli-shellout-lifecycle PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-shellout-lifecycle pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-shellout-lifecycle)

# Inspector Phase 2 — `pulp tweaks diff` shell-out tests. Drives the
# built binary against pulp-tweaks.json + design-snapshot fixtures and
# pins the clean / drifted / orphaned exit-code contract.
add_executable(pulp-test-cli-tweaks-shellout test_cli_tweaks_shellout.cpp)
target_link_libraries(pulp-test-cli-tweaks-shellout PRIVATE pulp::platform Catch2::Catch2WithMain)
if(TARGET pulp-cli)
    add_dependencies(pulp-test-cli-tweaks-shellout pulp-cli)
endif()
catch_discover_tests(pulp-test-cli-tweaks-shellout)

# pulp #776 — pin shell_quote's per-platform contract so it can't
# silently regress to the pre-fix shape that broke `git fetch origin`
# on Windows by writing doubled backslashes into clone URLs.
add_executable(pulp-test-cli-shell-quote
    test_cli_shell_quote.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp
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

add_executable(pulp-test-cli-docs-command
    test_cli_docs_command.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cmd_docs.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp
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

# Item 7.4b (macos-plugin-authoring-plan): `pulp build --install`
# validation gate. The test compiles install_paths_mac.cpp directly
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
catch_discover_tests(pulp-test-cli-install-paths-mac)

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

# Phase 5 mac runtime validators (chainer plan
# planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md):
# hermetic tests for `pulp validate --target {standalone|auv3|macho}`
# that stub the shell-out and filesystem probes via MacValidatorEnv,
# so the tests run on any host (including Linux CI legs) without
# needing real .app / .appex / auval.
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
# install branches; links the package_commands_* TUs + package_registry.cpp
# directly so the command surface can be exercised without shelling out
# to the built binary. Roadmap item P11-2 split the sub-command bodies
# out of package_commands.cpp into _util / _search / _add sibling TUs.
add_executable(pulp-test-cli-package-commands
    test_cli_package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_util.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_search.cpp
    ${CMAKE_SOURCE_DIR}/tools/cli/package_commands_add.cpp
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
pulp_add_test_suite(pulp-test-child-process LIBRARIES pulp::platform)

# Progress parser tests
pulp_add_test_suite(pulp-test-progress-parser LIBRARIES pulp::platform)

# Stream tests (unified I/O interface)
pulp_add_test_suite(pulp-test-stream LIBRARIES pulp::runtime)

# #334: AudioFocusRegistry — cross-platform observer for OS audio-focus
# signals (Android AudioManager.OnAudioFocusChangeListener,
# AVAudioSession interruptions). Pure C++ unit tests — no platform deps.
pulp_add_test_suite(pulp-test-audio-focus LIBRARIES pulp::audio)

# #244: zero-fill helper for the Oboe input-frame-read short-read path.
# Header-only math, so no library link required beyond Catch2.
pulp_add_test_suite(pulp-test-frame-fill LIBRARIES pulp::audio)

# #86: SysExAccumulator — cross-platform MIDI SysEx aggregation state
# machine shared by Android/ALSA/Windows/iOS MIDI backends. Header-only.
pulp_add_test_suite(pulp-test-sysex-accumulator LIBRARIES pulp::midi)

# macOS plan 8.2: UmpSysex7Reassembler — shared UMP type-0x3 reassembler
# extracted from the inline AUv3 / CoreMIDI state machines. Header-only.
pulp_add_test_suite(pulp-test-ump-sysex7-reassembler LIBRARIES pulp::midi)

# macOS plan 8.1: UmpSession + UmpEndpoint + VirtualUmpEndpoint —
# headless tests for the cross-platform session + virtual-endpoint
# registry. The macOS .mm path is exercised opportunistically; the
# suite runs on every platform via the virtual-endpoint half.
pulp_add_test_suite(pulp-test-ump-session LIBRARIES pulp::midi)

# AsyncStream tests (Phase 2)
pulp_add_test_suite(pulp-test-async-stream LIBRARIES pulp::runtime)

# Network Stream tests (Phase 3)
pulp_add_test_suite(pulp-test-network-stream
    LIBRARIES pulp::runtime
    PROPERTIES RESOURCE_LOCK network-stream)

# MessageChannel tests (Phase 4)
pulp_add_test_suite(pulp-test-memory-message-channel LIBRARIES pulp::runtime)

pulp_add_test_suite(pulp-test-websocket-channel LIBRARIES pulp::runtime)

pulp_add_test_suite(pulp-test-osc-channel LIBRARIES pulp::osc)

pulp_add_test_suite(pulp-test-json-rpc LIBRARIES pulp::runtime)

# Validation harness tests (Phase 2)
pulp_add_test_suite(pulp-test-validation-harness LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-plugin-state-io LIBRARIES pulp::format)

# AAX metadata/model tests
pulp_add_test_suite(pulp-test-aax-model LIBRARIES pulp::format)

# Appcast/auto-update tests
pulp_add_test_suite(pulp-test-appcast LIBRARIES pulp::ship)

# Sparkle Ed25519 sign/verify (macOS plan item 7.3)
pulp_add_test_suite(pulp-test-sparkle-verify LIBRARIES pulp::ship pulp::runtime)

# Android package/signing tests (host-side fake SDK/toolchain only)
pulp_add_test_suite(pulp-test-android-package LIBRARIES pulp::ship)

# V3 gaps tests (Bias, MidiSequence, SubsectionReader, HostType, Primes, Expression)
pulp_add_test_suite(pulp-test-v3-gaps LIBRARIES pulp::signal pulp::midi pulp::audio pulp::format pulp::runtime)

# Codesign tests
pulp_add_test_suite(pulp-test-codesign LIBRARIES pulp::ship)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(pulp-test-linux-packaging test_linux_packaging.cpp)
    target_link_libraries(pulp-test-linux-packaging PRIVATE
        pulp::ship
        pulp::platform
        Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-linux-packaging)
endif()

# Sync primitives tests (SeqLock, TripleBuffer)
add_executable(pulp-test-sync test_sync_primitives.cpp)
target_link_libraries(pulp-test-sync PRIVATE pulp::runtime Catch2::Catch2WithMain)
# `slow` (#1589): hammer tests run a writer + readers for 500 ms each
# to surface ordering bugs. Sanitizers.yml exercises the same code
# under TSan; PR fast-CI can skip the redundant smoke.
catch_discover_tests(pulp-test-sync PROPERTIES LABELS slow)

# License and BigInteger tests
pulp_add_test_suite(pulp-test-license LIBRARIES pulp::runtime)

# License v1 -> v2 migration (macOS plan item 7.2)
pulp_add_test_suite(pulp-test-license-migration LIBRARIES pulp::runtime)

# MIDI CI tests
pulp_add_test_suite(pulp-test-midi-ci LIBRARIES pulp::midi)

# MIDI CI Property Exchange (PE) tests — issue-84, plan item 8.4
pulp_add_test_suite(pulp-test-midi-ci-pe LIBRARIES pulp::midi)

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

    # AU v2 Cocoa editor-view advertisement (kAudioUnitProperty_CocoaUI). Compiles
    # the Cocoa view module with PULP_AU_GUI + a test-unique factory class name and
    # asserts the cross-TU filler registers + advertises the right, resolvable class.
    add_executable(pulp-test-au-v2-cocoa-ui
        test_au_v2_cocoa_ui.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm)
    target_link_libraries(pulp-test-au-v2-cocoa-ui PRIVATE
        pulp::format pulp::view Catch2::Catch2WithMain
        "-framework AppKit" "-framework AudioToolbox" "-framework QuartzCore")
    target_compile_definitions(pulp-test-au-v2-cocoa-ui PRIVATE
        PULP_AU_GUI=1 PULP_AU_COCOA_VIEW_CLASS=PulpAUCocoaViewFactory_Test)
    set_target_properties(pulp-test-au-v2-cocoa-ui PROPERTIES CXX_STANDARD 23)
    # AudioUnitSDK 1.4 headers use std::expected (C++23). The root sets
    # CMAKE_CXX_STANDARD=20 which wins for ObjC++ over the target CXX_STANDARD,
    # so force C++23 explicitly on the OBJCXX sources (both .mm here).
    target_compile_options(pulp-test-au-v2-cocoa-ui PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
    catch_discover_tests(pulp-test-au-v2-cocoa-ui)
endif()

# iOS AVAudioSession bridge (workstream 05 slice 5.2)
pulp_add_test_suite(pulp-test-ios-audio-session LIBRARIES pulp::format)
if(NOT PULP_IOS)
    add_executable(pulp-test-pulp-bridge test_pulp_bridge.cpp)
    target_link_libraries(pulp-test-pulp-bridge PRIVATE
        pulp::format
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-pulp-bridge)
endif()
# ARA scaffolding (workstream 06 slice 6.1)
pulp_add_test_suite(pulp-test-ara LIBRARIES pulp::format)
# CLAP ARA extension end-to-end (workstream 06 A2a)
pulp_add_test_suite(pulp-test-clap-ara-extension LIBRARIES pulp::format)
# Processor ARA hook (workstream 06 slice 6.3a)
pulp_add_test_suite(pulp-test-processor-ara-hook LIBRARIES pulp::format)
# Processor bus-layout validation + processBlock precision contract +
# latency / tail change notification (workstream 03 items 3.7, 3.8, 3.11).
pulp_add_test_suite(pulp-test-processor-layout-latency LIBRARIES pulp::format)
# Processor and descriptor default contracts
pulp_add_test_suite(pulp-test-processor-defaults LIBRARIES pulp::format)
# Item 1.3 AudioPlayHead transport-extension helpers used by every
# format adapter (VST3, AU v2 / v3, CLAP) to derive `ProcessContext::bar`
# and the three change flags (tempo_changed / time_sig_changed /
# transport_changed) once per block.
pulp_add_test_suite(pulp-test-playhead-diff LIBRARIES pulp::format)
# VST3 SMPTE → FrameRate mapping (pure helper, no Steinberg SDK
# dependency). Pins the #2963 59.94 vs 60 regression among others.
pulp_add_test_suite(pulp-test-vst3-frame-rate LIBRARIES pulp::format)
# `pulp ship auv3-xcodeproj` developer-path validation helper. Pins the
# #2969 Xcode-beta regression. Header-only; no extra libraries needed.
add_executable(pulp-test-xcode-developer-path
    test_xcode_developer_path.cpp
)
target_include_directories(pulp-test-xcode-developer-path PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/cli
)
target_link_libraries(pulp-test-xcode-developer-path PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-xcode-developer-path)
# TableModel data/sort layer (workstream 07 slice 7.3)
pulp_add_test_suite(pulp-test-table-model LIBRARIES pulp::view)
# ModulationMatrix data model (workstream 07 slice 7.1)
pulp_add_test_suite(pulp-test-modulation-matrix LIBRARIES pulp::view)
# ModulationMatrixWidget canvas widget (workstream 07 slice B5)
pulp_add_test_suite(pulp-test-modulation-matrix-widget LIBRARIES pulp::view)
# Lasso selection overlay
pulp_add_test_suite(pulp-test-lasso LIBRARIES pulp::view)
# Split pane layout widget
pulp_add_test_suite(pulp-test-split-view LIBRARIES pulp::view)
# CommandRegistry + KeyMappingEditor (Pulp-native names — planning slice 6.4)
pulp_add_test_suite(pulp-test-command-registry LIBRARIES pulp::view pulp::state)
# PluginDescriptor vendor_url + vendor_email (workstream 01 slice 1.8)
pulp_add_test_suite(pulp-test-vendor-url LIBRARIES pulp::format)
# Image cache (workstream 07 slice 7.4)
pulp_add_test_suite(pulp-test-image-cache LIBRARIES pulp::view)
# ImageView ↔ ImageCache wiring (workstream 07 B4)
pulp_add_test_suite(pulp-test-image-view-cache LIBRARIES pulp::view)

# Accessibility tree snapshot (workstream 04 slice 4.4)
pulp_add_test_suite(pulp-test-accessibility-tree LIBRARIES pulp::view)
# Text-accessibility scaffold (font v2 Slice 2.6)
pulp_add_test_suite(pulp-test-text-accessibility LIBRARIES pulp::view)
# macOS NSAccessibility backend for TextAccessibilityNode (font v2 Slice 2.6 macOS)
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-text-accessibility-macos test_text_accessibility_macos.mm)
    target_link_libraries(pulp-test-text-accessibility-macos
        PRIVATE pulp::view Catch2::Catch2WithMain "-framework AppKit")
    catch_discover_tests(pulp-test-text-accessibility-macos)
endif()
# Windows UIA backend (font v2 Slice 2.6) — compile-gated on _WIN32 in the
# source. The sentinel test case keeps the binary present + named
# consistently on non-Windows hosts so ctest output stays stable.
pulp_add_test_suite(pulp-test-text-accessibility-windows LIBRARIES pulp::view)
# Linux AccessKit backend (font v2 Slice 2.6) — compile-gated on
# __linux__ && !__ANDROID__; sentinel on other hosts.
pulp_add_test_suite(pulp-test-text-accessibility-linux LIBRARIES pulp::view)
# A/B compare (workstream 07 slice 7.2)
pulp_add_test_suite(pulp-test-ab-compare LIBRARIES pulp::view)
# ViewSize::aspect_ratio field (workstream 07 slice 7.5b)
pulp_add_test_suite(pulp-test-view-size-aspect LIBRARIES pulp::format)

# pulp #59/#63/#64/#65 — WindowHost::set_design_viewport pure-math
# coverage. Locks down the scale + letterbox transform that the mac
# GPU host (and any other future platform host) uses to fit
# fixed-design content into the current window.
pulp_add_test_suite(pulp-test-view-design-viewport LIBRARIES pulp::view)

# PluginViewHost design-viewport wiring (mac CPU + GPU). The pure
# math is already pinned by `pulp-test-view-design-viewport`; this
# target covers the new PluginViewHost virtuals (set_design_viewport,
# set_fixed_aspect_ratio, window_to_root_point) and that the mac CPU
# host inverse-maps mouse coords before hit_test. GPU section soft-
# skips without Dawn / a window server. Foundation for CLAP
# gui_get_resize_hints + VST3 checkSizeConstraint resize negotiation.
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-plugin-view-host-design-viewport
        test_plugin_view_host_design_viewport.mm)
    target_link_libraries(pulp-test-plugin-view-host-design-viewport PRIVATE
        pulp::view pulp::canvas Catch2::Catch2WithMain "-framework AppKit")
    if(PULP_HAS_SKIA)
        target_link_libraries(pulp-test-plugin-view-host-design-viewport
            PRIVATE pulp::render skia::skia)
        target_compile_definitions(pulp-test-plugin-view-host-design-viewport
            PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-plugin-view-host-design-viewport
            PRIVATE ${SKIA_INCLUDE_DIRS})
    endif()
    catch_discover_tests(pulp-test-plugin-view-host-design-viewport)
endif()

# pulp-internal #70 — design-tool viewport resolver probe-layout
# pattern. Locks in the yoga-measured-height path that the design-tool
# example uses to auto-size its window for grid + row trees where
# View::intrinsic_height returns 0.
pulp_add_test_suite(pulp-test-design-tool-viewport-resolver LIBRARIES pulp::view)

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
# Crash-isolated scanner (macOS plan item 4.1). Skipped on iOS/Android
# where pulp-scan-worker isn't built (sandboxed platforms can't fork).
if(NOT IOS AND NOT ANDROID)
    add_executable(pulp-isolated-scanner-crash-helper
        fixtures/isolated_scanner_crash_helper.cpp)
    set_target_properties(pulp-isolated-scanner-crash-helper PROPERTIES
        OUTPUT_NAME "pulp-isolated-scanner-crash-helper")

    add_executable(pulp-test-isolated-scanner test_isolated_scanner.cpp)
    # pulp::platform comes transitively via pulp::host (it's a PRIVATE
    # dep there, but the static-archive link still pulls in the symbols
    # the test needs through pulp::host's USAGE side). We only have to
    # list Catch2 + the public dep.
    target_link_libraries(pulp-test-isolated-scanner PRIVATE
        pulp::host
        Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-isolated-scanner PRIVATE
        PULP_ISOLATED_SCANNER_REAL_WORKER="$<TARGET_FILE:pulp-scan-worker>"
        PULP_ISOLATED_SCANNER_CRASH_HELPER="$<TARGET_FILE:pulp-isolated-scanner-crash-helper>")
    add_dependencies(pulp-test-isolated-scanner
        pulp-scan-worker
        pulp-isolated-scanner-crash-helper)
    catch_discover_tests(pulp-test-isolated-scanner)
endif()
# Hosted editor API (workstream 03 slice 3.4)
pulp_add_test_suite(pulp-test-hosted-editor LIBRARIES pulp::host)
# HostedEditor → WindowHost attachment migration (item 4.4 macOS plan)
pulp_add_test_suite(pulp-test-hosted-editor-migration LIBRARIES pulp::view pulp::host)
# CFRunLoop cooperation — plugin-mode MainThreadDispatcher (item 6.4b macOS plan)
pulp_add_test_suite(pulp-test-cfrunloop-cooperation LIBRARIES pulp::events)
if(APPLE)
    target_link_libraries(pulp-test-cfrunloop-cooperation PRIVATE "-framework CoreFoundation")
endif()
# High-level window classes (item 6.5 macOS plan)
pulp_add_test_suite(pulp-test-document-window LIBRARIES pulp::view)
# Accessibility announcements (workstream 04 slice 4.3)
pulp_add_test_suite(pulp-test-announce LIBRARIES pulp::view)
# ARA core type layer (workstream 06 slice 6.2)
pulp_add_test_suite(pulp-test-ara-types LIBRARIES pulp::format)
# Processor on_host_transport_changed hook (workstream 01 slice 1.11)
pulp_add_test_suite(pulp-test-transport-hook LIBRARIES pulp::format)
# Scan cache (workstream 03 slice 3.1)
add_executable(pulp-test-scan-cache test_scan_cache.cpp)
target_link_libraries(pulp-test-scan-cache PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow` (#1589): each scenario waits for filesystem mtime resolution
# (~1s each on every platform). Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scan-cache PROPERTIES LABELS slow)
# Processor memory-pressure hook (workstream 05 slice 5.3)
pulp_add_test_suite(pulp-test-memory-pressure LIBRARIES pulp::format)
# iOS background-audio descriptor flag (workstream 05 slice 5.5)
pulp_add_test_suite(pulp-test-ios-background-audio-flag LIBRARIES pulp::format)

# MidiBuffer UMP sidecar (workstream 02 slice 2.6a)
pulp_add_test_suite(pulp-test-midi-buffer-ump LIBRARIES pulp::midi)
# MidiBuffer SysEx sidecar (workstream 01 — full MIDI vocabulary)
pulp_add_test_suite(pulp-test-midi-buffer-sysex LIBRARIES pulp::midi)
# Raw MIDI byte-stream parser — shared by ALSA raw-midi and any future
# raw-byte MIDI transport. Tests aborted-sysex recovery (#406 codex P2),
# realtime-inside-sysex passthrough (#239), runaway cap, and multi-read
# accumulator state.
pulp_add_test_suite(pulp-test-raw-midi-parser LIBRARIES pulp::midi)
# Win MIDI MIM_LONGDATA SysEx + QPC timestamps (#19 / #245 partial).
# Cross-platform — compiles everywhere; Windows-specific cases are
# guarded behind #ifdef _WIN32.
pulp_add_test_suite(pulp-test-winmidi-sysex LIBRARIES pulp::midi)
# Accessibility provider cross-platform entry (workstream 04 slices 4.1 + 4.2)
pulp_add_test_suite(pulp-test-accessibility-provider LIBRARIES pulp::view)
# PluginDescriptor validation (workstream 01 slice 1.9)
pulp_add_test_suite(pulp-test-descriptor-validation LIBRARIES pulp::format)
# PluginInfo metadata shape (workstream 03 slice 3.7)
pulp_add_test_suite(pulp-test-plugin-info-metadata LIBRARIES pulp::host)
# UIA mapping (workstream 04 slice 4.1a)
pulp_add_test_suite(pulp-test-uia-mapping LIBRARIES pulp::view)

# AudioSystem hotplug base plumbing (workstream 02 slice 2.7)
pulp_add_test_suite(pulp-test-audio-system-hotplug LIBRARIES pulp::audio)

# WASAPI capture path (#243). Windows-only; cross-platform stub keeps
# CI green elsewhere. Skips at runtime on hosts without a default
# capture endpoint.
add_executable(pulp-test-wasapi-input test_wasapi_input.cpp)
target_link_libraries(pulp-test-wasapi-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-input)

# WASAPI share-mode coverage contract (gap-doc Phase 0 audit, #302).
# Pins "shared-mode only" surface; static_asserts fire on every platform
# so a Windows-only DeviceConfig extension still trips this on macOS/Linux CI.
add_executable(pulp-test-wasapi-share-modes test_wasapi_share_modes.cpp)
target_link_libraries(pulp-test-wasapi-share-modes PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-share-modes PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-share-modes)

# ALSA capture + real device metadata (#20 / #215). Linux-only;
# cross-platform stub keeps CI green elsewhere. Skips at runtime on
# hosts without an ALSA-routable default device.
add_executable(pulp-test-alsa-input test_alsa_input.cpp)
target_link_libraries(pulp-test-alsa-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-alsa-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-alsa-input)
# Processor on_host_tempo_changed hook (workstream 01 slice 1.10)
pulp_add_test_suite(pulp-test-tempo-hook LIBRARIES pulp::format)
# Resizable plugin shell (workstream 07 slice 7.5)
pulp_add_test_suite(pulp-test-resizable-shell LIBRARIES pulp::view)
# ATK / AT-SPI mapping (workstream 04 slice 4.2a)
pulp_add_test_suite(pulp-test-atk-mapping LIBRARIES pulp::view)
# Running-status MIDI parser (workstream 02 slice 2.5)
pulp_add_test_suite(pulp-test-running-status LIBRARIES pulp::midi)
# Background scanner (workstream 03 slice 3.2)
pulp_add_test_suite(pulp-test-background-scanner LIBRARIES pulp::host)
# Host parameter-event queue
pulp_add_test_suite(pulp-test-parameter-event-queue LIBRARIES pulp::host)

# TextShaper (PreText-style measure-once-reflow-forever)
pulp_add_test_suite(pulp-test-text-shaper LIBRARIES pulp::canvas)

# Item 6.8 / 2026-05-24 macOS plugin-authoring plan — SheenBidi-backed
# BidiAnalyzer + TextRunPlanner non-ICU fallback. Exercises pure-Arabic,
# pure-Hebrew, and mixed Latin+Arabic/Hebrew strings. Each TEST_CASE
# gates RTL-specific assertions on BidiAnalyzer::has_sheenbidi() so the
# suite still passes on reduced-deps configs that compile the
# pass-through stub.
pulp_add_test_suite(pulp-test-bidi-text LIBRARIES pulp::canvas)

# pulp #2163 / font v2 Slice 2.7 — FontScope memory-budget eviction.
pulp_add_test_suite(pulp-test-font-scope-budget LIBRARIES pulp::canvas)

# pulp #2163 / font v2 Slice 2.8 — TTF/OTF structural sanitizer.
# Gated on PULP_HAS_SKIA: the non-Skia stub in font_registry_stubs.cpp
# accepts any non-empty buffer, so the rejection cases false-negative.
# (Codex review on PR #2177, P2.)
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-security LIBRARIES pulp::canvas)
endif()

# pulp #2163 / font v2 Slice 2.3 — variable axis wiring.
# Gated on PULP_HAS_SKIA: FontResolver::resolve_family_list returns
# NotFound when Skia is absent, so the has_typeface() assertions
# false-negative. (Codex review on PR #2180, P1.)
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-variable-axes LIBRARIES pulp::canvas)
endif()

# pulp #2163 / font v2 Slice 3.6 — UAX #29-lite cluster_step.
pulp_add_test_suite(pulp-test-cluster-step LIBRARIES pulp::canvas)

# pulp #2163 / font v2 Slice 2.4 — locale-aware word + line breaking.
# The surface always links: when ICU's <unicode/brkiter.h> is on the
# include path the assertions exercise the real UAX #14 path against
# the SkUnicode_icu symbols bundled in libskia.a; otherwise the
# degraded ASCII-space fallback keeps the API functional and the
# English-only expectations still hold.
pulp_add_test_suite(pulp-test-font-locale-shaping LIBRARIES pulp::canvas)

# pulp #2163 / font v2 Slice 3.2 — AA / hinting / subpixel policy centralization.
add_executable(pulp-test-font-aa-hinting test_font_aa_hinting.cpp)
target_link_libraries(pulp-test-font-aa-hinting PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_link_libraries(pulp-test-font-aa-hinting PRIVATE skia::skia)
endif()
catch_discover_tests(pulp-test-font-aa-hinting)

# pulp #2163 / font v2 Slice 2.2 — FontFlightRecorder + JSON drain.
add_executable(pulp-test-font-flight-recorder test_font_flight_recorder.cpp)
target_link_libraries(pulp-test-font-flight-recorder PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-flight-recorder)

# pulp #2163 / font v2 Slice 3.3 — variable-font axis-animation LRU cache.
add_executable(pulp-test-font-axis-animation test_font_axis_animation.cpp)
target_link_libraries(pulp-test-font-axis-animation PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-axis-animation)

# pulp #2163 / font v2 Slice 3.7 — parallel shaping via TextRunPlanner::shape_batch.
add_executable(pulp-test-text-run-planner-parallel test_text_run_planner_parallel.cpp)
target_link_libraries(pulp-test-text-run-planner-parallel PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-run-planner-parallel)

# pulp #2163 / font v2 Slice 1.2.a finish — real ICU bidi + script run
# iterators. Verifies multi-run shaping on bidi / script boundaries and
# UAX #29 cluster grouping (ZWJ, regional-indicator-pair, virama,
# combining marks). Cluster-grouping cases run on every build; the
# bidi / script cases auto-skip under non-Skia builds via SUCCEED.
#
# Add PULP_HAS_SKIA explicitly so the ICU-path assertions register
# (mirrors test_canvas_fonts.cpp's pattern — pulp::canvas exports the
# define PUBLIC but CMake's PUBLIC propagation doesn't always reach
# sibling test targets, depending on configure ordering).
add_executable(pulp-test-text-run-planner-icu test_text_run_planner_icu.cpp)
target_link_libraries(pulp-test-text-run-planner-icu PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-text-run-planner-icu PRIVATE PULP_HAS_SKIA=1)
endif()
catch_discover_tests(pulp-test-text-run-planner-icu)

# pulp #2163 / font v2 Slice 2.1 — async font lifecycle (register_font_url).
add_executable(pulp-test-font-async-lifecycle test_font_async_lifecycle.cpp)
target_link_libraries(pulp-test-font-async-lifecycle PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-async-lifecycle)

# pulp #2163 / font v2 Slice 3.5 — WOFF2 runtime decoding (security-gated).
# Structural rejection + decoder-availability probe; no Skia link
# required because the negative paths are universal.
add_executable(pulp-test-font-woff2 test_font_woff2.cpp)
target_link_libraries(pulp-test-font-woff2 PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-woff2)

# pulp #2163 / font v2 Slice 3.1 — color-font predicate on ResolvedFont.
# Gated on PULP_HAS_SKIA — the predicate returns false without a real
# typeface so the resolver-bound assertions can't fire on non-Skia.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-font-color-mode test_font_color_mode.cpp)
    target_link_libraries(pulp-test-font-color-mode PRIVATE pulp::canvas Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-font-color-mode)
endif()

# Font subsystem typed options and scope generation tests
add_executable(pulp-test-font-options test_font_options.cpp)
target_link_libraries(pulp-test-font-options PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-options)

# pulp #2163 — font v2 Slice 1.3 measurement-paint parity harness.
# Asserts TextShaper predictions match Skia's measured advance within
# 0.5px for a hand-picked sample set representative of CHAIN INFO /
# CROSSOVER bugs. Full multilingual torture corpus (test/text_corpus/
# corpus.json) loader + per-TextAnchor bbox assertions land alongside
# the JSON parser link in a follow-up.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-parity test_parity.cpp)
    target_link_libraries(pulp-test-parity PRIVATE pulp::canvas Catch2::Catch2WithMain skia::skia)
    catch_discover_tests(pulp-test-parity)
endif()

# Font v2 Slice 3.4 — cross-backend rendering goldens.
# Catches font-cascade / hinting / AA regressions in the Skia
# raster path before they land. Uses a structural digest (width,
# height, opaque-pixel count, darkness sum) with ±5% tolerance
# rather than byte-exact pixel hashes — see the test header for
# the rationale. Skia-only; the bare TU still builds without
# Skia (single soft-skip case) so this stays portable.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-font-rendering-goldens
        test_font_rendering_goldens.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens PRIVATE
        pulp::canvas Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-font-rendering-goldens PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens)
endif()

# Font v2 Slice 3.4 — Skia GPU lane (Dawn → Metal on macOS-arm64).
# Sibling of `pulp-test-font-rendering-goldens` (raster). Same
# three strings, same structural digest, ±15% tolerance to absorb
# Graphite/Metal subpixel + AA drift. Gated at compile time on
# PULP_HAS_SKIA && APPLE && PULP_ENABLE_GPU — the test soft-skips
# at runtime when Dawn / Graphite init fails so CI lanes without a
# working adapter don't hard-fail. Vulkan + D3D backends are
# deferred — see the PULP_VULKAN_AVAILABLE / PULP_D3D_AVAILABLE
# scaffold blocks immediately below.
if(PULP_HAS_SKIA AND APPLE AND PULP_ENABLE_GPU)
    add_executable(pulp-test-font-rendering-goldens-gpu
        test_font_rendering_goldens_gpu.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-gpu PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-font-rendering-goldens-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    # APPLE-gated, so the Windows webgpu DL_PATHS dance from the
    # PULP_GPU_TEST_DISCOVERY_ARGS block below isn't needed here.
    catch_discover_tests(pulp-test-font-rendering-goldens-gpu)

    # GPU view-host-in-plugins (P1) — proves a Processor::create_view() editor
    # that sets requires_gpu_host() auto-selects the GPU host, mounts a real
    # (non-AutoUi) tree, and paints a nonblank offscreen Dawn/Skia frame.
    # Soft-skips on Dawn-init failure.
    add_executable(pulp-test-plugin-editor-headless-gpu
        test_plugin_editor_headless_gpu.cpp)
    target_link_libraries(pulp-test-plugin-editor-headless-gpu PRIVATE
        pulp::format pulp::view pulp::state pulp::canvas pulp::render
        Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-plugin-editor-headless-gpu PRIVATE
        PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-plugin-editor-headless-gpu PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-plugin-editor-headless-gpu)

    # Embedded-host smoke (mac GPU lane): attaches the GPU host to a hidden
    # NSWindow and proves a nonblank first-frame capture, plus drives the CLAP
    # gui_create/set_parent adapter path. Needs CLAP for the PULP_CLAP_PLUGIN
    # entry. Soft-skips without a Dawn adapter / window server.
    if(PULP_HAS_CLAP)
        add_executable(pulp-test-plugin-editor-host-smoke-mac
            test_plugin_editor_host_smoke_mac.mm
            ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp)
        target_link_libraries(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            pulp::format pulp::view pulp::state pulp::canvas pulp::render
            clap Catch2::Catch2WithMain skia::skia "-framework AppKit")
        target_compile_definitions(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            PULP_HAS_SKIA=1 PULP_CLAP_GUI=1)
        target_include_directories(pulp-test-plugin-editor-host-smoke-mac PRIVATE
            ${SKIA_INCLUDE_DIRS})
        catch_discover_tests(pulp-test-plugin-editor-host-smoke-mac)
    endif()
endif()

# Font v2 Slice 3.4 — Skia GPU Vulkan lane (SCAFFOLD).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The Vulkan
# lane is intentionally OFF by default: there is no Vulkan-capable
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <vulkan/vulkan.h>"
# failures. Flip `PULP_VULKAN_AVAILABLE=ON` on a future Linux or
# Windows CI lane (with Skia-Vulkan linked) to light the target up.
# Tolerance for the raster↔Vulkan cross-backend probe is ±20 % —
# see the test header for the rationale.
option(PULP_VULKAN_AVAILABLE
    "Build the Skia GPU Vulkan rendering-goldens scaffold (font v2 \
Slice 3.4 follow-up). Requires Skia built with skia_use_vulkan=true \
and a working Vulkan ICD on the host. OFF on every Pulp CI lane \
today; flip ON when a Vulkan-capable runner exists."
    OFF)
if(PULP_HAS_SKIA AND (UNIX AND NOT APPLE OR WIN32) AND PULP_VULKAN_AVAILABLE)
    add_executable(pulp-test-font-rendering-goldens-vulkan
        test_font_rendering_goldens_vulkan.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-vulkan PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-vulkan PRIVATE
        PULP_HAS_SKIA=1
        PULP_VULKAN_AVAILABLE=1)
    target_include_directories(pulp-test-font-rendering-goldens-vulkan PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens-vulkan)
endif()

# Font v2 Slice 3.4 — Skia GPU Direct3D 12 lane (SCAFFOLD).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The D3D
# lane is intentionally OFF by default: there is no Windows D3D
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <d3d12.h>" failures.
# Flip `PULP_D3D_AVAILABLE=ON` on a future Windows CI lane (with
# Skia-D3D linked) to light the target up. Tolerance for the
# raster↔D3D cross-backend probe is ±20 % — see the test header for
# the rationale.
option(PULP_D3D_AVAILABLE
    "Build the Skia GPU Direct3D 12 rendering-goldens scaffold \
(font v2 Slice 3.4 follow-up). Requires Skia built with \
skia_use_direct3d=true on Windows. OFF on every Pulp CI lane \
today; flip ON when a Windows D3D runner exists."
    OFF)
if(PULP_HAS_SKIA AND WIN32 AND PULP_D3D_AVAILABLE)
    add_executable(pulp-test-font-rendering-goldens-d3d
        test_font_rendering_goldens_d3d.cpp)
    target_link_libraries(pulp-test-font-rendering-goldens-d3d PRIVATE
        pulp::canvas pulp::render Catch2::Catch2WithMain skia::skia)
    target_compile_definitions(pulp-test-font-rendering-goldens-d3d PRIVATE
        PULP_HAS_SKIA=1
        PULP_D3D_AVAILABLE=1)
    target_include_directories(pulp-test-font-rendering-goldens-d3d PRIVATE
        ${SKIA_INCLUDE_DIRS})
    catch_discover_tests(pulp-test-font-rendering-goldens-d3d)
endif()

# Analytics tests
pulp_add_test_suite(pulp-test-analytics LIBRARIES pulp::runtime)

# Crypto tests (SHA-256, MD5, AES, machine ID)
pulp_add_test_suite(pulp-test-crypto LIBRARIES pulp::runtime)

# Ed25519 (RFC 8032) — macOS plan item 7.1b
pulp_add_test_suite(pulp-test-ed25519 LIBRARIES pulp::runtime)

# IPC (InterprocessConnection) tests
add_executable(pulp-connected-child-process-fixture
    fixtures/connected_child_process_fixture.cpp)
target_link_libraries(pulp-connected-child-process-fixture PRIVATE pulp::events)

pulp_add_test_suite(pulp-test-ipc LIBRARIES pulp::events pulp::runtime)
add_dependencies(pulp-test-ipc pulp-connected-child-process-fixture)
target_compile_definitions(pulp-test-ipc PRIVATE
    "PULP_TEST_CONNECTED_CHILD_FIXTURE=\"$<TARGET_FILE:pulp-connected-child-process-fixture>\"")

pulp_add_test_suite(pulp-test-ipc-endpoints LIBRARIES pulp::events pulp::runtime)

# AttributedString and TextLayout tests
pulp_add_test_suite(pulp-test-attributed-string LIBRARIES pulp::canvas)

# i18n translation tests
pulp_add_test_suite(pulp-test-i18n LIBRARIES pulp::runtime)

# StateTree and ObservableValue tests
pulp_add_test_suite(pulp-test-state-tree LIBRARIES pulp::state pulp::runtime)

# GUI component tests (table, toolbar, concertina, buttons, lasso)
pulp_add_test_suite(pulp-test-gui-components LIBRARIES pulp::view)

pulp_add_test_suite(pulp-test-table-list-box LIBRARIES pulp::view)

# CodeEditor / FileBasedDocument / RecentlyOpenedFilesList tests
pulp_add_test_suite(pulp-test-code-editor LIBRARIES pulp::view)

# PropertiesFile JSON persistence tests
pulp_add_test_suite(pulp-test-properties SOURCES test_properties_file.cpp LIBRARIES pulp::state pulp::runtime)

# AudioDeviceManager — persistence + MIDI hub (macOS plan item 1.2a)
pulp_add_test_suite(pulp-test-audio-device-manager
    SOURCES test_audio_device_manager.cpp
    LIBRARIES pulp::audio pulp::midi pulp::state pulp::runtime)

# DSP enhancement tests (dry/wet mixer, processor duplicator, matrix, special functions)
pulp_add_test_suite(pulp-test-dsp-enhancements LIBRARIES pulp::signal)

# Elliptic / Jacobi special functions (pulp::signal::special). Kept in its
# own binary so the upcoming elliptic IIR design slice can grow tests
# beside it without bloating pulp-test-dsp-enhancements.
pulp_add_test_suite(pulp-test-special-functions LIBRARIES pulp::signal)

# Animation & 3D math tests
pulp_add_test_suite(pulp-test-animation-3d LIBRARIES pulp::view)

# AnimatorSet / 3D types tests
pulp_add_test_suite(pulp-test-animator-set LIBRARIES pulp::view)

# Runtime utility tests (mmap, temp file, dynlib, base64, range, child process)
pulp_add_test_suite(pulp-test-runtime-utils LIBRARIES pulp::runtime)

# XML and ZIP/GZIP compression tests
pulp_add_test_suite(pulp-test-xml-zip LIBRARIES pulp::runtime)

# SIMD operations and aligned buffer tests
pulp_add_test_suite(pulp-test-simd LIBRARIES pulp::runtime pulp::signal)

# Drag-and-drop tests
pulp_add_test_suite(pulp-test-dnd SOURCES test_drag_drop.cpp LIBRARIES pulp::view)

# OSC tests
pulp_add_test_suite(pulp-test-osc LIBRARIES pulp::osc)

# ImageConvolutionKernel tests
pulp_add_test_suite(pulp-test-image-convolution LIBRARIES pulp::canvas)

# RectangleList geometry tests
pulp_add_test_suite(pulp-test-rectangle-list LIBRARIES pulp::canvas)

# OSC Bundle serialization tests
pulp_add_test_suite(pulp-test-osc-bundle LIBRARIES pulp::osc)

# SVG tests
pulp_add_test_suite(pulp-test-svg LIBRARIES pulp::canvas)

# VectorScene retained scene graph (item 6.1 of the macOS plugin-authoring
# plan, 2026-05-24). Pulp-native names; covers SceneRect/SceneTransform
# math, every SceneNode kind's local_bounds + paint command stream, SVG
# ingest into a SceneGroup, and the dirty-rect contract (mutating one
# child's opacity reports a rect bounded by that sub-tree only).
pulp_add_test_suite(pulp-test-vector-scene LIBRARIES pulp::canvas)

# Effects tests
pulp_add_test_suite(pulp-test-effects LIBRARIES pulp::canvas)

# Signal/DSP tests
pulp_add_test_suite(pulp-test-signal LIBRARIES pulp::signal)

# Signal filter tests (P5 test-split — extracted from test_signal.cpp).
# Biquad / SVF / LadderFilter / LinkwitzRiley TEST_CASE clusters moved
# verbatim into a sibling TU to keep test_signal.cpp under ~1,200 lines.
pulp_add_test_suite(pulp-test-signal-filters LIBRARIES pulp::signal)

# Signal spectral tests (P5 test-split — extracted from test_signal.cpp).
# WindowFunction / FFT / Convolver TEST_CASE clusters moved verbatim.
pulp_add_test_suite(pulp-test-signal-spectral LIBRARIES pulp::signal)

# Signal meter tests (P5 test-split — extracted from test_signal.cpp).
# MultiChannelMeter / MultiChannelBallistics TEST_CASE clusters moved
# verbatim. Distinct from pulp-test-multi-channel-meter (test_multi_
# channel_meter.cpp), which keeps its own #645 edge-case coverage.
pulp_add_test_suite(pulp-test-signal-meter LIBRARIES pulp::signal)

# Biquad filter tests
pulp_add_test_suite(pulp-test-biquad LIBRARIES pulp::signal)

# Multi-channel meter edge tests (#645)
pulp_add_test_suite(pulp-test-multi-channel-meter LIBRARIES pulp::signal)

# DSL processor contract tests (FaustProcessor + PulpFaustUI + PulpFaustMeta)
add_executable(pulp-test-dsl-processor test_dsl_processor.cpp)
target_link_libraries(pulp-test-dsl-processor PRIVATE
    pulp::dsl pulp::format pulp::state pulp::audio pulp::midi
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsl-processor)

# Convolution engine tests
pulp_add_test_suite(pulp-test-convolver LIBRARIES pulp::signal)

# Background-IR-swap tests for PartitionedConvolver (macOS plugin authoring
# plan item 2.3, Slice A). Lock-free pointer-shuttle hand-off from worker
# thread → audio thread, including a concurrent stage/swap hammer test.
pulp_add_test_suite(pulp-test-convolver-bg-swap LIBRARIES pulp::signal pulp::runtime)

# Non-uniform partitioned convolver tests (macOS plugin authoring plan
# item 2.3, Slice B). Two-stage Gardner-style head (small block) +
# tail (K× larger block) with zero-latency mixing.
pulp_add_test_suite(pulp-test-convolver-non-uniform LIBRARIES pulp::signal)

# Test signal source (sine tone, file playback)
pulp_add_test_suite(pulp-test-test-signal LIBRARIES pulp::standalone)

# Standalone editor chrome helpers
pulp_add_test_suite(pulp-test-standalone-editor-chrome LIBRARIES pulp::standalone)
pulp_add_test_suite(pulp-test-standalone-apply-config LIBRARIES pulp::standalone)
pulp_add_test_suite(pulp-test-standalone-transport-midi LIBRARIES pulp::standalone)

# Headless screenshot capture state machine (pulp #468)
pulp_add_test_suite(pulp-test-screenshot-capture LIBRARIES pulp::standalone)

# STFT and audio visualization signal tests
pulp_add_test_suite(pulp-test-stft LIBRARIES pulp::signal)

# Visualization bridge and widget tests
pulp_add_test_suite(pulp-test-visualization LIBRARIES pulp::view)

# Clipboard tests
add_executable(pulp-test-clipboard test_clipboard.cpp)
target_link_libraries(pulp-test-clipboard PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clipboard
    PROPERTIES RESOURCE_LOCK system-clipboard)

# FileDialog backend-registration tests (#301)
pulp_add_test_suite(pulp-test-file-dialog LIBRARIES pulp::platform)

# Platform tests
pulp_add_test_suite(pulp-test-platform LIBRARIES pulp::platform)

# Permissions tests
pulp_add_test_suite(pulp-test-permissions LIBRARIES pulp::platform)

# Environment API tests (#342)
pulp_add_test_suite(pulp-test-environment LIBRARIES pulp::platform)

# Runtime tests
pulp_add_test_suite(pulp-test-runtime LIBRARIES pulp::runtime)

# AbstractFifo (index-pair SPSC FIFO management)
pulp_add_test_suite(pulp-test-abstract-fifo LIBRARIES pulp::runtime)

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
pulp_add_test_suite(pulp-test-events LIBRARIES pulp::events)

pulp_add_test_suite(pulp-test-events-async-helpers LIBRARIES pulp::events)

add_executable(pulp-test-events-timer-helpers test_events_timer_helpers.cpp)
target_link_libraries(pulp-test-events-timer-helpers PRIVATE pulp::events Catch2::Catch2WithMain)
# `slow` (#1589): Timer hammer + UAF-free destroy-while-dispatched
# tests deliberately exercise message-loop pacing (~0.3-1.5 sec each
# depending on platform). Excluded from PR fast-CI; sanitizers.yml
# still covers Timer under TSan.
catch_discover_tests(pulp-test-events-timer-helpers PROPERTIES LABELS slow)

# Audio file I/O tests
pulp_add_test_suite(pulp-test-audio-file LIBRARIES pulp::audio)

# AudioThumbnail + AudioThumbnailCache (item 6.12).
# Links pulp::view so the WaveformView integration smoke compiles.
pulp_add_test_suite(pulp-test-audio-thumbnail LIBRARIES pulp::audio pulp::view)

# WAV metadata round-trip tests (BWAV / iXML / ASWG / ACID — item 6.11)
pulp_add_test_suite(pulp-test-wav-metadata LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-offline-processor-edges LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-ogg-reader LIBRARIES pulp::audio)

# Audio tests
pulp_add_test_suite(pulp-test-audio LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-system-volume LIBRARIES pulp::audio)

# Excerpt window enumeration tests
pulp_add_test_suite(pulp-test-audio-excerpt LIBRARIES pulp::audio)

# Repo-level audio tooling tests
pulp_add_test_suite(pulp-test-audio-tools LIBRARIES pulp::tool-audio)

# MCP server protocol tests
add_executable(pulp-test-mcp-server test_mcp_server.cpp)
target_link_libraries(pulp-test-mcp-server PRIVATE pulp::tool-audio Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-mcp-server PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# test_mcp_server.cpp #include's ../tools/mcp/pulp_mcp.cpp directly (via
# `#define main pulp_mcp_main_for_test`), so it picks up the same
# generated version header pulp-mcp itself sees. The generated header
# lives under the binary tree's tools/mcp/ directory.
target_include_directories(pulp-test-mcp-server PRIVATE
    "${CMAKE_BINARY_DIR}/tools/mcp")
add_dependencies(pulp-test-mcp-server pulp-mcp)
catch_discover_tests(pulp-test-mcp-server)

# MIDI tests
pulp_add_test_suite(pulp-test-midi LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-midi-file LIBRARIES pulp::midi)

# State tests
pulp_add_test_suite(pulp-test-state LIBRARIES pulp::state pulp::events)

# Binding tests
pulp_add_test_suite(pulp-test-binding LIBRARIES pulp::state)

# Headless adapter tests
pulp_add_test_suite(pulp-test-headless LIBRARIES pulp::format)

# Diagnostic reporter tests
pulp_add_test_suite(pulp-test-diagnostic SOURCES test_diagnostic_reporter.cpp LIBRARIES pulp::format)

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-entry PRIVATE PULP_CLAP_GUI=1)
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

    # Item 3.4 (macOS plan) — pin the CLAP host-facing API contract that
    # Bitwig / Reaper / FL Studio / Studio One depend on. Re-compiles the
    # adapter TU into its own executable so the test owns the
    # PULP_CLAP_PLUGIN(...) entry-symbol generator.
    add_executable(pulp-test-clap-host-validation test_clap_host_validation.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-host-validation PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-host-validation PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-host-validation)
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
pulp_add_test_suite(pulp-test-image-decode LIBRARIES pulp::view)

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

    # Skia surface test — also covers SkPicture (.skp) serialization on the
    # Graphite backend (Phase 6.4 spike). The .skp round-trip cases include
    # Skia headers directly (SkPicture, SkPictureRecorder, SkSerialProcs,
    # SkPngEncoder), so this target needs SKIA_INCLUDE_DIRS even though
    # pulp::render carries Skia privately.
    add_executable(pulp-test-skia-surface test_skia_surface.cpp)
    target_link_libraries(pulp-test-skia-surface PRIVATE pulp::render Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-skia-surface PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-skia-surface PRIVATE ${SKIA_INCLUDE_DIRS})
        if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
            target_include_directories(pulp-test-skia-surface PRIVATE
                "${SKIA_INCLUDE_DIRS}/dawn")
        endif()
    endif()
    catch_discover_tests(pulp-test-skia-surface ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # HeadlessSurface CI wrapper (plan §6.7) — one-call offscreen
    # Dawn/Skia helper for golden-test fixtures. Compiles on every
    # build; runtime cases gate on PULP_HAS_SKIA && __APPLE__ at the
    # source level and otherwise soft-skip when no Dawn adapter is
    # available. Needs Skia headers directly for the SkPngEncoder /
    # SkPixmap symbols the wrapper uses to PNG-encode the readback.
    add_executable(pulp-test-headless-surface test_headless_surface.cpp)
    target_link_libraries(pulp-test-headless-surface PRIVATE
        pulp::render pulp::canvas Catch2::Catch2WithMain)
    if(PULP_HAS_SKIA)
        target_compile_definitions(pulp-test-headless-surface PRIVATE
            PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-headless-surface PRIVATE
            ${SKIA_INCLUDE_DIRS})
    endif()
    catch_discover_tests(pulp-test-headless-surface
        ${PULP_GPU_TEST_DISCOVERY_ARGS})

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
pulp_add_test_suite(pulp-test-animation LIBRARIES pulp::view)

# FrameClock tests
pulp_add_test_suite(pulp-test-frame-clock LIBRARIES pulp::view)

# Motion (agent-first motion observability) — Phase 0 bedrock tests
pulp_add_test_suite(pulp-test-motion LIBRARIES pulp::view)

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

    # Motion scrubber — Phase 7 timeline scrubber that consumes
    # .motion.jsonl fixtures and re-emits events up to a frame playhead.
    add_executable(pulp-test-motion-scrubber test_motion_scrubber.cpp)
    target_link_libraries(pulp-test-motion-scrubber PRIVATE
        pulp::view pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-scrubber)
endif()

# Motion end-to-end animation smoke — mirrors what a plugin author
# would write: drive a real Tween, publish_value each tick, read the
# fixture back, assert the shape matches the configured animation.
add_executable(pulp-test-motion-animation-smoke test_motion_animation_smoke.cpp)
target_link_libraries(pulp-test-motion-animation-smoke PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-animation-smoke)

# Motion provenance adapters — Phase 9 surface coverage. One test per
# animation surface (Tween + PULP_MOTION_TWEEN, AnimatorSetBuilder::name,
# CSS TransitionSpec, ambient slot for rAF + design-import). Verifies the
# Provenance envelope round-trips end to end through the publish channel.
add_executable(pulp-test-motion-provenance test_motion_provenance.cpp)
target_link_libraries(pulp-test-motion-provenance PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-provenance)

# Motion input record + replay end-to-end (Phase 10) — records a
# hover -> click -> drag against a synthetic view tree, replays the
# same fixture against a fresh tree, asserts identical motion fixture
# emerges (modulo timing tolerance).
add_executable(pulp-test-motion-input-replay test_motion_input_replay.cpp)
target_link_libraries(pulp-test-motion-input-replay PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-input-replay)

# Motion cost attribution — correlates per-frame render cost +
# dirty-rect area with the trace_ids that emitted on the same frame
# (carrying their Phase 9 provenance envelopes). The bridge probe
# test pulls real RenderPassManager + DirtyTracker stats, so this
# test target depends on pulp::render alongside pulp::view.
#
# Sanitizer builds (ASan/UBSan/TSan) disable PULP_ENABLE_GPU and so
# pulp::render is never created — skip the cost test in that case
# instead of failing the CMake generate step. The cost code itself
# is exercised by the GPU-on lanes (macOS local smoke + linux/windows
# release-path), so coverage isn't lost.
if(TARGET pulp::render)
    add_executable(pulp-test-motion-cost test_motion_cost.cpp)
    target_link_libraries(pulp-test-motion-cost PRIVATE
        pulp::view pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-cost)
endif()

# Motion Swift bridge — exercises the C ABI shims in
# apple/Sources/PulpSwift/PulpBridge.cpp without needing the Swift
# compiler. Verifies that pulp_motion_publish_value / publish_components,
# the ambient provenance slot, and register/update/detach geometry
# traces round-trip through motion::Coordinator and respect the
# off-by-default tracing gate.
add_executable(pulp-test-motion-swift-bridge
    test_motion_swift_bridge.cpp
    ${CMAKE_SOURCE_DIR}/apple/Sources/PulpSwift/PulpBridge.cpp)
target_include_directories(pulp-test-motion-swift-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/apple/Sources/PulpSwift)
target_link_libraries(pulp-test-motion-swift-bridge PRIVATE
    pulp::view pulp::state pulp::format Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-swift-bridge)

# Motion Android JNI bridge — exercises the C ABI shims in
# core/platform/src/android/jni_motion.cpp directly. The JNI shims
# (`Java_com_pulp_motion_PulpMotionNative_*`) only compile under
# __ANDROID__, but the underlying C ABI (`pulp_motion_*`) compiles on
# every host so this test runs in the normal Linux/macOS CTest matrix.
# Includes the Coordinator::reset() deadlock regression test that the
# Swift bridge's sibling-struct fix is the load-bearing change for.
add_executable(pulp-test-motion-android-bridge
    test_motion_android_bridge.cpp
    ${CMAKE_SOURCE_DIR}/core/platform/src/android/jni_motion.cpp)
target_include_directories(pulp-test-motion-android-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/core/platform/include)
target_link_libraries(pulp-test-motion-android-bridge PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-android-bridge)

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

    # Visual-plus extensions (grid overlay, trim, affine first→last).
    # Same SKIP_RETURN_CODE contract — runs only when numpy / Pillow /
    # scikit-image are available; optional cv2 path is exercised when
    # opencv-python is installed.
    add_test(
        NAME pulp-motion-visual-grid-self-check
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_grid_overlay.py
    )
    set_tests_properties(pulp-motion-visual-grid-self-check PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
    )

    # Motion-gated capture smoke. Exits 3 when neither screencapture
    # nor a booted simulator is available, so non-Apple CI hosts skip
    # cleanly. When the macOS capture path is usable it exercises the
    # gate logic against a static 4×4 region (gate never opens →
    # CLI returns 3, smoke wraps that into 0).
    add_test(
        NAME pulp-motion-visual-capture-smoke
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/motion/visual/test_capture_smoke.py
    )
    set_tests_properties(pulp-motion-visual-capture-smoke PROPERTIES
        SKIP_RETURN_CODE 3
        LABELS "motion;visual"
        TIMEOUT 60
    )
endif()

# Widget animation behavior tests
pulp_add_test_suite(pulp-test-widget-animation LIBRARIES pulp::view)

# Auto UI tests
pulp_add_test_suite(pulp-test-auto-ui LIBRARIES pulp::view)

# ViewBridge lifecycle tests
pulp_add_test_suite(pulp-test-view-bridge LIBRARIES pulp::format)

# Remote View Protocol loopback tests (Feature 1, Phase 4)
pulp_add_test_suite(pulp-test-remote-view LIBRARIES pulp::format pulp::runtime)

# Script engine tests (legacy API)
pulp_add_test_suite(pulp-test-script SOURCES test_script_engine.cpp LIBRARIES pulp::view)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow` (#1589): ScriptedUiSession reload tests sleep on file-watcher
# debounce + filesystem mtime; ~0.5-1 sec each. Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scripted-ui PROPERTIES LABELS slow)

# JS engine abstraction tests (shared across all backends)
pulp_add_test_suite(pulp-test-js-engine LIBRARIES pulp::view)

# Theme/design token tests
pulp_add_test_suite(pulp-test-theme LIBRARIES pulp::view)

# Screenshot tests (macOS only — uses CoreGraphics bitmap context)
if(APPLE)
    add_executable(pulp-test-screenshot test_screenshot.cpp)
    target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot)

    add_executable(pulp-test-screenshot-cli-contracts test_screenshot_cli_contracts.cpp)
    target_link_libraries(pulp-test-screenshot-cli-contracts PRIVATE
        pulp::view
        pulp::state
        Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-cli-contracts)
endif()

# Canvas tests
pulp_add_test_suite(pulp-test-canvas-color LIBRARIES pulp::canvas)

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

# Canvas CG-paths tests (P5-2 follow-up — extracted from test_canvas.cpp,
# 2026-05). Apple-only TU covering pulp #943 / #933 concat_transform,
# pulp #1322 Canvas2D path API (fill + curves + stroke + gradients),
# fill_rect / fill_path with active gradient, pulp #1368 save/restore
# tracking, and pulp #1322 BlendMode every-value round-trip. Companion
# to test_canvas_cg_gradients.cpp; the non-Apple parts stay in
# test_canvas.cpp.
add_executable(pulp-test-canvas-cg-paths test_canvas_cg_paths.cpp)
target_link_libraries(pulp-test-canvas-cg-paths PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    target_link_libraries(pulp-test-canvas-cg-paths PRIVATE
        "-framework CoreGraphics"
        "-framework ImageIO"
        "-framework CoreServices"
    )
endif()
catch_discover_tests(pulp-test-canvas-cg-paths)

# Canvas font tests (P5-2 first cut — extracted from test_canvas.cpp).
# Covers pulp #932 (bundled-font registration via match_bundled_typeface)
# + pulp #1150 (public register_font(path) API). Shares the
# PULP_TEST_FONT_PATH compile def so test fixtures find the bundled
# Inter-Regular.ttf regardless of CWD.
add_executable(pulp-test-canvas-fonts test_canvas_fonts.cpp)
target_link_libraries(pulp-test-canvas-fonts PRIVATE pulp::canvas Catch2::Catch2WithMain)
# pulp::canvas exports PULP_HAS_SKIA=1 as a PUBLIC compile def when Skia
# is available, but CMake's optimisation skips re-propagating PUBLIC defs
# to sibling targets that don't trigger a rebuild. Add the define + Skia
# include path explicitly so the #ifdef PULP_HAS_SKIA blocks register
# their TEST_CASEs.
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-fonts PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-fonts PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
target_compile_definitions(pulp-test-canvas-fonts PRIVATE
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\"")
catch_discover_tests(pulp-test-canvas-fonts)

# CG-degraded gradient + pattern cluster (P5-2 follow-up — extracted from
# test_canvas.cpp). Apple-only TU: every TEST_CASE drives CoreGraphics
# directly to prove the CoreGraphicsCanvas fallback honours the canvas2d
# spec (pulp #1524) — conic / two-circle radial / pattern + gradient-
# anchored fill/stroke text + line-dash CG paths.
add_executable(pulp-test-canvas-cg-gradients test_canvas_cg_gradients.cpp)
target_link_libraries(pulp-test-canvas-cg-gradients PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-cg-gradients PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-cg-gradients PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-cg-gradients)

# CSS filter chain coverage (P5-2 follow-up — extracted from
# test_canvas.cpp). pulp #1434 Phase A2-4: Skia-only pixel readback
# (contrast / invert / opacity ordering with drop-shadow, set_filter
# drop-shadow parser) + portable filter-chain matrix math
# (contrast bias, invert maps, identity, opacity alpha scaling).
add_executable(pulp-test-canvas-filter-chain test_canvas_filter_chain.cpp)
target_link_libraries(pulp-test-canvas-filter-chain PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-filter-chain PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-filter-chain PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-filter-chain)

# Canvas arcs / path-primitive Skia rasterization fixtures (P5-2
# follow-up — extracted from test_canvas.cpp). pulp #1521 native
# arc / arcTo / ellipse / roundRect cluster: RecordingCanvas captures
# of native primitives + Skia rasterization fixtures (full / half
# circle arc matches reference SkPath::arcTo, arc_to collinear
# lineTos, round_rect 4-corner radii, ellipse rotation contour
# continuity).
add_executable(pulp-test-canvas-arcs test_canvas_arcs.cpp)
target_link_libraries(pulp-test-canvas-arcs PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-arcs PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-arcs PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-arcs)

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
pulp_add_test_suite(pulp-test-sdf-atlas LIBRARIES pulp::canvas)

# MSDF glyph atlas (Phase 2 scaffold)
pulp_add_test_suite(pulp-test-msdf-atlas LIBRARIES pulp::canvas)

# PSDF glyph atlas + vector-fallback (Phase 3 scaffold)
pulp_add_test_suite(pulp-test-psdf-atlas LIBRARIES pulp::canvas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
pulp_add_test_suite(pulp-test-sdf-text LIBRARIES pulp::canvas)

# SDF effects layer (Phase 4 — host-side API + presets)
pulp_add_test_suite(pulp-test-sdf-effects LIBRARIES pulp::canvas)

# Runtime SDF atlas cache (Phase 5 — LRU + frame-based eviction)
pulp_add_test_suite(pulp-test-sdf-atlas-cache LIBRARIES pulp::canvas)

# Software SDF renderer (end-to-end headless raster)
pulp_add_test_suite(pulp-test-sdf-software-renderer LIBRARIES pulp::canvas)

# Runtime path → SDF conversion (Phase 5 — on-device SDF generation)
pulp_add_test_suite(pulp-test-path-to-sdf LIBRARIES pulp::canvas)

# View and layout tests
pulp_add_test_suite(pulp-test-view LIBRARIES pulp::view)

# View corner-radius tests (P5-3 first cut — extracted from test_view.cpp).
# Covers pulp #1731 Panel + View percent-radius math, per-corner radius
# resolution, reflow-on-bounds-change, view border stroke + outline +
# box-shadow honoring effective_corner_radius.
pulp_add_test_suite(pulp-test-view-corner-radius LIBRARIES pulp::view)

# View z-index + overflow tests (P5-3 follow-up — extracted from
# test_view.cpp). Covers pulp #972 z-index paint-order + hit-test +
# overflow:visible default for absolute-positioned popovers + pulp
# #1148 slice (a) symmetric overflow:visible hit-test extension.
pulp_add_test_suite(pulp-test-view-zindex-overflow LIBRARIES pulp::view)

# View CSS mask + overflow:visible hit-test extension (P5-3 follow-up).
# pulp #1148 slice (a) symmetric overflow:visible hit-test extension
# (500px in each direction so absolutely-positioned popovers protrude
# past their parent) + pulp #1737 / #1515 CSS mask-image + mask-size
# paint slice (save_layer_with_mask routing).
pulp_add_test_suite(pulp-test-view-mask-overflow LIBRARIES pulp::view)

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

# Phase 6.5: Dawn GPU timestamp queries. The pure tick->ms resolution
# math plus the CPU-only GpuTimestamps stub compile without a GPU link,
# so this builds the gpu_timestamps.cpp translation unit directly (it
# falls back to the no-op stub when PULP_HAS_SKIA is undefined). Live-
# device QuerySet resolution is covered by CI's GPU matrix.
add_executable(pulp-test-gpu-timestamps
    test_gpu_timestamps.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/gpu_timestamps.cpp)
target_include_directories(pulp-test-gpu-timestamps PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-timestamps PRIVATE
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-timestamps
    PROPERTIES
        LABELS coverage)

add_executable(pulp-test-gpu-render-time
    test_gpu_render_time.cpp)
target_include_directories(pulp-test-gpu-render-time PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-gpu-render-time PRIVATE
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-gpu-render-time
    PROPERTIES
        LABELS coverage)

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

    # Slice 16 — WindowHost::mark_dirty() VBlank-locked safe-repaint routing.
    # Needs both pulp::view (mark_dirty / attach_render_loop) and
    # pulp::render (RenderLoop factory), so it gates on PULP_ENABLE_GPU.
    add_executable(pulp-test-window-host-repaint test_window_host_repaint.cpp)
    target_link_libraries(pulp-test-window-host-repaint PRIVATE
        pulp::view pulp::render Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-window-host-repaint)

endif()

# Texture atlas helpers compile cleanly without GPU deps; keep this pure
# test available in GPU-off builds so sanitizer and local coverage lanes do
# not fetch GPU assets. The atlas .cpp impl is compiled directly into the
# test target rather than linking pulp::render (which would pull in Skia/Dawn).
add_executable(pulp-test-texture-atlas
    test_texture_atlas.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/texture_atlas.cpp)
target_include_directories(pulp-test-texture-atlas PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include)
target_link_libraries(pulp-test-texture-atlas PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-texture-atlas)

# Sprite strip tests
pulp_add_test_suite(pulp-test-sprite-strip LIBRARIES pulp::view)

# Edit history (global undo) tests
pulp_add_test_suite(pulp-test-edit-history LIBRARIES pulp::state)

# macOS plan Batch P — HostQuirks scaffolding + HostVersion detection
pulp_add_test_suite(pulp-test-host-version LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-host-quirks LIBRARIES pulp::format)

# macOS plan Batch B (partial: mixers) — DryWetMixer + Panner pan laws
pulp_add_test_suite(pulp-test-pan-mix-laws LIBRARIES pulp::signal)

# macOS plan Batch E (partial) — UMP per-note + JR Clock
pulp_add_test_suite(pulp-test-ump-extensions LIBRARIES pulp::midi)

# macOS plan Batch D (partial) — HMAC + AEAD primitives
pulp_add_test_suite(pulp-test-hmac-aead LIBRARIES pulp::runtime)

# macOS plan Batch C (partial) — MidiMessageCollector (UI→audio MIDI hub)
pulp_add_test_suite(pulp-test-midi-message-collector LIBRARIES pulp::midi)

# macOS plan Batch G (partial) — Compressor sidechain HPF + lookahead
pulp_add_test_suite(pulp-test-compressor-sidechain LIBRARIES pulp::signal)

# macOS plan Batch R — ARA scaffold validation
pulp_add_test_suite(pulp-test-ara-scaffold LIBRARIES pulp::format)

# macOS plan Batch E remainder — MIDI 1.0 backend audit (item 8.6)
pulp_add_test_suite(pulp-test-midi1-backend-audit LIBRARIES pulp::midi)

# macOS plan Batch C — BufferOps SIMD helpers (item 1.6)
pulp_add_test_suite(pulp-test-buffer-ops LIBRARIES pulp::audio pulp::runtime)

# macOS plan Batch E remainder — MpeVoiceTracker per-note management +
# assignable PNC consumption (item 8.5b)
pulp_add_test_suite(pulp-test-mpe-tracker-per-note-management LIBRARIES pulp::midi)

# macOS plan Batch G — Wavetable oscillator (item 2.6)
pulp_add_test_suite(pulp-test-wavetable LIBRARIES pulp::signal)

# macOS plan Batch C — Resampler (item 1.7)
pulp_add_test_suite(pulp-test-resampler LIBRARIES pulp::signal)

# macOS plan Batch B — Polyphase IIR half-band filter (item 2.2)
pulp_add_test_suite(pulp-test-halfband-iir LIBRARIES pulp::signal)

# macOS plan Batch G — Generic Synthesiser polyphony (item 2.5)
pulp_add_test_suite(pulp-test-synthesiser LIBRARIES pulp::midi)

# macOS plan Batch C — SampleConverter (item 1.5)
pulp_add_test_suite(pulp-test-sample-converter LIBRARIES pulp::audio)

# macOS plan Batch C — AudioSource hierarchy (item 1.8)
pulp_add_test_suite(pulp-test-audio-source LIBRARIES pulp::audio)

# macOS plan Batch A — Foundations
pulp_add_test_suite(pulp-test-dc-blocker LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-denormal LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-normalisable-range LIBRARIES pulp::state)
pulp_add_test_suite(pulp-test-adsr LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-high-resolution-timer LIBRARIES pulp::runtime)

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
pulp_add_test_suite(pulp-test-platform-maturity LIBRARIES pulp::view)

# Audio bridge tests
pulp_add_test_suite(pulp-test-audio-bridge LIBRARIES pulp::view)

# Widget tests
pulp_add_test_suite(pulp-test-widgets LIBRARIES pulp::view)

# Widget tests — Label cluster (P5-3 follow-up — extracted from
# test_widgets.cpp). Label intrinsic_width / intrinsic_height /
# line-height multiplier (#76) / line_clamp / measured_height under
# bounded width / baseline_y from text metrics / vertical text
# direction / letter_spacing glyph counting (#928 + #1407 + #76).
pulp_add_test_suite(pulp-test-widgets-label LIBRARIES pulp::view)

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
    # PULP_INSPECTOR_NO_LAUNCH: the source-jump tests exercise the J
    # hotkey, whose handler resolves with dry_run=false. Without this
    # guard the test would spawn a real `open vscode://file/...`, popping
    # a macOS open-confirmation dialog. The env var makes launch_editor_url()
    # a no-op. The test file also sets it in-process so it stays safe when
    # the binary is run directly, outside CTest.
    catch_discover_tests(pulp-test-inspector
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Inspector test split (Phase-5 oversized-test-file refactor): four
    # concern-scoped sibling TUs carved verbatim out of test_inspector.cpp
    # — GPU pass attribution, source-jump, drift/reconcile, atlas viewer.
    # Each mirrors pulp-test-inspector's exact registration (own executable
    # + catch_discover_tests, same libraries, same NO_LAUNCH env guard).
    add_executable(pulp-test-inspector-gpu-passes test_inspector_gpu_passes.cpp)
    target_link_libraries(pulp-test-inspector-gpu-passes PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-gpu-passes
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-source-jump test_inspector_source_jump.cpp)
    target_link_libraries(pulp-test-inspector-source-jump PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-source-jump
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-drift-reconcile test_inspector_drift_reconcile.cpp)
    target_link_libraries(pulp-test-inspector-drift-reconcile PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-drift-reconcile
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-atlas-viewer test_inspector_atlas_viewer.cpp)
    target_link_libraries(pulp-test-inspector-atlas-viewer PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-atlas-viewer
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-server test_inspector_server.cpp)
    target_link_libraries(pulp-test-inspector-server PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-server
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # P11-5 (#2647): three more concern-scoped sibling TUs carved verbatim out
    # of test_inspector.cpp to bring the 3,126-line parent under the 2,000-line
    # target (now 1,859). Same registration as pulp-test-inspector.
    add_executable(pulp-test-inspector-domains test_inspector_domains.cpp)
    target_link_libraries(pulp-test-inspector-domains PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-domains
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-field-edit test_inspector_field_edit.cpp)
    target_link_libraries(pulp-test-inspector-field-edit PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-field-edit
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-eyedropper test_inspector_eyedropper.cpp)
    target_link_libraries(pulp-test-inspector-eyedropper PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-eyedropper
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Phase 0b PR-A: TweakStore + Inspector.applyTweak protocol surface.
    # planning/2026-05-18-inspector-direct-manipulation-roadmap.md
    add_executable(pulp-test-tweak-store test_tweak_store.cpp)
    target_link_libraries(pulp-test-tweak-store PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-tweak-store)

    # Phase 5.3: editor URI plumbing for the future source-jump action.
    # Pure config / format-helper / protocol — no overlay / GPU surface,
    # but lives under the same PULP_ENABLE_GPU guard as the rest of
    # pulp-inspect's tests so it links against the same library.
    # planning/2026-05-19-inspector-phase5-source-jump-spike.md
    add_executable(pulp-test-editor-url test_editor_url.cpp)
    target_link_libraries(pulp-test-editor-url PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-editor-url
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
endif()

# Widget bridge tests
pulp_add_test_suite(pulp-test-widget-bridge LIBRARIES pulp::view)

# Widget bridge — runtime-import handlers (pulp #468 follow-up).
# Split out of test_widget_bridge.cpp in the Phase 5 P5-1 first cut so
# the 14k-line god-test file shrinks toward per-surface modules.
pulp_add_test_suite(pulp-test-widget-bridge-runtime-import LIBRARIES pulp::view)

# Widget bridge — Canvas2D surface (P5-1 continuation).
# Covers canvasSetTransform / canvasClip / canvasGlobalCompositeOperation
# (issue-896), canvasMeasureText / canvasSetLineDash / canvasDrawImage /
# canvasGetImageData / canvasPutImageData (issue-916), and pulp #1899 /
# #1901 4-arg canvasFillText prior-state preservation.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d LIBRARIES pulp::view)

# Widget bridge — Yoga layer (P5-1 follow-up). Three coherent clusters:
# (1) pulp #1434 rn-batch-C dimension percent strings (width/height +
# min/max via Yoga's percent API), (2) pulp #1545 flexBasis%
# promotion (basis "NN%" + "flex 1 1 NN%" decomposition), (3) pulp
# #1434 rn-batch-B yoga value-aliasing (flexDirection /
# justifyContent / alignItems / alignSelf / order / flexWrap value
# translations).
pulp_add_test_suite(pulp-test-widget-bridge-yoga LIBRARIES pulp::view)

# Widget bridge — Canvas2D Wave 2 cheap-wiring surface (P5-1 follow-up).
# Covers canvas2d/fill + canvas2d/clip evenodd, canvas2d/roundRect
# 4-corner radii, canvas2d/ellipse rotation, canvas2d/strokeText
# dedicated-command routing — the five Wave 2 partial → supported
# compat.json entries.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-wave2 LIBRARIES pulp::view)

# Widget bridge — yoga logical-edge fan-out + A4 OOS pins (P5-1
# follow-up). pulp #1542 yoga logical-edge (marginInline/Block +
# paddingInline/Block + inset shorthand) + pulp #1434 A4 Bundles 2-7
# css NOT-IMPL closure catalog hygiene.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-a4-oos LIBRARIES pulp::view)

# Widget bridge — Wave 2 cheap-wiring (P5-1 follow-up). Bundles the
# Wave 2 canvas2d cheap wiring (DIVERGE → PASS) + Wave 2 css value-
# coverage wiring entries from compat.json's Wave 2 cleanup pass.
pulp_add_test_suite(pulp-test-widget-bridge-wave2-cheap LIBRARIES pulp::view)

# Widget bridge — RN style-prop bridge primitives (P5-1 follow-up,
# pulp #1026). setShadow / setOpacity / setTransform RN-shaped style
# functions flow through bridge into View's slots. Includes RN's
# shadowOpacity-into-color-alpha composition + transform-prop
# aggregation.
pulp_add_test_suite(pulp-test-widget-bridge-rn-style LIBRARIES pulp::view)

# Widget bridge — Tier-4 OOS / perf-hints / interaction misc pins
# (P5-1 follow-up, pulp #1434). Pins the no-op / fallback contract
# for properties Pulp deliberately doesn't paint: 3D transforms,
# generated content, scroll-snap, will-change, contain, touch-action
# secondary keywords. Catalog hygiene tests.
pulp_add_test_suite(pulp-test-widget-bridge-tier4-oos LIBRARIES pulp::view)

# Widget bridge — RN outline cluster (P5-1 follow-up, pulp #1519).
# outlineColor / outlineOffset / outlineStyle / outlineWidth longhands
# + `outline` shorthand decomposition through the RN style shim.
pulp_add_test_suite(pulp-test-widget-bridge-rn-outline LIBRARIES pulp::view)

# Widget bridge — clip-path + mask cluster (P5-1 follow-up, pulp #1515).
# clip-path: inset/circle/polygon/url + shape coordinate parsing;
# mask-image: url + linear-gradient + transforms; mask-size /
# -position / -repeat / -origin / -clip / -composite; mask shorthand.
pulp_add_test_suite(pulp-test-widget-bridge-clip-mask LIBRARIES pulp::view)

# Widget bridge — SVG widgets (P5-1 follow-up). Three clusters:
# pulp #965 SvgPathWidget JS bridge integration + pulp #1416
# SvgRectWidget + SvgLineWidget JS bridge integration + compound-path
# parser regression (Spectr PEAK / AVG / BOTH / OFF analyzer icons).
pulp_add_test_suite(pulp-test-widget-bridge-svg LIBRARIES pulp::view)

# Widget bridge — Wave 5 css.5 audit (P5-1 follow-up). Runtime-path
# coverage for the 49 entries PR #1649 flipped from partial/DIVERGE
# to supported: backgroundPosition / backgroundSize / textShadow /
# border / border-side / borderRadius / per-corner radius / boxShadow
# / opacity / outline / textOverflow / transformOrigin / zIndex /
# backdropFilter / display / overflow / overflow per-axis / and
# many more. 45 TEST_CASEs each exercising JS shim → bridge → View
# slot round-trip.
pulp_add_test_suite(pulp-test-widget-bridge-wave5-css LIBRARIES pulp::view)

# Widget bridge — HTML ARIA + querySelector (P5-1 follow-up, Wave 3
# html bundle). aria-label / role flow into View accessibility slots;
# document.querySelector accepts attribute selectors + combinators +
# :hover / :disabled / :checked / :enabled / :not / :first-child /
# :nth-child / :empty pseudo-classes.
pulp_add_test_suite(pulp-test-widget-bridge-html-aria LIBRARIES pulp::view)

# Widget bridge — CSS animations + transitions (P5-1 follow-up,
# pulp #1434 Phase A2-1). animation-* longhands + shorthand
# decomposition; transition-* longhands + shorthand round-trip
# through the CSS shim and bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-animations LIBRARIES pulp::view)

# Widget bridge — CSS Grid extended surface (P5-1 follow-up, pulp #1434
# Phase A2-2). grid-template-columns / -rows / -areas, grid-column /
# -row / -area placement shorthand, gap longhands + shorthand,
# justify-* / align-* / place-* alignment, repeat() + minmax() +
# fr-unit + auto sizing tokens round-trip through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-grid LIBRARIES pulp::view)

# Widget bridge — animation API cluster (P5-1 follow-up). Bridge ↔
# MotionEngine plumbing for setMotionToken, animate(), the Web
# Animations API surface (Element.animate / KeyframeEffect), motion
# provenance, and pulp-motion-bench harness output. Self-contained
# ~800-line cluster from test_widget_bridge.cpp.
pulp_add_test_suite(pulp-test-widget-bridge-animation LIBRARIES pulp::view)

# Widget bridge — pulp #1434 (cross-surface mega-batch) per-edge margin
# / padding (P5-1 follow-up). marginTop / marginRight / marginBottom /
# marginLeft (and padding counterparts) each route to their own Yoga
# edge enum without cross-contamination through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-per-edge LIBRARIES pulp::view)

# Widget bridge — pulp #1737 RN-OOS-fixup catalog audit tail (P5-1
# follow-up). Material elevation shim, includeFontPadding round-trip,
# pulp #1812 borderCurve squircle paint dispatch, isolation honest
# CSS-subset, and other RN-side OOS catalog hygiene checks.
pulp_add_test_suite(pulp-test-widget-bridge-rn-oos-fixup LIBRARIES pulp::view)

# Widget bridge — Canvas2D bridge-fn cluster (P5-1 follow-up). pulp
# #1434 canvasSetFontFull + pulp #1522 fillRule + pulp #1520
# canvasSetDirection / canvasSetFilter — three closely related
# bridge entry-points that thread JS-side Canvas2D semantics through
# WidgetBridge into the native canvas pipeline.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-bridge-fns LIBRARIES pulp::view)

# Widget bridge — pulp #1543 Yoga borderWidth wiring (P5-1 follow-up).
# Pins YGNodeStyleSetBorder integration with Yoga 3.x default box-sizing
# (border-box); the companion content-box test lives in #1516 once
# setBoxSizing is wired.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-border LIBRARIES pulp::view)

# Widget bridge — CSS-misc cluster (P5-1 follow-up). pulp #1434 batch 3
# text-decoration longhands + pulp #1552 line-clamp + background-repeat.
# Two coherent small CSS clusters that together push test_widget_bridge.cpp
# under the 3,000-line P5-1 target.
pulp_add_test_suite(pulp-test-widget-bridge-css-misc LIBRARIES pulp::view)

# Autonomous Spectr regression suite (pulp-internal task #63, follow-up to
# pulp #1792). Composes the six [contract] invariants from
# test_widget_bridge.cpp into a Spectr-shaped mini-scenario so a future
# PR that keeps each unit-level invariant passing but breaks their
# *interaction* still fails first.
pulp_add_test_suite(pulp-test-spectr-regression LIBRARIES pulp::view)

# Typography inheritance — pulp #969 (CSS-style cascade)
pulp_add_test_suite(pulp-test-typography-inheritance LIBRARIES pulp::view)

# Text-overflow ellipsis helper — pulp #1407
pulp_add_test_suite(pulp-test-text-overflow LIBRARIES pulp::view)

# Editor bridge tests (pulp #709 — renderer-agnostic envelope/dispatcher)
pulp_add_test_suite(pulp-test-editor-bridge LIBRARIES pulp::view)

# Input events tests
pulp_add_test_suite(pulp-test-input-events LIBRARIES pulp::view)

# Text editor tests
pulp_add_test_suite(pulp-test-text-editor LIBRARIES pulp::view)

# TextEditor multi-line coverage (audit P3: wrap, click-to-caret in
# wrapped rows, caret_rect pixel positioning, single-vs-multi Enter
# contract). Keeps the original test_text_editor.cpp file unchanged
# so the existing single-line surface stays pinned in isolation.
pulp_add_test_suite(pulp-test-text-editor-multiline LIBRARIES pulp::view)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
pulp_add_test_suite(pulp-test-text-input LIBRARIES pulp::view)

# W3C Layout parity tests (flexbox, box model, visual properties)
pulp_add_test_suite(pulp-test-layout-w3c LIBRARIES pulp::view)

# Visual semantic snapshot harness. This is a custom-main Catch2 binary:
# --self-test runs its focused tests, while --fixture emits a stable JSON
# snapshot for tools/harness/visual/runner.py.
add_executable(pulp-test-visual visual/pulp-test-visual.cpp)
target_link_libraries(pulp-test-visual PRIVATE pulp::view Catch2::Catch2)
add_test(NAME visual-harness-self-test COMMAND pulp-test-visual --self-test)
set_tests_properties(visual-harness-self-test PROPERTIES
    LABELS "visual;harness;yoga"
    TIMEOUT 30)

# Reusable layout-tree parity oracle for design-import live/baked modes.
pulp_add_test_suite(pulp-test-layout-snapshot LIBRARIES pulp::view)

# CanvasWidget tests (JS-driven custom drawing)
pulp_add_test_suite(pulp-test-canvas-widget LIBRARIES pulp::view)

# CanvasWidget NaN/Infinity sanitization cluster (P5-3 follow-up,
# pulp #1387 gap #2). Pins that JS-supplied NaN / ±Inf coords land
# as 0 in the recorded draw command, while finite values pass through.
pulp_add_test_suite(pulp-test-canvas-widget-sanitize LIBRARIES pulp::view)

# CanvasWidget + SkiaCanvas Canvas2D shadow-state cluster (P5-3
# follow-up). Replay + clear + transparent/zero short-circuit
# of sticky Canvas2D shadow* state.
pulp_add_test_suite(pulp-test-canvas-widget-shadow LIBRARIES pulp::view)

# pulp #964 — Canvas2D JS-shim coverage. Drives the full
# web-compat-canvas.js → bridge → CanvasWidget path so a regression
# that drops a save/restore/setTransform/createLinearGradient method
# (and silently aborts FilterBank-style frame renders) gets caught.
pulp_add_test_suite(pulp-test-canvas2d-shim LIBRARIES pulp::view)

# Canvas2D _PulpCanvasMatrix DOMMatrix arithmetic — direct unit tests
# (new coverage for the matrix prelude extracted to
# web-compat-canvas-matrix.js by #2253). Previously only exercised
# indirectly via ctx.getTransform() round-trip tests; this binary
# pins identity construction, mutator chain composition (not
# last-write-wins), rotateSelf degrees-not-radians, multiplySelf
# composition, inverse singular-matrix → NaN+is2D=false detection,
# toJSON honoring actual is2D state (Codex P2 #1754).
pulp_add_test_suite(pulp-test-canvas2d-dommatrix LIBRARIES pulp::view)

# Canvas2D shim — post-Wave-2 coverage (P5-2 follow-up — extracted from
# test_canvas2d_shim.cpp). Bundles #1525 (fillText / strokeText
# maxWidth + glyph cluster handling) + #1521 (arc-as-path
# DIVERGE → PASS) + #1520 (ctx.direction / ctx.filter) + #1526
# (catalog hygiene round-trip) + Wave 4 lineDashOffset re-flush +
# #1527 (getTransform / resetTransform + isPointInPath /
# isPointInStroke).
pulp_add_test_suite(pulp-test-canvas2d-shim-late LIBRARIES pulp::view)

# SvgPathWidget tests (issue-965)
pulp_add_test_suite(pulp-test-svg-path-widget LIBRARIES pulp::view)

# SvgRect + SvgLine widget tests (issue-1416)
pulp_add_test_suite(pulp-test-svg-rect-widget LIBRARIES pulp::view)

# ScrollView tests
pulp_add_test_suite(pulp-test-scroll-view LIBRARIES pulp::view)

# Standalone ScrollBar widget tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-scroll-bar LIBRARIES pulp::view)

# SidePanel slide-in animation tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-side-panel LIBRARIES pulp::view)

# ComboBox dropdown interaction tests
pulp_add_test_suite(pulp-test-combo-dropdown LIBRARIES pulp::view)

# pulp #1148 — generalized overlay-click routing (View::active_overlay_)
pulp_add_test_suite(pulp-test-overlay-routing LIBRARIES pulp::view)

# pulp #1708 — auto-clearing input-focus slot (View::focused_input_)
pulp_add_test_suite(pulp-test-focused-input LIBRARIES pulp::view)

# pulp #1148 (slice b) — auto-claim active_overlay_ from CSS shape
# (`position:absolute` + `z-index >= 10`) or `data-overlay` author hint
# detected by the web-compat layer (web-compat-style-decl.js +
# web-compat-element.js). Uses the real WidgetBridge so the heuristic
# runs against the same prelude stack the runtime ships.
pulp_add_test_suite(pulp-test-web-compat-overlay LIBRARIES pulp::view)

# Panel widget tests (styled containers)
pulp_add_test_suite(pulp-test-panel LIBRARIES pulp::view)

# ComponentDragger — drag-to-move helper for any View (gap-doc P2)
pulp_add_test_suite(pulp-test-component-dragger LIBRARIES pulp::view)

# TooltipWindow — transient floating tooltip near cursor (gap-doc P2)
pulp_add_test_suite(pulp-test-tooltip-window LIBRARIES pulp::view)

# PreferencesPanel — sidebar-of-pages container (gap-doc P3)
pulp_add_test_suite(pulp-test-preferences-panel LIBRARIES pulp::view)

# UI components tests (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox)
pulp_add_test_suite(pulp-test-ui-components LIBRARIES pulp::view)

# GraphEditorView tests
pulp_add_test_suite(pulp-test-graph-editor-view LIBRARIES pulp::view)

# Modal overlay tests
pulp_add_test_suite(pulp-test-modal LIBRARIES pulp::view)

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
    TEST_SPEC "~[network]"
    PROPERTIES LABELS "parser-import")
catch_discover_tests(pulp-test-design-import
    TEST_SPEC "[network]"
    PROPERTIES
        LABELS "parser-import"
        RESOURCE_LOCK "design-import-network")

add_executable(pulp-test-design-import-native-common test_design_import_native_common.cpp)
target_link_libraries(pulp-test-design-import-native-common PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-native-common
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-native-materializer test_design_import_native_materializer.cpp)
target_link_libraries(pulp-test-design-import-native-materializer PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-native-materializer
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-view-core-link test_view_core_link.cpp)
target_link_libraries(pulp-test-view-core-link PRIVATE pulp::view-core Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-core-link
    PROPERTIES LABELS "parser-import")

if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-view-core-window-host-link
        test_view_core_window_host_link.mm)
    target_link_libraries(pulp-test-view-core-window-host-link
        PRIVATE pulp::view-core Catch2::Catch2WithMain)
    target_link_options(pulp-test-view-core-window-host-link PRIVATE
        "LINKER:-force_load,${CMAKE_BINARY_DIR}/core/view/CMakeFiles/pulp-view-core.dir/platform/mac/window_host_mac.mm.o"
    )
    catch_discover_tests(pulp-test-view-core-window-host-link
        PROPERTIES LABELS "parser-import")
endif()

add_executable(pulp-test-design-import-cpp-codegen
    test_design_import_cpp_codegen.cpp
    fixtures/design_import_generated_cpp_fixture.cpp)
target_link_libraries(pulp-test-design-import-cpp-codegen
    PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-cpp-codegen PRIVATE
    PULP_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(pulp-test-design-import-cpp-codegen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
catch_discover_tests(pulp-test-design-import-cpp-codegen
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-benchmark test_design_import_benchmark.cpp)
target_link_libraries(pulp-test-design-import-benchmark
    PRIVATE pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-benchmark PRIVATE
    PULP_DESIGN_IMPORT_BENCH_PATH="$<TARGET_FILE:pulp-design-import-bench>")
add_dependencies(pulp-test-design-import-benchmark pulp-design-import-bench)
catch_discover_tests(pulp-test-design-import-benchmark
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-benchmark-contracts
    test_design_import_benchmark_contracts.cpp)
target_link_libraries(pulp-test-design-import-benchmark-contracts
    PRIVATE pulp::view pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-benchmark-contracts
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-screenshot-parity
    test_design_import_screenshot_parity.cpp)
target_link_libraries(pulp-test-design-import-screenshot-parity
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-screenshot-parity
    PROPERTIES LABELS "parser-import")

# P5-3 follow-up — W3C Design Tokens cluster extracted from
# test_design_import.cpp. Covers parse_w3c_tokens, export_w3c_tokens,
# composite typography/shadow shapes, alias resolution, math
# expressions, group $type inheritance, ir_tokens_to_theme +
# theme_to_ir_tokens round-trips.
add_executable(pulp-test-design-import-w3c-tokens test_design_import_w3c_tokens.cpp)
target_link_libraries(pulp-test-design-import-w3c-tokens PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-w3c-tokens PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-w3c-tokens
    PROPERTIES LABELS "parser-import")

# P5-3 follow-up — React-runtime parser cluster extracted from
# test_design_import.cpp. Covers TSX/runtime React parsers for every
# supported design-tool source: parse_v0_tsx / parse_v0_dev_react,
# parse_figma_make_react, parse_stitch_react, parse_react_native_export,
# parse_pencil_react. Shared contract: parse fixture, materialize via
# host React shim, accept sanitized TSX, reject out-of-matrix surfaces.
add_executable(pulp-test-design-import-react-runtime test_design_import_react_runtime.cpp)
target_link_libraries(pulp-test-design-import-react-runtime PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-react-runtime PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-react-runtime
    PROPERTIES LABELS "parser-import")

# Phase 0a: stable_anchor_id assignment per
# planning/2026-05-18-inspector-direct-manipulation-roadmap.md
add_executable(pulp-test-design-import-anchors test_design_import_anchors.cpp)
target_link_libraries(pulp-test-design-import-anchors PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-anchors
    PROPERTIES LABELS "parser-import")

# Inspector Phase 4a — Lock-to-source, Path A (generated-TSX/JS rewrite).
# Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md
# Proves the tweak -> lock-to-source -> re-import round-trip.
add_executable(pulp-test-lock-to-source test_lock_to_source.cpp)
target_link_libraries(pulp-test-lock-to-source PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-lock-to-source
    PROPERTIES LABELS "parser-import")

# WYSIWYG T3 — ui-preview settle-probe design-viewport sizing. The probe
# algorithm lives in examples/ui-preview/design_viewport_probe.hpp (header-
# only) so it can be tested headlessly without linking the ui-preview app.
add_executable(pulp-test-ui-preview-viewport test_ui_preview_viewport.cpp)
target_link_libraries(pulp-test-ui-preview-viewport PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-ui-preview-viewport
    PROPERTIES LABELS "view")

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

# Phase 4c — token lock-to-source via DESIGN.md rewrite
# (planning/2026-05-18-inspector-direct-manipulation-roadmap.md).
add_executable(pulp-test-token-lock test_token_lock.cpp)
target_link_libraries(pulp-test-token-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-token-lock
    PROPERTIES LABELS "parser-import")

# Inspector Phase 4b — Lock-to-source, Path B (hand-authored JSX/TSX patch).
# Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md (#1308).
# Proves the tweak -> JSX/TSX surgical patch -> formatting-preserving
# round-trip, plus the ambiguous / not-found / too-dynamic failure paths.
add_executable(pulp-test-jsx-lock test_jsx_lock.cpp)
target_link_libraries(pulp-test-jsx-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-jsx-lock
    PROPERTIES LABELS "parser-import")

# Library-backed post-parse widget promotion: <div onClick> / role=button /
# cursor:pointer → button. Closure of pulp-internal task #84 / follow-up on
# #1814. The pass now lives in pulp::view so parser API users and the CLI share
# one normalization path.
add_executable(pulp-test-widget-promotion
    test_widget_promotion.cpp)
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
    TEST_SPEC "~[network]"
    PROPERTIES LABELS "parser-import")
catch_discover_tests(pulp-test-import-design-tool
    TEST_SPEC "[network]"
    PROPERTIES
        LABELS "parser-import"
        RESOURCE_LOCK "design-import-network")

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

# pulp jsx-instrument-import experiment (2026-05-17). Loads a
# pre-compiled JSX bundle from PULP_JSX_BUNDLE and asserts that the
# Claude-style runtime harness can materialize it. See
# planning/2026-05-17-jsx-instrument-import.md.
add_executable(pulp-test-design-import-jsx-runtime test_design_import_jsx_runtime.cpp)
target_link_libraries(pulp-test-design-import-jsx-runtime
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-jsx-runtime
    PROPERTIES LABELS "parser-import")

# Screenshot comparison tests (visual diff for design import validation)
if(APPLE)
    add_executable(pulp-test-screenshot-compare test_screenshot_compare.cpp)
    target_link_libraries(pulp-test-screenshot-compare PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-compare
        PROPERTIES LABELS "parser-import")
endif()

# Parameter attachment tests
pulp_add_test_suite(pulp-test-param-attachment LIBRARIES pulp::view)

# App framework tests (KeyMapping, MenuBar, Toolbar, AppSettings)
pulp_add_test_suite(pulp-test-app-framework LIBRARIES pulp::view)

# Preset manager tests
pulp_add_test_suite(pulp-test-preset-manager LIBRARIES pulp::state)

# Waveform editor tests
pulp_add_test_suite(pulp-test-waveform-editor LIBRARIES pulp::view)

# FastMath approximation tests
pulp_add_test_suite(pulp-test-fast-math LIBRARIES pulp::signal)

# Polynomial/Matrix math tests
pulp_add_test_suite(pulp-test-poly-math LIBRARIES pulp::signal)

# Preset browser UI tests
pulp_add_test_suite(pulp-test-preset-browser LIBRARIES pulp::view)

# Plugin manager panel UI tests (issue #494)
add_executable(pulp-test-plugin-manager-panel test_plugin_manager_panel.cpp)
target_link_libraries(pulp-test-plugin-manager-panel PRIVATE
    pulp::view pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-plugin-manager-panel)

# DSP expansion tests (FIR, Ballistics, LogRamp, ProcessorChain, LookupTable, TPT)
pulp_add_test_suite(pulp-test-dsp-expansion LIBRARIES pulp::signal)

# Interpolator tests (Hermite, Lagrange, windowed-sinc)
pulp_add_test_suite(pulp-test-interpolator LIBRARIES pulp::signal)

# MIDI expansion tests (RPN/NRPN parser, MidiKeyboardState)
pulp_add_test_suite(pulp-test-midi-expansion LIBRARIES pulp::midi)

# Audio process load measurer tests
pulp_add_test_suite(pulp-test-load-measurer LIBRARIES pulp::audio)

# FilterDesign tests
pulp_add_test_suite(pulp-test-filter-design LIBRARIES pulp::signal)

# IIR design tests (Chebyshev I/II, Elliptic)
pulp_add_test_suite(pulp-test-iir-design LIBRARIES pulp::signal)

# TreeView tests
pulp_add_test_suite(pulp-test-tree-view LIBRARIES pulp::view)

# File browser, file tree, and multi-document panel tests
pulp_add_test_suite(pulp-test-file-browser LIBRARIES pulp::view)

# FileChooser widget (macOS plan item 6.2 — builder wrapper over
# pulp::platform::FileDialog with async callback delivery).
pulp_add_test_suite(pulp-test-file-chooser LIBRARIES pulp::view pulp::platform)

# Undo manager tests
pulp_add_test_suite(pulp-test-undo-manager LIBRARIES pulp::state)

# BufferingReader tests
pulp_add_test_suite(pulp-test-buffering-reader LIBRARIES pulp::audio)

# AudioWorkgroup tests
pulp_add_test_suite(pulp-test-workgroup LIBRARIES pulp::audio)

# AudioWorkgroup ↔ AudioDevice wiring (macOS plan item 1.1)
pulp_add_test_suite(pulp-test-audio-workgroup-wiring
    SOURCES test_audio_workgroup_wiring.cpp
    LIBRARIES pulp::audio)

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
pulp_add_test_suite(pulp-test-ios-foundation LIBRARIES pulp::view pulp::platform)

# Identity/UUID tests
pulp_add_test_suite(pulp-test-identity LIBRARIES pulp::runtime)

# WebView tests (requires PULP_BUILD_WEBVIEW — WebViewPanel::create is only compiled when ON)
if(PULP_BUILD_WEBVIEW)
    add_executable(pulp-test-webview test_web_view.cpp)
    target_link_libraries(pulp-test-webview PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-webview)
endif()

# MIDI 2.0 UMP/MPE tests
pulp_add_test_suite(pulp-test-ump-mpe LIBRARIES pulp::midi)

# MPE voice tracker tests
pulp_add_test_suite(pulp-test-mpe-voice-tracker LIBRARIES pulp::midi)

# UMP buffer + MIDI 1.0 <-> UMP conversion + UMP-aware MpeVoiceTracker
pulp_add_test_suite(pulp-test-ump-buffer-conversion LIBRARIES pulp::format)

# MPE buffer + Processor sidecar tests
pulp_add_test_suite(pulp-test-mpe-buffer LIBRARIES pulp::format)

# MPE synth voice helpers (voice, allocator, glide detector)
pulp_add_test_suite(pulp-test-mpe-synth-voice LIBRARIES pulp::midi)

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

# P11-5 (#2647): SignalGraph tests carved out of test_host.cpp to bring the
# 2,190-line parent under 2,000 (now 852). No CLAP fixture needed — these
# exercise pure graph routing/topology, not plugin loading.
add_executable(pulp-test-host-signal-graph test_host_signal_graph.cpp)
target_link_libraries(pulp-test-host-signal-graph PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-host-signal-graph)

# Item 4.5 — ExtensionsVisitor. Pure-header pattern test, no plugin
# loading required; uses lightweight mock slots to exercise dispatch.
add_executable(pulp-test-extensions-visitor test_extensions_visitor.cpp)
target_link_libraries(pulp-test-extensions-visitor PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-extensions-visitor)

# Item 4.2 — opt-in real-plugin integration runner. Builds only when
# PULP_REAL_PLUGIN_TESTS=ON. Even when built, individual TEST_CASEs SKIP
# (via WARN + return) when their fixture binary is not on disk, so the
# binary is safe to run on machines that haven't populated the cache.
# Drive the cache with: python3 tools/scripts/fetch_real_plugins.py
if(PULP_REAL_PLUGIN_TESTS)
    add_executable(pulp-test-real-plugins integration/real_plugin_runner.cpp)
    target_link_libraries(pulp-test-real-plugins
        PRIVATE pulp::host Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-real-plugins PRIVATE
        PULP_REAL_PLUGINS_TOML="${CMAKE_CURRENT_SOURCE_DIR}/integration/real_plugins.toml")
    catch_discover_tests(pulp-test-real-plugins)
endif()

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
pulp_add_test_suite(pulp-test-graph-serializer LIBRARIES pulp::host)

# NetworkServiceDiscovery backend-dispatch tests (#302)
pulp_add_test_suite(pulp-test-network-service-discovery LIBRARIES pulp::events)

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
pulp_add_test_suite(pulp-test-design-tool-layout LIBRARIES pulp::view pulp::state)

add_executable(pulp-test-design-debug-contracts test_design_debug_contracts.cpp)
target_link_libraries(pulp-test-design-debug-contracts PRIVATE
    pulp::view
    pulp::state
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-debug-contracts)

# AI Designer: style pack tests
pulp_add_test_suite(pulp-test-style-pack LIBRARIES pulp::view)

# Removed: pulp-test-token-diff (test_token_diff.cpp) and pulp-test-showcase
# (test_showcase.cpp) were placeholder TEST_CASE("...") { REQUIRE(true); }
# stubs for design-tool C++ classes that never shipped. The design tool
# landed as JS via core/view/js/web-compat* and widget_bridge's
# applyTokenDiff JS binding; there is no C++ surface to verify here.
# Removed in PR #290/#357 follow-up; file a new focused test if a C++
# token-diff or Showcase class is ever added.

# WindowManager multi-window framework tests (Phase 6)
pulp_add_test_suite(pulp-test-window-manager LIBRARIES pulp::view)

# Multi-window rendering fairness + per-window RenderLoop isolation
# (planning §6.6 — macOS-plan item 6.6). Mirrors pulp-test-render-loop:
# compiles render_loop.cpp directly with PULP_RENDER_LOOP_FORCE_TIMER=1 so
# the test runs headlessly on every host (GPU off / no native vsync source).
add_executable(pulp-test-multi-window
    test_multi_window.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/render_loop.cpp)
target_compile_definitions(pulp-test-multi-window PRIVATE
    PULP_RENDER_LOOP_FORCE_TIMER=1)
target_include_directories(pulp-test-multi-window PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include
    ${CMAKE_SOURCE_DIR}/core/render/src)
target_link_libraries(pulp-test-multi-window PRIVATE
    pulp::view
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-multi-window)

# ── Phase 9: Theme management, widgets, asset system ────────────────────────

# Theme contrast utilities (WCAG AA)
pulp_add_test_suite(pulp-test-theme-contrast LIBRARIES pulp::view)

# Theme preset library (38 tweakcn + derivation)
pulp_add_test_suite(pulp-test-theme-presets LIBRARIES pulp::view)

# OS appearance tracking
pulp_add_test_suite(pulp-test-appearance SOURCES test_appearance_tracker.cpp LIBRARIES pulp::view)

# Splash screen lifecycle and paint behavior
pulp_add_test_suite(pulp-test-splash-screen LIBRARIES pulp::view)

# New widgets (EqCurve, MidiKeyboard, ColorPicker, FileDropZone, SplitView, PropertyList, Breadcrumb)
pulp_add_test_suite(pulp-test-phase9-widgets LIBRARIES pulp::view)

pulp_add_test_suite(pulp-test-property-list LIBRARIES pulp::view)

# Focused SplitView and ConcertinaPanel coverage
pulp_add_test_suite(pulp-test-view-layout-widgets LIBRARIES pulp::view)

# Asset manager and resource system
pulp_add_test_suite(pulp-test-asset-manager LIBRARIES pulp::view)

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
        "LINKER:-force_load,${CMAKE_BINARY_DIR}/core/view/CMakeFiles/pulp-view-core.dir/platform/mac/window_host_mac.mm.o"
    )
    catch_discover_tests(pulp-test-mac-perform-key-equivalent)
endif()
