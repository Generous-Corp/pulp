# Pulp test suite — Catch2

include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpTestSuite.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dsp_rt_contract_tests.cmake")
find_package(Python3 COMPONENTS Interpreter QUIET)
# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

if(Python3_Interpreter_FOUND)
    # Reskinnability ratchet: fail on a NEW hardcoded theme colour.
    add_test(NAME token-coverage-ratchet COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/token_coverage_check.py")

    # Token-key correctness: fail on resolve_color("typo.key", …) that isn't a
    # real theme token (silently renders the hardcoded fallback, breaks reskin).
    add_test(NAME token-key-correctness COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/token_key_check.py")

    # Fidelity harness: pure-Python diff-core self-test (always runs) +
    # the end-to-end gallery visual regression (skips=77 without binary/Pillow).
    add_test(NAME gallery-diff-selftest
        COMMAND ${Python3_EXECUTABLE} -m unittest test_gallery_visual_diff
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/scripts")
    add_test(NAME gallery-visual-regression COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/gallery_regression.py"
        --binary "${CMAKE_BINARY_DIR}/examples/widget-gallery/pulp-widget-gallery"
        --golden-dir "${CMAKE_SOURCE_DIR}/assets/design-system/ink-signal/reference/gallery")
    set_tests_properties(gallery-visual-regression PROPERTIES SKIP_RETURN_CODE 77)

    # Per-primitive visual regression — catches widget-fidelity drift vs the
    # Figma-converged native renders (skips=77 without binary/Pillow/backend).
    add_test(NAME component-visual-regression COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/component_regression.py"
        --binary "${CMAKE_BINARY_DIR}/examples/component-shots/pulp-component-shots"
        --golden-dir "${CMAKE_SOURCE_DIR}/assets/design-system/ink-signal/reference/components")
    set_tests_properties(component-visual-regression PROPERTIES SKIP_RETURN_CODE 77)

    if(PULP_ENABLE_SCENE3D)
        set(PULP_SCENE3D_CMAKE_EXPECT on)
    else()
        set(PULP_SCENE3D_CMAKE_EXPECT off)
    endif()
    add_test(NAME scene3d-cmake-boundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_cmake_boundary.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --expect "${PULP_SCENE3D_CMAKE_EXPECT}")
    set_tests_properties(scene3d-cmake-boundary PROPERTIES
        PASS_REGULAR_EXPRESSION
            "scene3d_cmake_boundary_verified=${PULP_SCENE3D_CMAKE_EXPECT}")
    add_test(NAME scene3d-cmake-boundary-negative-contract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_cmake_boundary_contract.py"
            --boundary-verifier
            "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_cmake_boundary.py"
            --build-dir "${CMAKE_BINARY_DIR}")
    set_tests_properties(scene3d-cmake-boundary-negative-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "scene3d_cmake_boundary_contract_case=valid-current-${PULP_SCENE3D_CMAKE_EXPECT}.*scene3d_cmake_boundary_contract_case=expect-mismatch.*scene3d_cmake_boundary_contract_case=enabled-missing-build-path.*scene3d_cmake_boundary_contract_case=enabled-missing-link-file.*scene3d_cmake_boundary_contract_case=valid-enabled-gpu-off.*scene3d_cmake_boundary_contract_case=enabled-gpu-off-render-target-present.*scene3d_cmake_boundary_contract_case=valid-disabled.*scene3d_cmake_boundary_contract_case=disabled-build-path-present.*scene3d_cmake_boundary_contract_case=disabled-link-file-present.*scene3d_cmake_boundary_contract_case=disabled-target-present.*scene3d_cmake_boundary_contract_verified=true")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/refactor_baseline_tests.cmake")
endif()
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
        # 2700s (45 min) outer budget — configure-only on a warm cache
        # finishes in ~1 min, but the default-on .appex + HostApp build
        # (PULP_IOS_AUV3_SMOKE_BUILD=1, the post-2026-05-27 default)
        # takes 9–10 minutes on cold caches, on top of a 530s configure
        # pass that runs mbedtls / Highway / Yoga FetchContent unpacks
        # before Xcode finishes generating. The shell script's internal
        # timeouts are tighter (600s configure + 1500s build).
        TIMEOUT 2700
        # Tagged `slow` for fast-CI exclusion: a fresh-cache
        # configure spends ~3 minutes downloading + unpacking iOS-leg
        # FetchContent sources before the actual smoke fires. Full-CI
        # (push to main / nightly / workflow_dispatch) still runs it.
        LABELS slow)

    # iOS HostApp link-time regression test. Companion to the AUv3
    # configure smoke above; this one always builds the HostApp .app
    # and asserts the produced bundle has a real Info.plist + Mach-O
    # executable + embedded .appex. Catches "pulp-view-core does not
    # link on iOS" regressions like the 2026-05-26 walkthrough that
    # exposed missing pulp::audio and unguarded pulp::host includes.
    add_test(NAME cmake-ios-hostapp-links
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_ios_hostapp_links.sh
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(cmake-ios-hostapp-links PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 2700
        LABELS slow)
    # iOS HostApp/AUv3 bundle-id containment guard (configure-only, fast):
    # rejects sibling or host-equal extension ids that only fail later at
    # `simctl install`. See cmake/test_ios_hostapp_bundle_guard.sh.
    add_test(NAME cmake-ios-hostapp-bundle-guard
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_ios_hostapp_bundle_guard.sh
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(cmake-ios-hostapp-bundle-guard PROPERTIES
        LABELS "cmake;ios;auv3" TIMEOUT 60)
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
# pulp_add_binary_data Python encoder smoke — guards the fix
# (legacy CMake hex loop was O(n²) and pinned cmake configure for 10–22
# minutes on a 1 MB asset). Verifies the encoder's output ABI, mtime
# advance on every run (required for `add_custom_command` build-system
# correctness), and regeneration on input change. Runs under `cmake -P`
# so it works on every platform without building the Pulp tree.
add_test(NAME cmake-pulp-add-binary-data-encoder
    COMMAND ${CMAKE_COMMAND} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_add_binary_data_encoder.cmake)
set_tests_properties(cmake-pulp-add-binary-data-encoder PROPERTIES
    # `slow`: runs `cmake -P` which spawns a CMake interpreter
    # to encode a synthetic 1 MB asset; consistently 4-5 sec wall on
    # all three platforms. Excluded from PR fast-CI.
    LABELS "cmake;binary-data;issue-898;slow"
    TIMEOUT 60)
# Windows FetchContent base-dir smoke — guards the WebGPU prebuilt
# subbuild path from exceeding MSBuild's MAX_PATH limit in deep Actions
# workspaces. Runs under `cmake -P`, so it does not need a Windows host.
add_test(NAME cmake-fetchcontent-base-dir
    COMMAND ${CMAKE_COMMAND} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_fetchcontent_base_dir.cmake)
set_tests_properties(cmake-fetchcontent-base-dir PROPERTIES
    LABELS "cmake;fetchcontent;windows;issue-4039"
    TIMEOUT 30)
# Install-layout regression: when the SDK is
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
    # `slow`: runs `cmake --install` from this build into a
    # temp prefix and inspects the staged layout. Cheap on warm builds
    # but pays a configure-amortised cost — pull it out of PR fast-CI.
    LABELS "cmake;binary-data;issue-905;slow"
    TIMEOUT 120)
# Install-layout regression for format helper sources used by
# PulpAuv3.cmake / PulpPluginFormats.cmake from _PULP_FORMAT_SOURCE_DIR.
# Without au_view_controller_mac.mm in the installed SDK, downstream
# pulp_add_plugin(FORMATS AUv3 ...) builds on macOS fail with a
# missing-source error. The test keeps the installed SDK's AUv3 helper
# source layout aligned with the source-tree build.
# It runs against the staged install tree, not the source checkout, so
# missing install rules fail before a consumer project sees them.
add_test(NAME cmake-pulp-install-format-sources
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_install_format_sources.cmake)
set_tests_properties(cmake-pulp-install-format-sources PROPERTIES
    LABELS "cmake;sdk;auv3;chainer-cross;slow"
    TIMEOUT 120)
# PULP_REQUIRE_GPU_FOR_SDK gate smoke. Two full configures
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
# PULP_ENABLE_SWIFT is intentionally retained only as a compatibility/reserved
# option. ON must produce a clear configure warning rather than silently doing
# nothing or implying Swift is globally enabled.
if(UNIX)
    add_test(NAME cmake-swift-option-reserved
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_swift_option_reserved.sh
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(cmake-swift-option-reserved PROPERTIES
        LABELS "cmake;swift;config;slow"
        TIMEOUT 600)
endif()
# retry_git_clone unit test: a partial clone target is
# scrubbed between retries so attempt 2+ re-runs the fetch. Same UNIX block runs
# the ensure_signing_ready.sh doctor test (PATH-shims security/xcrun, hermetic).
if(UNIX)
    add_test(NAME retry-git-clone
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_retry_git_clone.sh)
    set_tests_properties(retry-git-clone PROPERTIES TIMEOUT 30)
    add_test(NAME ensure-signing-ready
        COMMAND bash ${CMAKE_SOURCE_DIR}/tools/scripts/test_ensure_signing_ready.sh)
    set_tests_properties(ensure-signing-ready PROPERTIES TIMEOUT 60)
endif()
# Launcher unit test: tools/mcp/pulp-mcp-launcher resolves the
# source build / $PATH fallback / diagnostic. Hermetic (tempdir), no real binary.
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
    # Visual-fidelity diff harness unit + integration tests (PIL-based). The
    # tool (tools/import-design/fidelity_diff.py) measures how close an
    # imported + rendered design is to its captured Figma references. The
    # suite skips itself (exit 77) when Pillow is not installed, so it stays
    # green on PIL-less interpreters. Tagged `slow`: the integration
    # tests do pixel-scan image IO over the checked-in fixtures.
    add_test(NAME import-fidelity-diff
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_import_fidelity_diff.py)
    set_tests_properties(import-fidelity-diff PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "import;fidelity;slow"
        TIMEOUT 120)

    # Golden re-import regression tool (tools/import-validation/golden_regression.py)
    # — self-test for its structural edge map. Guards the uint8-wraparound fix
    # (a 255->0 dark-on-light edge must not vanish). Exits 77 (skips) when numpy
    # is unavailable.
    add_test(NAME golden-regression-selftest
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/import-validation/golden_regression.py
                --selftest)
    set_tests_properties(golden-regression-selftest PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "import;fidelity"
        TIMEOUT 30)

    # Labeled comparison-montage helper (tools/import-design/montage.py).
    # Builds titled reference-vs-render(s) montages (labels ON by default).
    # PIL-based; exits 77 (skips) on a PIL-less interpreter.
    add_test(NAME import-montage
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_import_montage.py)
    set_tests_properties(import-montage PROPERTIES
        SKIP_RETURN_CODE 77
        LABELS "import;fidelity"
        TIMEOUT 60)

    # Headless Figma REST exporter — font-capture + content-hash unit test.
    add_test(NAME figma-rest-export
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_figma_rest_export.py)
    set_tests_properties(figma-rest-export PROPERTIES
        LABELS "import" TIMEOUT 30)
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

    # inject-claude-prefs.sh — the SessionStart hook that reads
    # claude.send_user_file from ~/.pulp/config.toml and injects the
    # SendUserFile preference. Hermetic: each case points PULP_CONFIG_FILE
    # at a staged config. Like the cli hook it must always exit 0.
    add_test(NAME inject-claude-prefs-hook
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_inject_claude_prefs_hook.sh)
    set_tests_properties(inject-claude-prefs-hook PROPERTIES TIMEOUT 15)
endif()

# install.sh contract: install CLI + matching SDK in one
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

# Window-only capture invariants: spectr-roundtrip.sh must not
# silently fall back to full-screen
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

# Workspace freshness guard — refuse to run
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

# Debug-SDK perf-killer guard smoke. Hermetic: runs the guard against a
# tempdir-fabricated SDK install,
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

# Android end-to-end smoke. Runs the tools/scripts/android_smoke.sh
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

# Validation contract tests — schema and reality snapshot
add_executable(pulp-test-validation-contract test_validation_contract.cpp)
target_link_libraries(pulp-test-validation-contract PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-validation-contract)

# WAMv2 Processor->WASM bridge: native coverage of the -fno-exceptions bridge TU (compiled directly; links real pulp libs; PulpWam.cmake's lib target can replace this later).
add_executable(pulp-test-wam-adapter test_wam_adapter.cpp ${CMAKE_SOURCE_DIR}/core/format/src/wasm/wam_adapter.cpp)
target_link_libraries(pulp-test-wam-adapter PRIVATE Catch2::Catch2WithMain pulp::format pulp::state pulp::runtime pulp::events pulp::midi pulp::audio)
catch_discover_tests(pulp-test-wam-adapter)
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

# CLI import EMIT materialisation — manifest parse, write-plan, the clean-room
# output denylist scan (all pure), plus an end-to-end shell-out through a mock
# emit responder. Links the pure import-emit logic directly.
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

# CLI import IMPORTER_TERMS accept-gate + provenance PR-check. Links the pure
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
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp
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
    ${CMAKE_SOURCE_DIR}/tools/cli/cli_common.cpp ${CMAKE_SOURCE_DIR}/tools/cli/shell_quote.cpp
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

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/content_workflow_tests.cmake")

# CLI package registry: pure local tests for registry parsing,
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
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_analyzer_tests.cmake")
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

# Child process tests
if(WIN32)
    pulp_add_test_suite(pulp-test-child-process
        LIBRARIES pulp::platform
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-child-process LIBRARIES pulp::platform)
endif()

# Progress parser tests
pulp_add_test_suite(pulp-test-progress-parser LIBRARIES pulp::platform)

# Stream tests (unified I/O interface)
pulp_add_test_suite(pulp-test-stream LIBRARIES pulp::runtime)

# Generic model registry/store (MM-PR1)
pulp_add_test_suite(pulp-test-model-store LIBRARIES pulp::runtime)

# Streaming model downloader (MM-PR2) — uses a local httplib server fixture.
# Link pulp-cpp-httplib so this TU compiles httplib.h with the SAME
# CPPHTTPLIB_MBEDTLS_SUPPORT macro as pulp-runtime (else httplib's inline
# functions ODR-fold across the two layouts → corrupt mutex → EINVAL).
pulp_add_test_suite(pulp-test-model-download LIBRARIES pulp::runtime pulp-cpp-httplib)

# ModelManager view (MM-PR3) — needs the view stack (pulp::view) for render_to_png
add_executable(pulp-test-model-manager-view test_model_manager_view.cpp)
target_link_libraries(pulp-test-model-manager-view PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-model-manager-view)

# Native-component core ABI contract tests. Always built (no Rust toolchain
# needed) — one case per forward-compatibility decision (1-12) so the C ABI
# shape can't silently drift. The Rust round-trip + RT-safety self-test is the
# separate opt-in lane below (PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS).
pulp_add_test_suite(pulp-test-native-core-ffi
    SOURCES test_native_core_ffi.cpp
    LIBRARIES pulp::native-components)

# NativeCoreProcessor seam + RT-safety harnesses; no Rust required.
pulp_add_test_suite(pulp-test-native-core-processor
    SOURCES test_native_core_processor.cpp test_rt_safety.cpp test_signal_graph_rt_safety.cpp $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp> $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>
    LIBRARIES pulp::format ${CMAKE_DL_LIBS}
    COMPILE_DEFINITIONS $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)

# Public node ABI (pulp_node_v1) conformance — always built, no Rust. Pins the
# ABI shape, version negotiation, and a C node's full lifecycle.
pulp_add_test_suite(pulp-test-pulp-node-v1
    SOURCES test_pulp_node_v1.cpp
    LIBRARIES pulp::native-components pulp::runtime)

# Signed node-pack loader. Build a real loadable node MODULE and a
# test that signs its manifest (Ed25519) then verifies trusted load + every
# rejection path (untrusted/bad-sig/hash-mismatch/abi-mismatch/missing). Desktop
# only: pulp::host (and dlopen) is compiled out on iOS.
if(TARGET pulp::host)
    add_library(pulp-test-node-pack-module MODULE
        fixtures/native-components/node_pack_test_module.cpp)
    target_link_libraries(pulp-test-node-pack-module PRIVATE pulp::native-components)
    # Only the explicitly-exported pulp_node_v1_entry should be visible.
    set_target_properties(pulp-test-node-pack-module PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    pulp_add_test_suite(pulp-test-node-pack
        SOURCES test_node_pack.cpp
        LIBRARIES pulp::host pulp::native-components pulp::runtime)
    add_dependencies(pulp-test-node-pack pulp-test-node-pack-module)
    target_compile_definitions(pulp-test-node-pack PRIVATE
        PULP_NODE_PACK_MODULE_PATH="$<TARGET_FILE:pulp-test-node-pack-module>")
endif()

# Opt-in Rust lane: build a Cargo staticlib reference core (with an RT-checking
# global allocator) and link the FFI round-trip + RT-safety death tests. OFF by
# default, so default builds need no Rust toolchain. The death tests fork() a
# child that deliberately allocates inside a no-alloc scope and assert it is
# trapped — never run in the Catch2 parent (Unix-only; the lane is desktop).
if(PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS AND NOT WIN32)
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpCargoStaticlib.cmake)
    pulp_add_cargo_staticlib(
        NAME pulp-noop-rust-core
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/noop-rust-core/Cargo.toml
        LIB_NAME pulp_noop_rust_core
        FEATURES pulp_rt_check_allocator
        PROFILE dev)
    pulp_add_test_suite(pulp-test-rust-dsp-ffi
        SOURCES test_rust_dsp_ffi.cpp native_components/rt_intercept_test_support.cpp
        LIBRARIES pulp::native-components pulp::runtime pulp-noop-rust-core)

    # Real Rust gain DSP core driven through the C++
    # NativeCoreProcessor adapter — proves native audio end-to-end.
    pulp_add_cargo_staticlib(
        NAME pulp-gain-rust-core
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/gain-rust-core/Cargo.toml
        LIB_NAME pulp_gain_rust_core
        PROFILE dev)
    pulp_add_test_suite(pulp-test-rust-dsp-processor
        SOURCES test_rust_dsp_processor.cpp
        LIBRARIES pulp::format pulp-gain-rust-core)

    # Rust node implementing the public pulp_node_v1 ABI, proving a C
    # node and a Rust node load through the same contract.
    pulp_add_cargo_staticlib(
        NAME pulp-node-rust
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/native-components/node-rust/Cargo.toml
        LIB_NAME pulp_node_rust
        PROFILE dev)
    pulp_add_test_suite(pulp-test-pulp-node-v1-rust
        SOURCES test_pulp_node_v1_rust.cpp
        LIBRARIES pulp::native-components pulp-node-rust)
endif()

# #334: AudioFocusRegistry — cross-platform observer for OS audio-focus
# signals (Android AudioManager.OnAudioFocusChangeListener,
# AVAudioSession interruptions). Pure C++ unit tests — no platform deps.
pulp_add_test_suite(pulp-test-audio-focus LIBRARIES pulp::audio)
# `pulp audio render` arg grammar + the device-free block stepper (no PluginSlot).
pulp_add_test_suite(pulp-test-cmd-audio-render SOURCES test_cmd_audio_render.cpp ${CMAKE_SOURCE_DIR}/tools/cli/cmd_audio_render_parse.cpp LIBRARIES pulp::audio pulp::midi pulp::state)
# #244: zero-fill helper for the Oboe input-frame-read short-read path.
# Header-only math, so no library link required beyond Catch2.
pulp_add_test_suite(pulp-test-frame-fill LIBRARIES pulp::audio)
# #3327 L4: MonotonicMidiClock — seconds-since-open MIDI input timestamps,
# the cross-platform helper that replaced ALSA's hardcoded 0.0 stamps.
pulp_add_test_suite(pulp-test-midi-monotonic-clock LIBRARIES pulp::midi)

# BLE-MIDI 1.0 packet codec + cross-platform BleMidiCentral factory contract. The
# CoreBluetooth backend's live discovery / pairing path is exercised
# manually (requires a real adapter + a paired peripheral); these
# tests pin the deterministic codec + the stub central behaviour.
# Keeping the codec tests host-independent lets every CI lane validate
# timestamp/running-status handling without Bluetooth hardware.
pulp_add_test_suite(pulp-test-ble-midi LIBRARIES pulp::midi)

# UmpSession + UmpEndpoint + VirtualUmpEndpoint headless tests for the
# cross-platform session + virtual-endpoint
# registry. The macOS .mm path is exercised opportunistically; the
# suite runs on every platform via the virtual-endpoint half.
pulp_add_test_suite(pulp-test-ump-session LIBRARIES pulp::midi)

# AsyncStream + portable-exceptions macro tests
pulp_add_test_suite(pulp-test-async-stream LIBRARIES pulp::runtime)
pulp_add_test_suite(pulp-test-exceptions LIBRARIES pulp::runtime)

# Network Stream tests
pulp_add_test_suite(pulp-test-network-stream
    LIBRARIES pulp::runtime pulp-cpp-httplib
    PROPERTIES RESOURCE_LOCK network-stream)

# MessageChannel tests
pulp_add_test_suite(pulp-test-memory-message-channel LIBRARIES pulp::runtime)
pulp_add_test_suite(pulp-test-websocket-channel LIBRARIES pulp::runtime)

pulp_add_test_suite(pulp-test-osc-channel LIBRARIES pulp::osc)

pulp_add_test_suite(pulp-test-json-rpc LIBRARIES pulp::runtime)

# Validation harness tests
pulp_add_test_suite(pulp-test-validation-harness LIBRARIES pulp::format)

# Reusable validation assertion helpers (validation_assertions.hpp)
pulp_add_test_suite(pulp-test-validation-assertions LIBRARIES pulp::format)

# DSP hot reload — build-fingerprint gate (Phase 0)
pulp_add_test_suite(pulp-test-build-fingerprint LIBRARIES pulp::format)

# DSP hot reload — RT-safe processor hot-swap slot (Phase 0)
pulp_add_test_suite(pulp-test-hotswap-slot LIBRARIES pulp::format)

# DSP hot reload — parameter-contract equivalence checker (Phase 0)
pulp_add_test_suite(pulp-test-param-contract LIBRARIES pulp::format)

# DSP hot reload — dynamic-library handle + leak policy (Phase 0). Builds a tiny
# MODULE fixture and dlopens it by absolute path (RELOAD_PROBE_PATH).
add_library(pulp-reload-probe MODULE fixtures/reload_probe.cpp)
set_target_properties(pulp-reload-probe PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON)
pulp_add_test_suite(pulp-test-reload-library LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-library pulp-reload-probe)
target_compile_definitions(pulp-test-reload-library PRIVATE
    RELOAD_PROBE_PATH="$<TARGET_FILE:pulp-reload-probe>")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-library PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload — verify-before-commit transaction, end to end (Phase 1). Two
# logic-library fixtures (compatible / incompatible parameter contract) are
# built as MODULEs and dlopen'd by the transaction; the test asserts the
# fingerprint + contract gates, the live swap, and parameter-state continuity.
add_library(pulp-reload-logic-compat MODULE fixtures/reload_logic_compatible.cpp)
add_library(pulp-reload-logic-incompat MODULE fixtures/reload_logic_incompatible.cpp)
add_library(pulp-reload-logic-throwing MODULE fixtures/reload_logic_throwing.cpp)
foreach(_pulp_reload_logic pulp-reload-logic-compat pulp-reload-logic-incompat
                           pulp-reload-logic-throwing)
    set_target_properties(${_pulp_reload_logic} PROPERTIES
        PREFIX "" POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${_pulp_reload_logic} PRIVATE pulp::format)
endforeach()
pulp_add_test_suite(pulp-test-reload-transaction LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-transaction
    pulp-reload-logic-compat pulp-reload-logic-incompat pulp-reload-logic-throwing)
target_compile_definitions(pulp-test-reload-transaction PRIVATE
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    RELOAD_LOGIC_THROWING="$<TARGET_FILE:pulp-reload-logic-throwing>")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-transaction PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload — watch->reload dev loop (Phase 1). Reuses the compatible /
# incompatible logic fixtures (above); the test copies them to a watched temp
# path and drives ReloadController::poll().
pulp_add_test_suite(pulp-test-reload-controller LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-controller
    pulp-reload-logic-compat pulp-reload-logic-incompat)
target_compile_definitions(pulp-test-reload-controller PRIVATE
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    # Watch a path inside the build tree, NOT /tmp: macOS dyld kills a process
    # that dlopens an unsigned .dylib from a world-writable temp dir.
    RELOAD_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-controller PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload — DAW-integration shell, end to end (Phase 1b). The shell is a
# Processor (format adapters wrap it) that loads its DSP from a logic library and
# hot-swaps it via a background watcher. A "unity" (1x) fixture is the initial
# logic so a swap to the 2x "compatible" fixture is observable in the output.
add_library(pulp-reload-logic-unity MODULE fixtures/reload_logic_unity.cpp)
set_target_properties(pulp-reload-logic-unity PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pulp-reload-logic-unity PRIVATE pulp::format)
pulp_add_test_suite(pulp-test-reloadable-shell LIBRARIES pulp::format)
add_dependencies(pulp-test-reloadable-shell
    pulp-reload-logic-unity pulp-reload-logic-compat pulp-reload-logic-incompat)
target_compile_definitions(pulp-test-reloadable-shell PRIVATE
    RELOAD_LOGIC_UNITY="$<TARGET_FILE:pulp-reload-logic-unity>"
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    RELOAD_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32)
    target_link_libraries(pulp-test-reloadable-shell PRIVATE ${CMAKE_DL_LIBS})
endif()
find_package(Threads REQUIRED)
target_link_libraries(pulp-test-reloadable-shell PRIVATE Threads::Threads)

# Plugin-contributed settings sections (MM-PR5) — SettingsPanel lives in pulp::standalone
pulp_add_test_suite(pulp-test-settings-sections LIBRARIES pulp::standalone)

pulp_add_test_suite(pulp-test-plugin-state-io LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-plugin-state-restore-diagnostics LIBRARIES pulp::format)

# AAX metadata/model tests
pulp_add_test_suite(pulp-test-aax-model LIBRARIES pulp::format)

# AAX MIDI bridge — SDK-gated runtime test. test_aax_midi.cpp covers the
# reassembly/fragmentation SDK-free; this drives the thin SDK glue
# (decode_midi_node / encode_midi_node in aax_midi_node.cpp) through the real
# Avid AAX SDK types (AAX_IMIDINode / AAX_CMidiStream / AAX_CMidiPacket), so it
# only builds when the developer-supplied SDK is configured (PULP_ENABLE_AAX=ON
# + a valid PULP_AAX_SDK_DIR). The adapter source is compiled directly into the
# test because aax_midi_node.cpp is otherwise only built per-plugin.
if(PULP_HAS_AAX)
    # Ensure the AAX SDK interface/library targets exist even when no AAX plugin
    # (i.e. no example) is part of this configure. The helper is idempotent.
    pulp_ensure_aax_sdk_targets()
    pulp_add_test_suite(pulp-test-aax-midi-node
        SOURCES test_aax_midi_node.cpp
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_midi_node.cpp"
        LIBRARIES pulp::format pulp-aax-sdk-interface
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/core/format/include")
    target_compile_options(pulp-test-aax-midi-node PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>)
endif()

# AAX MIDI/SysEx packet reassembly + fragmentation (SDK-free pure logic)
pulp_add_test_suite(pulp-test-aax-midi LIBRARIES pulp::format)

# Appcast/auto-update tests
pulp_add_test_suite(pulp-test-appcast LIBRARIES pulp::ship)

# Sparkle Ed25519 sign/verify
pulp_add_test_suite(pulp-test-sparkle-verify LIBRARIES pulp::ship pulp::runtime)

# Android package/signing tests (host-side fake SDK/toolchain only)
pulp_add_test_suite(pulp-test-android-package LIBRARIES pulp::ship)

# V3 gaps tests (Bias, MidiSequence, SubsectionReader, HostType, Primes, Expression)
pulp_add_test_suite(pulp-test-v3-gaps LIBRARIES pulp::signal pulp::midi pulp::audio pulp::format pulp::runtime)

# Codesign tests
if(WIN32)
    pulp_add_test_suite(pulp-test-codesign
        LIBRARIES pulp::ship
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-codesign LIBRARIES pulp::ship)
endif()
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
# `slow`: hammer tests run a writer + readers for 500 ms each
# to surface ordering bugs. Sanitizers.yml exercises the same code
# under TSan; PR fast-CI can skip the redundant smoke.
catch_discover_tests(pulp-test-sync PROPERTIES LABELS slow)

# License and BigInteger tests
pulp_add_test_suite(pulp-test-license LIBRARIES pulp::runtime pulp-cpp-httplib)

# License v1 -> v2 migration
pulp_add_test_suite(pulp-test-license-migration LIBRARIES pulp::runtime)

# MIDI CI tests
pulp_add_test_suite(pulp-test-midi-ci LIBRARIES pulp::midi)

# MIDI CI Property Exchange (PE) tests
pulp_add_test_suite(pulp-test-midi-ci-pe LIBRARIES pulp::midi)

# AU v2 effect adapter MIDI-input tests — guards the 2026-04-22 fix that
# wired HandleMIDIEvent / HandleSysEx into `PulpAUEffect` and flipped the
# component type from aufx to aumf for descriptor.accepts_midi=true.
# Apple-only because the adapter header pulls in AudioUnitSDK (which is
# not linked on non-Apple lanes).
if(APPLE AND NOT PULP_IOS AND PULP_HAS_AUSDK)
    add_executable(pulp-test-au-v2-effect test_au_v2_effect.cpp)
    # Links ausdk directly: this test includes AudioUnitSDK headers, which
    # pulp::format no longer exports (ausdk is PRIVATE on pulp-format to stop
    # leaking those headers onto every consumer). AU-header access is opt-in
    # via linking ausdk, same as _pulp_add_au does for real AU plugins.
    target_link_libraries(pulp-test-au-v2-effect PRIVATE
        pulp::format pulp::midi ausdk Catch2::Catch2WithMain)
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
        pulp::format pulp::view ausdk Catch2::Catch2WithMain
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

# iOS AVAudioSession bridge
pulp_add_test_suite(pulp-test-ios-audio-session LIBRARIES pulp::format)
if(NOT PULP_IOS)
    add_executable(pulp-test-pulp-bridge test_pulp_bridge.cpp)
    target_link_libraries(pulp-test-pulp-bridge PRIVATE
        pulp::format
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-pulp-bridge)
endif()
# ARA scaffolding
pulp_add_test_suite(pulp-test-ara LIBRARIES pulp::format)
# CLAP ARA extension end-to-end. This suite instantiates
# PulpClapPlugin directly, so it must see the same layout as pulp-format's
# desktop CLAP adapter TU.
pulp_add_test_suite(pulp-test-clap-ara-extension
    LIBRARIES pulp::format
    COMPILE_DEFINITIONS PULP_CLAP_GUI=1)
# Processor ARA hook
pulp_add_test_suite(pulp-test-processor-ara-hook LIBRARIES pulp::format)
# Processor bus-layout validation + processBlock precision contract +
# latency / tail change notification.
pulp_add_test_suite(pulp-test-processor-layout-latency LIBRARIES pulp::format)
# AudioPlayHead transport-extension helpers used by every
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
# TableModel data/sort layer
pulp_add_test_suite(pulp-test-table-model LIBRARIES pulp::view)
# ModulationMatrix data model
pulp_add_test_suite(pulp-test-modulation-matrix LIBRARIES pulp::view)
# ModulationMatrixWidget canvas widget
pulp_add_test_suite(pulp-test-modulation-matrix-widget LIBRARIES pulp::view)
# Lasso selection overlay
pulp_add_test_suite(pulp-test-lasso LIBRARIES pulp::view)
# WaveformRecorder three-state recorder widget (Ink & Signal design system)
pulp_add_test_suite(pulp-test-waveform-recorder LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-design-frame-momentary LIBRARIES pulp::view)
# Split pane layout widget
pulp_add_test_suite(pulp-test-split-view LIBRARIES pulp::view)
# CommandRegistry + KeyMappingEditor (Pulp-native names)
pulp_add_test_suite(pulp-test-command-registry LIBRARIES pulp::view pulp::state)
# Audio Inspector developer tool window
pulp_add_test_suite(pulp-test-audio-inspector-window LIBRARIES pulp::view pulp::audio)
# PluginDescriptor vendor_url + vendor_email
pulp_add_test_suite(pulp-test-vendor-url LIBRARIES pulp::format)
# Image cache + ImageView↔ImageCache wiring
pulp_add_test_suite(pulp-test-image-cache LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-image-view-cache LIBRARIES pulp::view)
# Accessibility tree snapshot
pulp_add_test_suite(pulp-test-accessibility-tree LIBRARIES pulp::view)
# Text-accessibility scaffold
pulp_add_test_suite(pulp-test-text-accessibility LIBRARIES pulp::view)
# macOS NSAccessibility backend for TextAccessibilityNode
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-text-accessibility-macos test_text_accessibility_macos.mm)
    target_link_libraries(pulp-test-text-accessibility-macos
        PRIVATE pulp::view Catch2::Catch2WithMain "-framework AppKit")
    catch_discover_tests(pulp-test-text-accessibility-macos)
endif()
# macOS hover delivery for a hosted PluginViewHost (tracking area +
# host-window acceptsMouseMovedEvents).
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-plugin-view-hover-macos test_plugin_view_host_hover_macos.mm)
    target_link_libraries(pulp-test-plugin-view-hover-macos
        PRIVATE pulp::view Catch2::Catch2WithMain "-framework AppKit")
    catch_discover_tests(pulp-test-plugin-view-hover-macos)
endif()
# Windows UIA backend — compile-gated on _WIN32 in the
# source. The sentinel test case keeps the binary present + named
# consistently on non-Windows hosts so ctest output stays stable.
pulp_add_test_suite(pulp-test-text-accessibility-windows LIBRARIES pulp::view)
# Linux AccessKit backend — compile-gated on
# __linux__ && !__ANDROID__; sentinel on other hosts.
pulp_add_test_suite(pulp-test-text-accessibility-linux LIBRARIES pulp::view)
# A/B compare
pulp_add_test_suite(pulp-test-ab-compare LIBRARIES pulp::view)
# ViewSize::aspect_ratio field
pulp_add_test_suite(pulp-test-view-size-aspect LIBRARIES pulp::format)

# WindowHost::set_design_viewport pure-math
# coverage. Locks down the scale + letterbox transform that the mac
# GPU host (and any other future platform host) uses to fit
# fixed-design content into the current window.
pulp_add_test_suite(pulp-test-view-design-viewport LIBRARIES pulp::view)

# Shared continuous-frame predicate (needs_continuous_frames) the window /
# plugin-view hosts and a foreign-host embed tick gate repaint on.
pulp_add_test_suite(pulp-test-continuous-frames LIBRARIES pulp::view)

# PluginViewHost design-viewport wiring (mac CPU + GPU). The pure
# math is already pinned by `pulp-test-view-design-viewport`; covers the
# PluginViewHost virtuals + CPU-host inverse mouse mapping (GPU section
# soft-skips without Dawn; foundation for CLAP/VST3 resize negotiation)
# and the keyboard-focus host-etiquette contract (prior-responder
# restore, host-grab ends text input — keeps Musical Typing alive).
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-plugin-view-host-design-viewport
        test_plugin_view_host_design_viewport.mm
        test_plugin_view_host_key_focus.mm)
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

# View host bridges — screenshot/window/plugin-view
add_executable(pulp-test-view-host-bridge test_view_host_bridge.cpp)
if(APPLE AND NOT PULP_IOS)
    target_sources(pulp-test-view-host-bridge PRIVATE test_view_host_bridge_mac.mm)
endif()
target_link_libraries(pulp-test-view-host-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-host-bridge)
# Scan blacklist
add_executable(pulp-test-scan-blacklist test_scan_blacklist.cpp)
target_link_libraries(pulp-test-scan-blacklist PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow`: file-mtime sleep loops (rebuilt-plugin-not-blacklisted
# flow waits on filesystem timestamp resolution). Each test ~1s.
catch_discover_tests(pulp-test-scan-blacklist PROPERTIES LABELS slow)
# Crash-isolated scanner. Skipped on iOS/Android
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
# Hosted editor API
pulp_add_test_suite(pulp-test-hosted-editor LIBRARIES pulp::host)
# HostedEditor → WindowHost attachment migration
pulp_add_test_suite(pulp-test-hosted-editor-migration LIBRARIES pulp::view pulp::host)
# CFRunLoop cooperation — plugin-mode MainThreadDispatcher
pulp_add_test_suite(pulp-test-cfrunloop-cooperation LIBRARIES pulp::events)
if(APPLE)
    target_link_libraries(pulp-test-cfrunloop-cooperation PRIVATE "-framework CoreFoundation")
endif()
# High-level window classes
pulp_add_test_suite(pulp-test-document-window LIBRARIES pulp::view)
# Accessibility announcements
pulp_add_test_suite(pulp-test-announce LIBRARIES pulp::view)
# ARA core type layer
pulp_add_test_suite(pulp-test-ara-types LIBRARIES pulp::format)
# Processor on_host_transport_changed hook
pulp_add_test_suite(pulp-test-transport-hook LIBRARIES pulp::format)
# Scan cache
add_executable(pulp-test-scan-cache test_scan_cache.cpp)
target_link_libraries(pulp-test-scan-cache PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow`: each scenario waits for filesystem mtime resolution
# (~1s each on every platform). Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scan-cache PROPERTIES LABELS slow)
# Processor memory-pressure hook
pulp_add_test_suite(pulp-test-memory-pressure LIBRARIES pulp::format)
# iOS background-audio descriptor flag
pulp_add_test_suite(pulp-test-ios-background-audio-flag LIBRARIES pulp::format)

# MidiBuffer UMP sidecar
pulp_add_test_suite(pulp-test-midi-buffer-ump LIBRARIES pulp::midi)
# MidiBuffer SysEx sidecar — full MIDI vocabulary
pulp_add_test_suite(pulp-test-midi-buffer-sysex LIBRARIES pulp::midi)
# Win MIDI MIM_LONGDATA SysEx + QPC timestamps. Cross-platform, with
# Windows-only cases guarded by #ifdef _WIN32.
pulp_add_test_suite(pulp-test-winmidi-sysex LIBRARIES pulp::midi)
# WinRT MIDI 2.0 (UMP) backend — W3 (UMP↔MIDI1 + shared sysex7 reassembly + F0..F7 framing; WinRT TU opt-in via PULP_HAS_WINRT_MIDI, #error-guarded off non-Windows).
pulp_add_test_suite(pulp-test-winrt-midi LIBRARIES pulp::midi)
# Accessibility provider cross-platform entry
pulp_add_test_suite(pulp-test-accessibility-provider LIBRARIES pulp::view)
# PluginDescriptor validation
pulp_add_test_suite(pulp-test-descriptor-validation LIBRARIES pulp::format)
# PluginInfo metadata shape
pulp_add_test_suite(pulp-test-plugin-info-metadata LIBRARIES pulp::host)
# A11y role mapping tables — UIA + AT-SPI
pulp_add_test_suite(pulp-test-uia-mapping LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-atspi-mapping LIBRARIES pulp::view)
# AudioSystem hotplug base plumbing
pulp_add_test_suite(pulp-test-audio-system-hotplug LIBRARIES pulp::audio)

# WASAPI capture path. Windows-only; cross-platform stub keeps
# CI green elsewhere. Skips at runtime on hosts without a default
# capture endpoint.
add_executable(pulp-test-wasapi-input test_wasapi_input.cpp)
target_link_libraries(pulp-test-wasapi-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-input)

# WASAPI share-mode coverage contract. Pins "shared-mode only"
# surface; static_asserts fire on every platform so a Windows-only
# DeviceConfig extension still trips this on macOS/Linux CI.
add_executable(pulp-test-wasapi-share-modes test_wasapi_share_modes.cpp)
target_link_libraries(pulp-test-wasapi-share-modes PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-share-modes PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-share-modes)

# ALSA capture + real device metadata. Linux-only;
# cross-platform stub keeps CI green elsewhere. Skips at runtime on
# hosts without an ALSA-routable default device.
add_executable(pulp-test-alsa-input test_alsa_input.cpp)
target_link_libraries(pulp-test-alsa-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-alsa-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-alsa-input)

# Cross-platform device-hotplug monitor. Lives in pulp::runtime so
# both audio (AlsaSystem) and MIDI (AlsaMidiSystem) share it; libudev is the
# Linux backend, no-op elsewhere — so this test runs on every platform.
add_executable(pulp-test-udev-monitor test_udev_monitor.cpp)
target_link_libraries(pulp-test-udev-monitor PRIVATE pulp::runtime Catch2::Catch2WithMain)
target_include_directories(pulp-test-udev-monitor PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-udev-monitor)

# JACK device-enumeration contract. Registered only when CMake
# detected a JACK dev package and compiled the backend into pulp-audio — the
# test calls JackSystem symbols that exist only in that configuration, and it
# is the regression guard for the enumerate_devices() build break.
if(PULP_JACK_AVAILABLE)
    add_executable(pulp-test-jack-device test_jack_device.cpp)
    target_link_libraries(pulp-test-jack-device PRIVATE pulp::audio Catch2::Catch2WithMain)
    target_include_directories(pulp-test-jack-device PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(pulp-test-jack-device PRIVATE PULP_HAS_JACK=1)
    catch_discover_tests(pulp-test-jack-device)
endif()
# Processor on_host_tempo_changed hook.
pulp_add_test_suite(pulp-test-tempo-hook LIBRARIES pulp::format)
# Resizable plugin shell.
pulp_add_test_suite(pulp-test-resizable-shell LIBRARIES pulp::view)
# ATK / AT-SPI mapping.
pulp_add_test_suite(pulp-test-atk-mapping LIBRARIES pulp::view)
# Background scanner.
pulp_add_test_suite(pulp-test-background-scanner LIBRARIES pulp::host)

# TextShaper (PreText-style measure-once-reflow-forever)
pulp_add_test_suite(pulp-test-text-shaper LIBRARIES pulp::canvas)

# PNG / GIF / TIFF codec round-trip suite: GIF89a encoder/decoder +
# Baseline TIFF 6.0 encoder/decoder. Skia-free, runs on every config.
pulp_add_test_suite(pulp-test-image-codecs-extended LIBRARIES pulp::canvas)

# SheenBidi-backed BidiAnalyzer + TextRunPlanner non-ICU fallback.
# Exercises pure-Arabic,
# pure-Hebrew, and mixed Latin+Arabic/Hebrew strings. Each TEST_CASE
# gates RTL-specific assertions on BidiAnalyzer::has_sheenbidi() so the
# suite still passes on reduced-deps configs that compile the
# pass-through stub.
pulp_add_test_suite(pulp-test-bidi-text LIBRARIES pulp::canvas)

# FontScope memory-budget eviction.
pulp_add_test_suite(pulp-test-font-scope-budget LIBRARIES pulp::canvas)

# TTF/OTF structural sanitizer.
# Gated on PULP_HAS_SKIA: the non-Skia stub in font_registry_stubs.cpp
# accepts any non-empty buffer, so the rejection cases false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-security LIBRARIES pulp::canvas)
endif()

# Variable axis wiring.
# Gated on PULP_HAS_SKIA: FontResolver::resolve_family_list returns
# NotFound when Skia is absent, so the has_typeface() assertions
# false-negative.
if(PULP_HAS_SKIA)
    pulp_add_test_suite(pulp-test-font-variable-axes LIBRARIES pulp::canvas)
endif()

# UAX #29-lite cluster_step.
pulp_add_test_suite(pulp-test-cluster-step LIBRARIES pulp::canvas)

# Locale-aware word + line breaking.
# The surface always links: when ICU's <unicode/brkiter.h> is on the
# include path the assertions exercise the real UAX #14 path against
# the SkUnicode_icu symbols bundled in libskia.a; otherwise the
# degraded ASCII-space fallback keeps the API functional and the
# English-only expectations still hold.
pulp_add_test_suite(pulp-test-font-locale-shaping LIBRARIES pulp::canvas)

# AA / hinting / subpixel policy centralization.
add_executable(pulp-test-font-aa-hinting test_font_aa_hinting.cpp)
target_link_libraries(pulp-test-font-aa-hinting PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_link_libraries(pulp-test-font-aa-hinting PRIVATE skia::skia)
endif()
catch_discover_tests(pulp-test-font-aa-hinting)

# FontFlightRecorder + JSON drain.
add_executable(pulp-test-font-flight-recorder test_font_flight_recorder.cpp)
target_link_libraries(pulp-test-font-flight-recorder PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-flight-recorder)

# Variable-font axis-animation LRU cache.
add_executable(pulp-test-font-axis-animation test_font_axis_animation.cpp)
target_link_libraries(pulp-test-font-axis-animation PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-axis-animation)

# Parallel shaping via TextRunPlanner::shape_batch.
add_executable(pulp-test-text-run-planner-parallel test_text_run_planner_parallel.cpp)
target_link_libraries(pulp-test-text-run-planner-parallel PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-text-run-planner-parallel)

# Real ICU bidi + script run iterators. Verifies multi-run shaping on
# bidi / script boundaries plus UAX #29 cluster grouping (ZWJ,
# regional-indicator-pair, virama,
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

# Async font lifecycle (register_font_url).
add_executable(pulp-test-font-async-lifecycle test_font_async_lifecycle.cpp)
target_link_libraries(pulp-test-font-async-lifecycle PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-async-lifecycle)

# WOFF2 runtime decoding (security-gated).
# Structural rejection + decoder-availability probe; no Skia link
# required because the negative paths are universal.
add_executable(pulp-test-font-woff2 test_font_woff2.cpp)
target_link_libraries(pulp-test-font-woff2 PRIVATE pulp::canvas Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-font-woff2)

# Color-font predicate on ResolvedFont.
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

# Measurement-paint parity harness.
# Asserts TextShaper predictions match Skia's measured advance within
# 0.5px for a hand-picked sample set representative of CHAIN INFO /
# CROSSOVER bugs. The full multilingual corpus (test/text_corpus/
# corpus.json) loader + per-TextAnchor bbox assertions land alongside
# a future JSON parser link.
if(PULP_HAS_SKIA)
    add_executable(pulp-test-parity test_parity.cpp)
    target_link_libraries(pulp-test-parity PRIVATE pulp::canvas Catch2::Catch2WithMain skia::skia)
    catch_discover_tests(pulp-test-parity)
endif()

# Cross-backend font rendering goldens.
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

# Skia GPU font rendering lane (Dawn → Metal on macOS-arm64).
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

    # GPU view-host-in-plugins — proves a Processor::create_view() editor
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

# Skia GPU Vulkan font rendering lane (scaffold).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The Vulkan
# lane is intentionally OFF by default: there is no Vulkan-capable
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <vulkan/vulkan.h>"
# failures. Flip `PULP_VULKAN_AVAILABLE=ON` on a future Linux or
# Windows CI lane (with Skia-Vulkan linked) to light the target up.
# Tolerance for the raster↔Vulkan cross-backend probe is ±20 % —
# see the test header for the rationale.
option(PULP_VULKAN_AVAILABLE
    "Build the Skia GPU Vulkan rendering-goldens scaffold. Requires \
Skia built with skia_use_vulkan=true and a working Vulkan ICD on the \
host. OFF on every Pulp CI lane today; flip ON when a Vulkan-capable \
runner exists."
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

# Skia GPU Direct3D 12 font rendering lane (scaffold).
# Sibling of `pulp-test-font-rendering-goldens-gpu` (Metal). The D3D
# lane is intentionally OFF by default: there is no Windows D3D
# runner in Pulp's required gate set today, so building this target
# would only burn CI roundtrips on "couldn't find <d3d12.h>" failures.
# Flip `PULP_D3D_AVAILABLE=ON` on a future Windows CI lane (with
# Skia-D3D linked) to light the target up. Tolerance for the
# raster↔D3D cross-backend probe is ±20 % — see the test header for
# the rationale.
option(PULP_D3D_AVAILABLE
    "Build the Skia GPU Direct3D 12 rendering-goldens scaffold. \
Requires Skia built with skia_use_direct3d=true on Windows. OFF on \
every Pulp CI lane today; flip ON when a Windows D3D runner exists."
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

# Ed25519 (RFC 8032).
pulp_add_test_suite(pulp-test-ed25519 LIBRARIES pulp::runtime)

# IPC (InterprocessConnection) tests
add_executable(pulp-connected-child-process-fixture
    fixtures/connected_child_process_fixture.cpp)
target_link_libraries(pulp-connected-child-process-fixture PRIVATE pulp::events)

if(WIN32)
    pulp_add_test_suite(pulp-test-ipc
        LIBRARIES pulp::events pulp::runtime
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-ipc LIBRARIES pulp::events pulp::runtime)
endif()
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

# CodeEditor per-language tokenizer coverage.
pulp_add_test_suite(pulp-test-code-editor-tokenizer LIBRARIES pulp::view)

# PropertiesFile JSON persistence tests
pulp_add_test_suite(pulp-test-properties SOURCES test_properties_file.cpp LIBRARIES pulp::state pulp::runtime)

# AudioDeviceManager persistence + MIDI hub.
pulp_add_test_suite(pulp-test-audio-device-manager
    SOURCES test_audio_device_manager.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::midi pulp::state pulp::runtime)

# DSP enhancement tests (dry/wet mixer, processor duplicator, matrix, special functions)
pulp_add_test_suite(pulp-test-dsp-enhancements LIBRARIES pulp::signal)

# Elliptic / Jacobi special functions (pulp::signal::special). Kept in its
# own binary so future elliptic IIR design tests can live
# beside it without bloating pulp-test-dsp-enhancements.
pulp_add_test_suite(pulp-test-special-functions LIBRARIES pulp::signal)

# Animation & 3D math tests
pulp_add_test_suite(pulp-test-animation-3d LIBRARIES pulp::view)

# AnimatorSet / 3D types tests
pulp_add_test_suite(pulp-test-animator-set LIBRARIES pulp::view)

# Runtime utility tests (mmap, temp file, dynlib, base64, range, child process)
pulp_add_test_suite(pulp-test-runtime-utils LIBRARIES pulp::runtime pulp-cpp-httplib)

# MAC address parser/formatter.
pulp_add_test_suite(pulp-test-mac-address LIBRARIES pulp::runtime)

# URL parser + percent-encoding + query helpers.
pulp_add_test_suite(pulp-test-url LIBRARIES pulp::runtime)

# Result<T,E> header-only utility.
pulp_add_test_suite(pulp-test-runtime-result LIBRARIES pulp::runtime)

# XML and ZIP/GZIP compression tests
pulp_add_test_suite(pulp-test-xml-zip LIBRARIES pulp::runtime)

# SIMD operations and aligned buffer tests
pulp_add_test_suite(pulp-test-simd LIBRARIES pulp::runtime pulp::signal)

# Drag-and-drop tests
pulp_add_test_suite(pulp-test-dnd SOURCES test_drag_drop.cpp LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-musical-typing SOURCES test_musical_typing.cpp LIBRARIES pulp::view)

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
# Spectral primitives: STFT/WOLA engine, pitch/time, formant, smoothing.
pulp_add_test_suite(pulp-test-spectral-primitives SOURCES test_spectral_frame_engine.cpp test_realtime_pitch_time.cpp test_transient_freeze_delay.cpp test_spectral_matrix.cpp test_stn_stretch.cpp test_sinc_pitch.cpp LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-stn-decomposer LIBRARIES pulp::signal)
# Offline time-stretch/pitch engine (orchestrates the spectral primitives).
pulp_add_test_suite(pulp-test-offline-stretch LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-noise-morpher LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-sinc-resampler LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-freeze-loop-sampler LIBRARIES pulp::signal)
# Multi-backend FFT facade (vdsp / kissfft / fftw3 / mkl). Verifies
# selector helpers, env-var routing, round-trip on each available
# backend, and cross-backend numerical equivalence.
pulp_add_test_suite(pulp-test-fft-backends LIBRARIES pulp::signal-fft-backend)
# Signal meter tests (P5 test-split — extracted from test_signal.cpp).
# MultiChannelMeter / MultiChannelBallistics TEST_CASE clusters moved
# verbatim. Distinct from pulp-test-multi-channel-meter (test_multi_
# channel_meter.cpp), which keeps its own #645 edge-case coverage.
pulp_add_test_suite(pulp-test-signal-meter LIBRARIES pulp::signal)
# Biquad filter tests
pulp_add_test_suite(pulp-test-biquad LIBRARIES pulp::signal)
# DSL processor contract tests (FaustProcessor + PulpFaustUI + PulpFaustMeta)
add_executable(pulp-test-dsl-processor test_dsl_processor.cpp)
target_link_libraries(pulp-test-dsl-processor PRIVATE
    pulp::dsl pulp::format pulp::state pulp::audio pulp::midi
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsl-processor)

# Convolution engine tests
pulp_add_test_suite(pulp-test-convolver LIBRARIES pulp::signal)

# Background-IR-swap tests for PartitionedConvolver. Lock-free
# pointer-shuttle hand-off from worker thread → audio thread, including
# a concurrent stage/swap hammer test.
pulp_add_test_suite(pulp-test-convolver-bg-swap LIBRARIES pulp::signal pulp::runtime)

# Non-uniform partitioned convolver tests. Two-stage Gardner-style
# head (small block) + tail (K× larger block) with zero-latency mixing.
pulp_add_test_suite(pulp-test-convolver-non-uniform LIBRARIES pulp::signal)

# Test signal source (sine tone, file playback)
pulp_add_test_suite(pulp-test-test-signal LIBRARIES pulp::standalone)
# Standalone editor chrome helpers
# These construct a StandaloneApp, which opens the real audio device on init
# (standalone.cpp create_device/open/start) — same RT-thread-starved teardown
# hazard as pulp-test-audio above, so they get the same PROCESSORS reservation.
pulp_add_test_suite(pulp-test-standalone-editor-chrome LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-apply-config LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-audio-capture-wav LIBRARIES pulp::standalone pulp::audio PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-audio-capture-rolling-wav LIBRARIES pulp::standalone pulp::audio PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-transport-midi LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-audio-inspector
    LIBRARIES pulp::standalone pulp::view PROPERTIES PROCESSORS 8)
# Headless screenshot capture state machine
pulp_add_test_suite(pulp-test-screenshot-capture LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
# STFT and audio visualization signal tests
pulp_add_test_suite(pulp-test-stft LIBRARIES pulp::signal)
# Visualization bridge and widget tests
pulp_add_test_suite(pulp-test-visualization LIBRARIES pulp::view)

# Clipboard tests
add_executable(pulp-test-clipboard test_clipboard.cpp)
target_link_libraries(pulp-test-clipboard PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clipboard
    PROPERTIES RESOURCE_LOCK system-clipboard)

# FileDialog backend-registration tests
pulp_add_test_suite(pulp-test-file-dialog LIBRARIES pulp::platform)

# D-Bus client + Linux xdg-desktop-portal file-dialog backend
pulp_add_test_suite(pulp-test-dbus LIBRARIES pulp::platform)

# Platform tests
pulp_add_test_suite(pulp-test-platform LIBRARIES pulp::platform)

# Permissions tests
pulp_add_test_suite(pulp-test-permissions LIBRARIES pulp::platform)

# Environment API tests
pulp_add_test_suite(pulp-test-environment LIBRARIES pulp::platform)

# Runtime tests
pulp_add_test_suite(pulp-test-runtime
    SOURCES test_runtime.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::runtime)

# AbstractFifo (index-pair SPSC FIFO management)
pulp_add_test_suite(pulp-test-abstract-fifo LIBRARIES pulp::runtime)

# FileSearchPath (ordered directory search list)
pulp_add_test_suite(pulp-test-file-search-path LIBRARIES pulp::runtime)

# Sync-primitive hammer for the TSan-focused race regression.
# Runs under the tag-scoped TSan subset via the [concurrent][race]
# tags embedded in each TEST_CASE (matches the 'Race|Concurrent'
# alternation in sanitizers.yml's --tests-regex).
add_executable(pulp-test-sync-race-hammer test_sync_race_hammer.cpp)
target_link_libraries(pulp-test-sync-race-hammer PRIVATE pulp::runtime Catch2::Catch2WithMain)
# `slow`: N-thread race smoke. Sanitizer builds also pick this
# up via the dedicated sanitizers.yml workflow, so PR fast-CI doesn't
# need to re-run it.
catch_discover_tests(pulp-test-sync-race-hammer PROPERTIES LABELS slow)

# Events tests
pulp_add_test_suite(pulp-test-events LIBRARIES pulp::events)

pulp_add_test_suite(pulp-test-events-async-helpers LIBRARIES pulp::events)

# PushNotifications cross-platform smoke + headless-mock coverage.
pulp_add_test_suite(pulp-test-push-notifications LIBRARIES pulp::events)

# IapClient cross-platform smoke + headless-mock coverage.
pulp_add_test_suite(pulp-test-in-app-purchase LIBRARIES pulp::events)

# Message-loop integration cross-platform introspection surface.
pulp_add_test_suite(pulp-test-message-loop-integration LIBRARIES pulp::events)

add_executable(pulp-test-events-timer-helpers test_events_timer_helpers.cpp)
target_link_libraries(pulp-test-events-timer-helpers PRIVATE pulp::events Catch2::Catch2WithMain)
# `slow`: Timer hammer + UAF-free destroy-while-dispatched
# tests deliberately exercise message-loop pacing (~0.3-1.5 sec each
# depending on platform). Excluded from PR fast-CI; sanitizers.yml
# still covers Timer under TSan.
catch_discover_tests(pulp-test-events-timer-helpers PROPERTIES LABELS slow)

# Audio file I/O tests
pulp_add_test_suite(pulp-test-audio-file LIBRARIES pulp::audio)

# Phase-vocoder offline time-stretch / pitch-shift.
pulp_add_test_suite(pulp-test-phase-vocoder LIBRARIES pulp::signal)

# Click-free soft-bypass wrapper.
pulp_add_test_suite(pulp-test-soft-bypass LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-sample-resource
    SOURCES test_sample_resource.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::runtime)

# AudioThumbnail + AudioThumbnailCache (item 6.12).
# Links pulp::view so the WaveformView integration smoke compiles.
pulp_add_test_suite(pulp-test-audio-thumbnail LIBRARIES pulp::audio pulp::view)

# Memory-mapped reader: true ranged (seek-based) decode, no whole-file decode.
pulp_add_test_suite(pulp-test-mmap-reader-ranged LIBRARIES pulp::audio)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sampler_runtime_tests.cmake")
# WAV metadata round-trip tests (BWAV / iXML / ASWG / ACID — item 6.11)
pulp_add_test_suite(pulp-test-wav-metadata LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-offline-processor-edges LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-ogg-reader LIBRARIES pulp::audio)

# Audio tests
# PROCESSORS reservation for suites that open the REAL CoreAudio output device.
# CoreAudioDevice::stop()/close() call AudioOutputUnitStop / AudioUnitUninitialize
# (coreaudio_device.mm), which block until the real-time I/O thread observes the
# request. Under `ctest -j8` heavy load that RT thread is CPU-starved and never
# observes it, so teardown hangs to the 120s timeout — the full-suite-only flake
# that wedged the macOS self-hosted runner. Reserving PROCESSORS == the CI -j
# makes ctest schedule these alone, so the RT thread gets the machine and
# teardown returns promptly. (Root-cause fix replacing the build.yml exclude.)
pulp_add_test_suite(pulp-test-audio LIBRARIES pulp::audio
    PROPERTIES PROCESSORS 8)

pulp_add_test_suite(pulp-test-system-volume LIBRARIES pulp::audio)

# Streaming sample source: preload window + background-filled ring + RT-safe
# sequential pull. Threaded case has an internal deadline; give headroom.
pulp_add_test_suite(pulp-test-streaming-sample-source LIBRARIES pulp::audio
    PROPERTIES TIMEOUT 60)

# Multi-mic / velocity-layer / round-robin layered zone selection.
pulp_add_test_suite(pulp-test-zone-layer-select LIBRARIES pulp::audio)

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
pulp_add_test_suite(pulp-test-state
    SOURCES test_state.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::state pulp::events)

# Binding tests
pulp_add_test_suite(pulp-test-binding LIBRARIES pulp::state)

# Structured non-param state channel (sequencer/mod-matrix transport)
pulp_add_test_suite(pulp-test-sequencer-state-channel LIBRARIES pulp::state)

# Headless adapter tests
pulp_add_test_suite(pulp-test-headless LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-audio-inspector-demo-processor SOURCES test_audio_inspector_demo_processor.cpp LIBRARIES pulp::format pulp::audio pulp::midi)
# .pulpset render/replay harness (G4)
pulp_add_test_suite(pulp-test-pulpset-replay SOURCES test_pulpset_replay.cpp LIBRARIES pulp::format)

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

    # CLAP inbound/outbound MIDI event coverage + steady-state RT process guard.
    #
    # Unlike `pulp-test-clap-entry` above, this test DOES NOT re-compile
    # clap_adapter.cpp into its own TU. The test drives the adapter via
    # its public header surface (clap_init / clap_activate / clap_process
    # — see core/format/include/pulp/format/clap_adapter.hpp), so linking
    # against pulp::format is sufficient. Keeping the direct compilation
    # out means the coverage artifact reports exactly one TU for
    # clap_adapter.cpp (the library's), which is what CI's diff-cover
    # gate reads — avoids the split-TU problem that produced "96% local
    # / 62% CI" reports.
    add_executable(pulp-test-clap-midi-events test_clap_midi_events.cpp)
    target_sources(pulp-test-clap-midi-events PRIVATE $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>)
    target_link_libraries(pulp-test-clap-midi-events PRIVATE pulp::format clap Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    target_compile_definitions(pulp-test-clap-midi-events PRIVATE PULP_CLAP_GUI=1 $<$<BOOL:${UNIX}>:PULP_CLAP_PROCESS_RT_TRAP_TESTS=1>)
    catch_discover_tests(pulp-test-clap-midi-events)

    # Pin the CLAP host-facing API contract that Bitwig / Reaper /
    # FL Studio / Studio One depend on. Re-compiles the
    # adapter TU into its own executable so the test owns the
    # PULP_CLAP_PLUGIN(...) entry-symbol generator.
    add_executable(pulp-test-clap-host-validation test_clap_host_validation.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(pulp-test-clap-host-validation PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-host-validation PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-host-validation)

    # Empirical proof the CLAP adapter respects clamp_latency_to_nonneg
    # end-to-end (negative latency → 0 when the
    # quirk is enforced, raw-wrapped when PULP_HOST_QUIRKS=off). Drives the
    # adapter through its public header surface (init() caches the resolved
    # quirks), so link-only — no clap_adapter.cpp re-compile, keeping the
    # diff-cover TU attribution single (same rationale as the MIDI test).
    add_executable(pulp-test-clap-latency-quirk test_clap_latency_quirk.cpp)
    target_link_libraries(pulp-test-clap-latency-quirk PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-latency-quirk PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-latency-quirk)
endif()

if(PULP_HAS_VST3)
    add_executable(pulp-test-vst3-plugin-state
        test_vst3_plugin_state.cpp harness/rt_allocation_probe.cpp
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
        ausdk
        Catch2::Catch2WithMain
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudioKit"
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

if(PULP_ENABLE_SCENE3D)
    add_executable(pulp-test-scene3d test_scene3d.cpp)
    target_link_libraries(pulp-test-scene3d PRIVATE
        pulp::scene Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-scene3d PRIVATE
        PULP_TEST_SCENE3D_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d")
    catch_discover_tests(pulp-test-scene3d)

    pulp_add_test_suite(pulp-test-scene3d-renderer-characterization SOURCES test_scene3d_renderer_characterization.cpp LIBRARIES pulp::scene)

    add_test(NAME scene3d-inspect-boxtextured-stats
        COMMAND $<TARGET_FILE:pulp-scene3d-inspect>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set_tests_properties(scene3d-inspect-boxtextured-stats PROPERTIES
        PASS_REGULAR_EXPRESSION
            "meshes=1.*vertices=24.*indices=36.*textures=1")

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-scene-stats-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_stats.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_stats.cpp")
        set_tests_properties(scene3d-scene-stats-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_stats_surface_verified=true")

        add_test(NAME scene3d-scene-stats-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_stats.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_stats.cpp")
        set_tests_properties(scene3d-scene-stats-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_stats_surface_contract_case=valid-current-stats.*scene_stats_surface_contract_case=missing-field.*scene_stats_surface_contract_case=field-order-drift.*scene_stats_surface_contract_case=text-key-drift.*scene_stats_surface_contract_case=primitive-count-drift.*scene_stats_surface_contract_case=vertex-count-drift.*scene_stats_surface_contract_case=texture-byte-drift.*scene_stats_surface_contract_case=advanced-extension-drift.*scene_stats_surface_contract_case=error-diagnostic-drift.*scene_stats_surface_contract_verified=true")
    endif()

    add_test(NAME scene3d-inspect-boxtextured-render-packet
        COMMAND $<TARGET_FILE:pulp-scene3d-inspect>
            --render-packet
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set_tests_properties(scene3d-inspect-boxtextured-render-packet PROPERTIES
        PASS_REGULAR_EXPRESSION
            "render_packet transformed_nodes=2 primitives=1.*primitive node=1 mesh=0 primitive=0 material=0.*features=normals,texcoord0,indexed,base_color_texture")

    if(Python3_Interpreter_FOUND AND TARGET pulp-scene3d-inspect-native)
        add_test(NAME scene3d-inspect-native-draco-diagnostic
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect-native>)
        set_tests_properties(scene3d-inspect-native-draco-diagnostic PROPERTIES
            PASS_REGULAR_EXPRESSION
                "draco_inspect_default_not_wired=true.*(native_draco_decoder_available=false.*gltf.draco_unavailable|native_draco_decoder_available=true.*gltf.draco_decode_failed).*draco_inspect_native_callback=true")
        add_test(NAME scene3d-inspect-native-draco-diagnostic-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke_contract.py"
                --smoke "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke.py")
        set_tests_properties(scene3d-inspect-native-draco-diagnostic-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "draco_inspect_smoke_contract_case=valid-native-disabled.*draco_inspect_smoke_contract_case=valid-native-enabled.*draco_inspect_smoke_contract_case=default-exit-drift.*draco_inspect_smoke_contract_case=default-diagnostic-drift.*draco_inspect_smoke_contract_case=native-exit-drift.*draco_inspect_smoke_contract_case=native-missing-availability.*draco_inspect_smoke_contract_case=native-ambiguous-availability.*draco_inspect_smoke_contract_case=native-disabled-diagnostic-drift.*draco_inspect_smoke_contract_case=native-enabled-diagnostic-drift.*draco_inspect_smoke_contract_case=native-not-wired-leak.*draco_inspect_smoke_contract_verified=true")
    endif()

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-inspect-boxtextured-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-inspect-boxtextured-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_inspect_verified=boxtextured")

        add_test(NAME scene3d-inspect-verifier-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect_contract.py"
                --inspect-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect.py")
        set_tests_properties(scene3d-inspect-verifier-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_inspect_contract_case=valid-fake-inspect.*scene3d_inspect_contract_case=missing-render-packet.*scene3d_inspect_contract_case=duplicate-primitive.*scene3d_inspect_contract_case=stats-drift.*scene3d_inspect_contract_case=primitive-feature-drift.*scene3d_inspect_contract_verified=true")

        add_test(NAME scene3d-render-packet-material-key-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-material-key-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_material_key_verified=true")
        add_test(NAME scene3d-render-packet-material-key-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_contract.py"
                --material-key-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_smoke.py")
        set_tests_properties(scene3d-render-packet-material-key-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_material_key_contract_case=valid-fake-inspect.*render_packet_material_key_contract_case=inspect-exit-drift.*render_packet_material_key_contract_case=stats-texture-count-drift.*render_packet_material_key_contract_case=stats-texture-bytes-drift.*render_packet_material_key_contract_case=stats-extra-key.*render_packet_material_key_contract_case=missing-render-packet.*render_packet_material_key_contract_case=packet-has-errors-drift.*render_packet_material_key_contract_case=missing-primitive.*render_packet_material_key_contract_case=duplicate-primitive.*render_packet_material_key_contract_case=feature-mask-drift.*render_packet_material_key_contract_case=feature-name-missing.*render_packet_material_key_contract_case=feature-name-order-drift.*render_packet_material_key_contract_case=material-index-drift.*render_packet_material_key_contract_case=world-transform-drift.*render_packet_material_key_contract_verified=true")

        add_test(NAME scene3d-material-feature-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/material_key.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/material_key.cpp")
        set_tests_properties(scene3d-material-feature-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "material_feature_surface_verified=true")

        add_test(NAME scene3d-material-feature-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/material_key.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/material_key.cpp")
        set_tests_properties(scene3d-material-feature-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "material_feature_surface_contract_case=valid-current-material-feature.*material_feature_surface_contract_case=bit-assignment-drift.*material_feature_surface_contract_case=missing-feature.*material_feature_surface_contract_case=feature-order-drift.*material_feature_surface_contract_case=feature-name-drift.*material_feature_surface_contract_case=missing-feature-name.*material_feature_surface_contract_case=duplicate-feature-name.*material_feature_surface_contract_verified=true")

        add_test(NAME scene3d-render-packet-graph-error-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-graph-error-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_graph_error_verified=true")
        add_test(NAME scene3d-render-packet-graph-error-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_contract.py"
                --graph-error-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_smoke.py")
        set_tests_properties(scene3d-render-packet-graph-error-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_graph_error_contract_case=valid-fake-inspect.*render_packet_graph_error_contract_case=inspect-exit-drift.*render_packet_graph_error_contract_case=stats-diagnostics-drift.*render_packet_graph_error_contract_case=stats-error-diagnostics-drift.*render_packet_graph_error_contract_case=missing-render-packet.*render_packet_graph_error_contract_case=packet-primitive-count-drift.*render_packet_graph_error_contract_case=packet-diagnostic-count-drift.*render_packet_graph_error_contract_case=packet-has-errors-drift.*render_packet_graph_error_contract_case=missing-diagnostic.*render_packet_graph_error_contract_case=diagnostic-severity-drift.*render_packet_graph_error_contract_case=diagnostic-code-drift.*render_packet_graph_error_contract_case=primitive-leak-after-error.*render_packet_graph_error_contract_verified=true")

        add_test(NAME scene3d-render-packet-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/render_packet.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/render_packet.cpp"
                --inspect-source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_inspect.cpp")
        set_tests_properties(scene3d-render-packet-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_surface_verified=true")

        add_test(NAME scene3d-render-packet-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/render_packet.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/render_packet.cpp"
                --inspect-source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_inspect.cpp")
        set_tests_properties(scene3d-render-packet-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_surface_contract_case=valid-current-render-packet.*render_packet_surface_contract_case=missing-primitive-field.*render_packet_surface_contract_case=packet-field-order-drift.*render_packet_surface_contract_case=has-errors-delegation-drift.*render_packet_surface_contract_case=validation-flow-drift.*render_packet_surface_contract_case=empty-packet-diagnostic-drift.*render_packet_surface_contract_case=cli-feature-mask-key-drift.*render_packet_surface_contract_case=cli-feature-name-handoff-drift.*render_packet_surface_contract_verified=true")

        add_test(NAME scene3d-render-packet-matrix-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-matrix-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_matrix_verified=true")
        add_test(NAME scene3d-render-packet-matrix-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_contract.py"
                --matrix-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_smoke.py")
        set_tests_properties(scene3d-render-packet-matrix-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_matrix_contract_case=valid-fake-inspect.*render_packet_matrix_contract_case=inspect-exit-drift.*render_packet_matrix_contract_case=stats-count-drift.*render_packet_matrix_contract_case=stats-extra-key.*render_packet_matrix_contract_case=missing-render-packet.*render_packet_matrix_contract_case=packet-has-errors-drift.*render_packet_matrix_contract_case=missing-primitive.*render_packet_matrix_contract_case=duplicate-primitive.*render_packet_matrix_contract_case=world-transform-drift.*render_packet_matrix_contract_case=feature-mask-drift.*render_packet_matrix_contract_case=feature-name-drift.*render_packet_matrix_contract_case=material-fallback-drift.*render_packet_matrix_contract_verified=true")
    endif()

    add_test(NAME scene3d-sidecar-boxtextured-json
        COMMAND $<TARGET_FILE:pulp-scene3d-sidecar>
            --source "khronos-boxtextured"
            --exported-at "2026-06-03T00:00:00Z"
            --runtime-evidence "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set_tests_properties(scene3d-sidecar-boxtextured-json PROPERTIES
        PASS_REGULAR_EXPRESSION
            "\"schema_version\".*\"source\": \"khronos-boxtextured\".*\"exporter\": \"pulp-scene3d-sidecar\".*\"exported_at\": \"2026-06-03T00:00:00Z\".*\"runtime_evidence\": \"https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927\".*\"diagnostics\": \\[\\].*\"unsupported_features\": \\[\\].*\"runtime_hints\": \\[\\]")

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-sidecar-preflight-boxtextured-clean
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/sidecar_preflight_smoke.py"
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --runtime-evidence "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927"
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-sidecar-preflight-boxtextured-clean PROPERTIES
            PASS_REGULAR_EXPRESSION "BoxTextured sidecar preflight clean")

        add_test(NAME scene3d-sidecar-boxtextured-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar.py"
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                --source "khronos-boxtextured"
                --exported-at "2026-06-03T00:00:00Z"
                --runtime-evidence "https://github.com/danielraffel/pulp/issues/3369#issuecomment-4610177927")
        set_tests_properties(scene3d-sidecar-boxtextured-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_sidecar_verified=boxtextured")

        add_test(NAME scene3d-sidecar-cli-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar_cli_contract.py"
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-sidecar-cli-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_sidecar_cli_contract_case=valid-explicit-provenance.*scene3d_sidecar_cli_contract_case=missing-exported-at.*scene3d_sidecar_cli_contract_case=empty-exported-at.*scene3d_sidecar_cli_contract_case=empty-exporter.*scene3d_sidecar_cli_contract_case=empty-source-defaults-to-path.*scene3d_sidecar_cli_contract_verified=true")

        add_test(NAME scene3d-sidecar-verifier-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar_contract.py"
                --sidecar-verifier "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar.py")
        set_tests_properties(scene3d-sidecar-verifier-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_sidecar_contract_case=valid-fake-sidecar.*scene3d_sidecar_contract_case=missing-runtime-hints.*scene3d_sidecar_contract_case=extra-root-key.*scene3d_sidecar_contract_case=provenance-exporter-drift.*scene3d_sidecar_contract_case=empty-provenance-source.*scene3d_sidecar_contract_case=empty-provenance-exported-at.*scene3d_sidecar_contract_case=empty-runtime-evidence.*scene3d_sidecar_contract_case=non-empty-diagnostics.*scene3d_sidecar_contract_case=malformed-json.*scene3d_sidecar_contract_verified=true")

        add_test(NAME scene3d-sidecar-schema-constants-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_sidecar_schema_constants.py"
                --canonical
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --preflight-matrix
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_matrix.py"
                --boxtextured-sidecar
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar.py")
        set_tests_properties(scene3d-sidecar-schema-constants-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "sidecar_schema_constants_verified=true")

        add_test(NAME scene3d-sidecar-schema-constants-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_sidecar_schema_constants_contract.py"
                --schema-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_sidecar_schema_constants.py"
                --canonical
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --preflight-matrix
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_matrix.py"
                --boxtextured-sidecar
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_sidecar.py")
        set_tests_properties(scene3d-sidecar-schema-constants-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "sidecar_schema_constants_contract_case=valid-current-schema-constants.*sidecar_schema_constants_contract_case=canonical-root-key-drift.*sidecar_schema_constants_contract_case=canonical-missing-constant.*sidecar_schema_constants_contract_case=preflight-root-extra-key.*sidecar_schema_constants_contract_case=preflight-diagnostic-key-drift.*sidecar_schema_constants_contract_case=preflight-missing-constant.*sidecar_schema_constants_contract_case=boxtextured-provenance-key-drift.*sidecar_schema_constants_contract_case=boxtextured-missing-constant.*sidecar_schema_constants_contract_verified=true")

        add_test(NAME scene3d-animation-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/animation_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-animation-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_animation_inspect_verified=true.*scene3d_animation_sidecar_verified=true.*scene3d_animation_preflight_verified=true.*scene3d_animation_contract_verified=true")

        add_test(NAME scene3d-primitive-mode-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/primitive_mode_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-primitive-mode-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_primitive_mode_inspect_verified=true.*scene3d_primitive_mode_sidecar_verified=true.*scene3d_primitive_mode_preflight_verified=true.*scene3d_primitive_mode_contract_verified=true")

        add_test(NAME scene3d-camera-light-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/camera_light_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-camera-light-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_camera_light_inspect_verified=true.*scene3d_camera_light_sidecar_verified=true.*scene3d_camera_light_preflight_verified=true.*scene3d_camera_light_contract_verified=true")

        add_test(NAME scene3d-camera-light-gap-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/camera_light_gap_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-camera-light-gap-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_camera_light_gap_inspect_verified=true.*scene3d_camera_light_gap_sidecar_verified=true.*scene3d_camera_light_gap_preflight_verified=true.*scene3d_camera_light_gap_contract_verified=true")

        add_test(NAME scene3d-light-node-transform-gap-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/light_node_transform_gap_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-light-node-transform-gap-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_light_node_transform_gap_inspect_verified=true.*scene3d_light_node_transform_gap_sidecar_verified=true.*scene3d_light_node_transform_gap_preflight_verified=true.*scene3d_light_node_transform_gap_contract_verified=true")

        add_test(NAME scene3d-advanced-material-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/advanced_material_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-advanced-material-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_advanced_material_inspect_verified=true.*scene3d_advanced_material_sidecar_verified=true.*scene3d_advanced_material_preflight_verified=true.*scene3d_advanced_material_contract_verified=true")

        add_test(NAME scene3d-material-texture-gap-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/material_texture_gap_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-material-texture-gap-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_material_texture_gap_inspect_verified=true.*scene3d_material_texture_gap_sidecar_verified=true.*scene3d_material_texture_gap_preflight_verified=true.*scene3d_material_texture_gap_contract_verified=true")

        add_test(NAME scene3d-texture-payload-format-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/texture_payload_format_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-texture-payload-format-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_texture_payload_format_inspect_verified=true.*scene3d_texture_payload_format_sidecar_verified=true.*scene3d_texture_payload_format_preflight_verified=true.*scene3d_texture_payload_format_contract_verified=true")

        add_test(NAME scene3d-unsupported-mesh-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/unsupported_mesh_contract_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --sidecar-tool $<TARGET_FILE:pulp-scene3d-sidecar>
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-unsupported-mesh-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_unsupported_mesh_inspect_verified=true.*scene3d_unsupported_mesh_sidecar_verified=true.*scene3d_unsupported_mesh_preflight_verified=true.*scene3d_unsupported_mesh_contract_verified=true")

        add_test(NAME scene3d-bake-preflight-runtime-evidence-required
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "export_blocked=true.*runtime_evidence_missing=true"
                --
                $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --require-runtime-evidence
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/missing-runtime-evidence.pulp3d.json")
        set_tests_properties(scene3d-bake-preflight-runtime-evidence-required PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_readiness=blocked.*runtime_evidence_missing=true")

        add_test(NAME scene3d-bake-preflight-runtime-evidence-url-required
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "export_blocked=true.*runtime_evidence_missing=false.*runtime_evidence_url_invalid=true"
                --
                $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --require-runtime-evidence-url
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/native-gap.pulp3d.json")
        set_tests_properties(scene3d-bake-preflight-runtime-evidence-url-required PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_readiness=blocked.*runtime_evidence_url_invalid=true")
    endif()

    add_test(NAME scene3d-bake-preflight-clean
        COMMAND $<TARGET_FILE:pulp-scene3d-bake-preflight>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/clean.pulp3d.json")
    set_tests_properties(scene3d-bake-preflight-clean PROPERTIES
        PASS_REGULAR_EXPRESSION
            "bake_readiness=clean.*export_blocked=false.*native_runtime_has_gaps=false")

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-bake-preflight-shader-material-blocked
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "export_blocked=true.*export_blocker: ShaderMaterial"
                --
                $<TARGET_FILE:pulp-scene3d-bake-preflight>
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/shader-material-blocked.pulp3d.json")
        set_tests_properties(scene3d-bake-preflight-shader-material-blocked PROPERTIES
            PASS_REGULAR_EXPRESSION
                "export_blocked=true.*export_blocker: ShaderMaterial")

        add_test(NAME scene3d-bake-preflight-live-runtime-blocked
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "export_blocked=true.*export_blocker: RawShaderMaterial.*export_blocker: Postprocessing.*export_blocker: RenderTarget.*export_blocker: ArbitraryJSAnimation.*export_blocker: Physics.*export_blocker: EventHandler"
                --
                $<TARGET_FILE:pulp-scene3d-bake-preflight>
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/live-runtime-blocked.pulp3d.json")
        set_tests_properties(scene3d-bake-preflight-live-runtime-blocked PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_readiness=blocked.*export_blockers=6")

        add_test(NAME scene3d-bake-preflight-texture-encoding-blocked
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "texture_encoding_blocked=true.*texture_encoding_blocker: TextureEncoding"
                --
                $<TARGET_FILE:pulp-scene3d-bake-preflight>
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/texture-encoding-blocked.pulp3d.json")
        set_tests_properties(scene3d-bake-preflight-texture-encoding-blocked PROPERTIES
            PASS_REGULAR_EXPRESSION
                "texture_encoding_blocked=true.*texture_encoding_blocker: TextureEncoding")

        add_test(NAME scene3d-bake-preflight-classification-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_classification_surface.py"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/bake_preflight.cpp")
        set_tests_properties(scene3d-bake-preflight-classification-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_classification_surface_verified=true")

        add_test(NAME scene3d-bake-preflight-classification-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_classification_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_classification_surface.py"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/bake_preflight.cpp")
        set_tests_properties(scene3d-bake-preflight-classification-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_classification_surface_contract_case=valid-current-classification.*bake_preflight_classification_surface_contract_case=missing-export-blocker.*bake_preflight_classification_surface_contract_case=extra-export-blocker.*bake_preflight_classification_surface_contract_case=texture-encoding-bucket-drift.*bake_preflight_classification_surface_contract_case=missing-native-gap.*bake_preflight_classification_surface_contract_case=extra-native-gap.*bake_preflight_classification_surface_contract_case=missing-native-prefix.*bake_preflight_classification_surface_contract_case=extra-export-prefix.*bake_preflight_classification_surface_contract_case=renamed-native-prefix.*bake_preflight_classification_surface_contract_verified=true")

        add_test(NAME scene3d-bake-preflight-cli-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_cli_surface.py"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_bake_preflight.cpp")
        set_tests_properties(scene3d-bake-preflight-cli-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_cli_surface_verified=true")

        add_test(NAME scene3d-bake-preflight-cli-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_cli_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_cli_surface.py"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_bake_preflight.cpp")
        set_tests_properties(scene3d-bake-preflight-cli-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_cli_surface_contract_case=valid-current-cli.*bake_preflight_cli_surface_contract_case=usage-option-drift.*bake_preflight_cli_surface_contract_case=read-file-error-drift.*bake_preflight_cli_surface_contract_case=runtime-evidence-flag-drift.*bake_preflight_cli_surface_contract_case=sidecar-parser-drift.*bake_preflight_cli_surface_contract_case=readiness-key-drift.*bake_preflight_cli_surface_contract_case=missing-report-field.*bake_preflight_cli_surface_contract_case=extra-report-field.*bake_preflight_cli_surface_contract_case=feature-row-reason-drift.*bake_preflight_cli_surface_contract_case=blocked-exit-code-drift.*bake_preflight_cli_surface_contract_verified=true")

        add_test(NAME scene3d-bake-preflight-fixture-cli-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_fixture_cli.py"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --fixtures-dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars")
        set_tests_properties(scene3d-bake-preflight-fixture-cli-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_fixture_cli_case=clean.*bake_preflight_fixture_cli_case=shader-material-blocked.*bake_preflight_fixture_cli_case=live-runtime-blocked.*bake_preflight_fixture_cli_case=texture-encoding-blocked.*bake_preflight_fixture_cli_case=native-gap.*bake_preflight_fixture_cli_case=missing-runtime-evidence.*bake_preflight_fixture_cli_verified=true")

        add_test(NAME scene3d-bake-preflight-fixture-cli-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_fixture_cli_contract.py"
                --fixture-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_fixture_cli.py"
                --fixtures-dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars")
        set_tests_properties(scene3d-bake-preflight-fixture-cli-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_fixture_cli_contract_case=valid-current-fixtures.*bake_preflight_fixture_cli_contract_case=clean-exit-drift.*bake_preflight_fixture_cli_contract_case=shader-row-drift.*bake_preflight_fixture_cli_contract_case=live-count-drift.*bake_preflight_fixture_cli_contract_case=texture-row-drift.*bake_preflight_fixture_cli_contract_case=native-gap-exit-drift.*bake_preflight_fixture_cli_contract_case=runtime-evidence-flag-drift.*bake_preflight_fixture_cli_contract_verified=true")

        add_test(NAME scene3d-bake-unsupported-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_unsupported_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/sidecar.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/sidecar.cpp")
        set_tests_properties(scene3d-bake-unsupported-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_unsupported_surface_verified=true")

        add_test(NAME scene3d-bake-unsupported-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_unsupported_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_unsupported_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/sidecar.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/sidecar.cpp")
        set_tests_properties(scene3d-bake-unsupported-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_unsupported_surface_contract_case=valid-current-bake-unsupported.*bake_unsupported_surface_contract_case=missing-enum-value.*bake_unsupported_surface_contract_case=enum-order-drift.*bake_unsupported_surface_contract_case=descriptor-feature-drift.*bake_unsupported_surface_contract_case=descriptor-code-drift.*bake_unsupported_surface_contract_case=descriptor-reason-drift.*bake_unsupported_surface_contract_case=missing-descriptor-case.*bake_unsupported_surface_contract_case=unsupported-feature-reason-drift.*bake_unsupported_surface_contract_case=diagnostic-severity-drift.*bake_unsupported_surface_contract_case=diagnostic-code-drift.*bake_unsupported_surface_contract_verified=true")

        add_test(NAME scene3d-bake-preflight-matrix-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_matrix.py"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --fixture-dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars")
        set_tests_properties(scene3d-bake-preflight-matrix-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_preflight_matrix_verified=true")

        add_test(NAME scene3d-bake-preflight-matrix-fixture-set
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_matrix_fixture_set.py"
                --matrix-verifier "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_matrix.py"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --fixture-dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars")
        set_tests_properties(scene3d-bake-preflight-matrix-fixture-set PROPERTIES
            PASS_REGULAR_EXPRESSION
                "fixture_set_drift_rejected=unlisted.*fixture_set_drift_rejected=missing.*bake_preflight_matrix_fixture_set_verified=true")

        add_test(NAME scene3d-bake-preflight-malformed-sidecars
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_preflight_malformed.py"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-bake-preflight-malformed-sidecars
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "malformed_sidecar_rejected=string-schema-version.*malformed_sidecar_rejected=unsupported-schema-version.*malformed_sidecar_rejected=numeric-provenance.*malformed_sidecar_rejected=empty-provenance-source.*malformed_sidecar_rejected=empty-provenance-exporter.*malformed_sidecar_rejected=empty-provenance-exported-at.*malformed_sidecar_rejected=invalid-diagnostic-severity.*malformed_sidecar_rejected=extra-diagnostic-key.*malformed_sidecar_rejected=missing-root-key.*malformed_sidecar_rejected=nonstring-unsupported-feature.*malformed_sidecar_rejected=nonstring-runtime-hint.*bake_preflight_malformed_verified=true")
    endif()

    add_test(NAME scene3d-bake-preflight-native-gap-warning
        COMMAND $<TARGET_FILE:pulp-scene3d-bake-preflight>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/native-gap.pulp3d.json")
    set_tests_properties(scene3d-bake-preflight-native-gap-warning PROPERTIES
        PASS_REGULAR_EXPRESSION
            "bake_readiness=native_gaps.*export_blocked=false.*native_runtime_has_gaps=true.*native_runtime_gap: TransformAnimation")

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-boxtextured-fixture-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_boxtextured_fixture.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-boxtextured-fixture-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "boxtextured_fixture_sha256=b510eca2e2ef33f62f9ed57d6e7ce2d10ebb2bdebc4a8e59d347719ba81abdf4.*boxtextured_fixture_verified=true")

        add_test(NAME scene3d-boxtextured-fixture-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_boxtextured_fixture_contract.py"
                --fixture-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_boxtextured_fixture.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-boxtextured-fixture-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "boxtextured_fixture_contract_case=valid-current-fixture.*boxtextured_fixture_contract_case=fixture-hash-drift.*boxtextured_fixture_contract_case=readme-source-url-drift.*boxtextured_fixture_contract_case=dependencies-license-drift.*boxtextured_fixture_contract_case=notice-marker-drift.*boxtextured_fixture_contract_case=licensing-path-drift.*boxtextured_fixture_contract_case=manifest-version-drift.*boxtextured_fixture_contract_case=manifest-source-file-drift.*boxtextured_fixture_contract_case=manifest-entry-missing.*boxtextured_fixture_contract_verified=true")

        add_test(NAME scene3d-validate-boxtextured-gltf
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_gltf.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-validate-boxtextured-gltf PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "glTF-Validator errors=0")

        add_test(NAME scene3d-gltf-validator-wrapper-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_validator_wrapper.py"
                --validator-wrapper
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_gltf.py")
        set_tests_properties(scene3d-gltf-validator-wrapper-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "gltf_validator_wrapper_verified=true")

        add_test(NAME scene3d-gltf-validator-wrapper-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_validator_wrapper_contract.py"
                --wrapper-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_validator_wrapper.py"
                --validator-wrapper
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_gltf.py")
        set_tests_properties(scene3d-gltf-validator-wrapper-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "gltf_validator_wrapper_contract_case=valid-current-wrapper.*gltf_validator_wrapper_contract_case=missing-validator-skip-code-drift.*gltf_validator_wrapper_contract_case=require-validator-exit-drift.*gltf_validator_wrapper_contract_case=cli-output-flag-drift.*gltf_validator_wrapper_contract_case=success-summary-label-drift.*gltf_validator_wrapper_contract_case=error-report-exit-drift.*gltf_validator_wrapper_contract_case=invalid-json-exit-drift.*gltf_validator_wrapper_contract_verified=true")

        add_test(NAME scene3d-public-boundary
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_public_scene3d_boundary.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-public-boundary PROPERTIES
            PASS_REGULAR_EXPRESSION
                "public_scene3d_boundary_verified=11 headers")

        add_test(NAME scene3d-public-boundary-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_public_scene3d_boundary_contract.py"
                --boundary-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_public_scene3d_boundary.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-public-boundary-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "public_scene3d_boundary_contract_case=valid-current-boundary.*public_scene3d_boundary_contract_case=missing-public-header.*public_scene3d_boundary_contract_case=extra-public-header.*public_scene3d_boundary_contract_case=parser-include.*public_scene3d_boundary_contract_case=parser-namespace.*public_scene3d_boundary_contract_case=webgpu-include.*public_scene3d_boundary_contract_case=webgpu-type.*public_scene3d_boundary_contract_case=skia-include.*public_scene3d_boundary_contract_case=skia-type.*public_scene3d_boundary_contract_case=view-include.*public_scene3d_boundary_contract_case=view-namespace.*public_scene3d_boundary_contract_case=js-engine.*public_scene3d_boundary_contract_case=live-threejs.*public_scene3d_boundary_contract_verified=true")

        add_test(NAME scene3d-scene-data-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_data_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_data.hpp")
        set_tests_properties(scene3d-scene-data-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_data_surface_verified=true")

        add_test(NAME scene3d-scene-data-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_data_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_data_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_data.hpp")
        set_tests_properties(scene3d-scene-data-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_data_surface_contract_case=valid-current-scene-data.*scene_data_surface_contract_case=primitive-field-drift.*scene_data_surface_contract_case=scene-table-order-drift.*scene_data_surface_contract_case=validation-code-drift.*scene_data_surface_contract_case=empty-contract-drift.*scene_data_surface_contract_case=index-contract-drift.*scene_data_surface_contract_case=severity-name-drift.*scene_data_surface_contract_verified=true")

        add_test(NAME scene3d-scene-graph-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_graph_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_graph.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_graph.cpp")
        set_tests_properties(scene3d-scene-graph-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_graph_surface_verified=true")

        add_test(NAME scene3d-scene-graph-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_graph_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_graph_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_graph.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_graph.cpp")
        set_tests_properties(scene3d-scene-graph-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_graph_surface_contract_case=valid-current-scene-graph.*scene_graph_surface_contract_case=identity-initializer-drift.*scene_graph_surface_contract_case=renderable-field-drift.*scene_graph_surface_contract_case=public-function-drift.*scene_graph_surface_contract_case=cycle-diagnostic-drift.*scene_graph_surface_contract_case=world-multiply-order-drift.*scene_graph_surface_contract_case=child-traversal-drift.*scene_graph_surface_contract_case=root-fallback-drift.*scene_graph_surface_contract_case=matrix-copy-drift.*scene_graph_surface_contract_case=quaternion-normalization-drift.*scene_graph_surface_contract_case=transform-point-translation-drift.*scene_graph_surface_contract_verified=true")

        add_test(NAME scene3d-gltf-loader-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_loader_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/gltf_loader.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/gltf_loader.cpp")
        set_tests_properties(scene3d-gltf-loader-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "gltf_loader_surface_verified=true")

        add_test(NAME scene3d-gltf-loader-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_loader_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_gltf_loader_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/gltf_loader.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/gltf_loader.cpp")
        set_tests_properties(scene3d-gltf-loader-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "gltf_loader_surface_contract_case=valid-current-loader.*gltf_loader_surface_contract_case=header-parser-leak.*gltf_loader_surface_contract_case=callback-signature-drift.*gltf_loader_surface_contract_case=load-pipeline-drift.*gltf_loader_surface_contract_case=diagnostic-code-drift.*gltf_loader_surface_contract_case=draco-flow-drift.*gltf_loader_surface_contract_case=parser-extension-drift.*gltf_loader_surface_contract_verified=true")

        add_test(NAME scene3d-native-source-boundary
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_source_boundary.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-native-source-boundary PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_source_boundary_verified=14 sources")

        add_test(NAME scene3d-native-source-boundary-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_source_boundary_contract.py"
                --boundary-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_source_boundary.py"
                --repo-root "${CMAKE_SOURCE_DIR}")
        set_tests_properties(scene3d-native-source-boundary-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_source_boundary_contract_case=valid-current-source-boundary.*native_source_boundary_contract_case=unexpected-source.*native_source_boundary_contract_case=missing-source.*native_source_boundary_contract_case=view-include.*native_source_boundary_contract_case=widget-bridge.*native_source_boundary_contract_case=js-engine.*native_source_boundary_contract_case=live-loader.*native_source_boundary_contract_case=scene-parser-leak.*native_source_boundary_contract_case=scene-gpu-leak.*native_source_boundary_contract_case=renderer-parser-leak.*native_source_boundary_contract_case=loader-parser-allowed.*native_source_boundary_contract_case=loader-gpu-rejected.*native_source_boundary_contract_case=renderer-gpu-allowed.*native_source_boundary_contract_verified=true")

        add_test(NAME scene3d-renderer-golden-manifest
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                --cpp-test "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer3d_shared.hpp")
        set_tests_properties(scene3d-renderer-golden-manifest PROPERTIES
            PASS_REGULAR_EXPRESSION "Renderer3D golden manifest entries=2")

        add_test(NAME scene3d-renderer-golden-manifest-malformed
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_golden_manifest_malformed.py"
                --validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py"
                --manifest
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                --cpp-test "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer3d_shared.hpp")
        set_tests_properties(scene3d-renderer-golden-manifest-malformed
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_golden_manifest_malformed_rejected=missing-root-key.*renderer_golden_manifest_malformed_rejected=extra-root-key.*renderer_golden_manifest_malformed_rejected=invalid-status.*renderer_golden_manifest_malformed_rejected=duplicate-entry-id.*renderer_golden_manifest_malformed_rejected=missing-entry-key.*renderer_golden_manifest_malformed_rejected=extra-entry-key.*renderer_golden_manifest_malformed_rejected=cpp-constant-missing.*renderer_golden_manifest_malformed_rejected=cpp-constant-mismatch.*renderer_golden_manifest_malformed_rejected=interim-scope-drift.*renderer_golden_manifest_malformed_rejected=interim-backend-drift.*renderer_golden_manifest_malformed_rejected=missing-software-adapter-key.*renderer_golden_manifest_malformed_rejected=extra-software-adapter-key.*renderer_golden_manifest_malformed_rejected=interim-software-pixel-claim.*renderer_golden_manifest_malformed_rejected=hardcoded-scene-data-consumed.*renderer_golden_manifest_malformed_rejected=hardcoded-primitive-count.*renderer_golden_manifest_malformed_rejected=scene-data-not-consumed.*renderer_golden_manifest_malformed_rejected=scene-data-empty-pipeline-cache.*renderer_golden_manifest_malformed_rejected=scene-data-cache-entry-overflow.*renderer_golden_manifest_malformed_rejected=scene-data-cache-accounting-drift.*renderer_golden_manifest_malformed_rejected=scene-data-source-hardcoded.*renderer_golden_manifest_malformed_verified=true")

        add_test(NAME scene3d-renderer-golden-software-adapter-required
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 1
                --expect-regex "pixel-producing software adapter"
                --
                ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                --cpp-test "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer3d_shared.hpp"
                --require-pixel-software-adapter)
        set_tests_properties(scene3d-renderer-golden-software-adapter-required
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "final Phase 7 goldens require a pixel-producing software adapter")

        add_test(NAME scene3d-renderer-golden-final-adapter-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_golden_manifest_final_adapter.py"
                --validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py")
        set_tests_properties(scene3d-renderer-golden-final-adapter-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_golden_manifest_final_adapter_case=valid-final-software-adapter.*renderer_golden_manifest_final_adapter_case=null-backend-rejected.*renderer_golden_manifest_final_adapter_case=missing-entry-rejected.*renderer_golden_manifest_final_adapter_case=scope-mismatch-rejected.*renderer_golden_manifest_final_adapter_case=final-status-without-pixel-adapter-rejected.*renderer_golden_manifest_final_adapter_verified=true")

        add_test(NAME scene3d-verify-bake-artifact-clean
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --asset "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                --sidecar "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/clean.pulp3d.json"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --require-extensions
                --require-runtime-evidence
                --require-runtime-evidence-url)
        set_tests_properties(scene3d-verify-bake-artifact-clean PROPERTIES
            PASS_REGULAR_EXPRESSION
                "asset_extension_supported=true.*sidecar_extension_supported=true.*sidecar_root_keys_valid=true.*sidecar_schema_supported=true.*sidecar_provenance_valid=true.*sidecar_provenance_keys_valid=true.*sidecar_provenance_values_valid=true.*sidecar_diagnostics_valid=true.*sidecar_unsupported_features_valid=true.*sidecar_runtime_hints_valid=true.*sidecar_source_present=true.*sidecar_exporter_present=true.*sidecar_exported_at_present=true.*runtime_evidence_present=true.*runtime_evidence_url_valid=true.*gltf_validator=(passed|skipped).*preflight_fields_valid=true.*preflight_row_counts_valid=true.*preflight_sidecar_counts_valid=true.*preflight_readiness_consistent=true.*preflight_exit_code_consistent=true.*unsupported_features_classified=true.*bake_readiness=clean.*bake_artifact_verified=true")

        add_test(NAME scene3d-verify-bake-artifact-missing-runtime-evidence
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "asset_extension_supported=true.*sidecar_extension_supported=true.*sidecar_root_keys_valid=true.*sidecar_schema_supported=true.*sidecar_provenance_valid=true.*sidecar_provenance_keys_valid=true.*sidecar_provenance_values_valid=true.*sidecar_diagnostics_valid=true.*sidecar_unsupported_features_valid=true.*sidecar_runtime_hints_valid=true.*sidecar_source_present=true.*sidecar_exporter_present=true.*sidecar_exported_at_present=true.*runtime_evidence_present=false.*preflight_fields_valid=true.*bake_readiness=blocked.*runtime_evidence_missing=true.*runtime_evidence_url_invalid=false.*bake_artifact_verified=false"
                --
                ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --asset "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                --sidecar "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/missing-runtime-evidence.pulp3d.json"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --require-extensions
                --require-runtime-evidence)
        set_tests_properties(scene3d-verify-bake-artifact-missing-runtime-evidence
            PROPERTIES
            PASS_REGULAR_EXPRESSION "bake_artifact_verified=false")

        add_test(NAME scene3d-verify-bake-artifact-extension-required
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                --expect-code 2
                --expect-regex "asset_extension_supported=false.*error=asset must have .glb or .gltf extension"
                --
                ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --asset "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/README.md"
                --sidecar "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/clean.pulp3d.json"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>
                --require-extensions)
        set_tests_properties(scene3d-verify-bake-artifact-extension-required
            PROPERTIES
            PASS_REGULAR_EXPRESSION "asset_extension_supported=false")

        add_test(NAME scene3d-verify-bake-artifact-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact_contract.py"
                --artifact-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_bake_artifact.py"
                --asset "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                --base-sidecar "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/sidecars/clean.pulp3d.json"
                --preflight-tool $<TARGET_FILE:pulp-scene3d-bake-preflight>)
        set_tests_properties(scene3d-verify-bake-artifact-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "bake_artifact_contract_rejected=sidecar-extension.*bake_artifact_contract_rejected=extra-root-key.*bake_artifact_contract_rejected=nonstring-runtime-evidence.*bake_artifact_contract_rejected=invalid-runtime-evidence-url.*bake_artifact_contract_rejected=empty-provenance-source.*bake_artifact_contract_rejected=empty-provenance-exporter.*bake_artifact_contract_rejected=empty-provenance-exported-at.*bake_artifact_contract_rejected=invalid-diagnostic-severity.*bake_artifact_contract_rejected=nonstring-runtime-hint.*bake_artifact_contract_rejected=unclassified-unsupported-feature.*bake_artifact_contract_rejected=preflight-row-count-drift.*bake_artifact_contract_rejected=preflight-sidecar-count-drift.*bake_artifact_contract_rejected=preflight-readiness-drift.*bake_artifact_contract_rejected=preflight-exit-code-drift.*bake_artifact_contract_verified=true")
    endif()
endif()

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
    # Graphite backend. The .skp round-trip cases include
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

    # HeadlessSurface CI wrapper: one-call offscreen Dawn/Skia helper
    # for golden-test fixtures. Compiles on every
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

    # GPU compute tests.
    add_executable(pulp-test-gpu-compute test_gpu_compute.cpp)
    target_link_libraries(pulp-test-gpu-compute PRIVATE pulp::render pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-gpu-compute ${PULP_GPU_TEST_DISCOVERY_ARGS})

    if(PULP_ENABLE_SCENE3D)
        set(_pulp_renderer3d_test_libs pulp::render pulp::scene)
        set(_pulp_renderer3d_test_definitions PULP_TEST_SCENE3D_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d")
        if(PULP_HAS_DRACO)
            list(APPEND _pulp_renderer3d_test_libs draco::draco)
            list(APPEND _pulp_renderer3d_test_definitions PULP_TEST_HAS_DRACO=1)
            list(APPEND _pulp_renderer3d_test_includes "${draco_SOURCE_DIR}/src" "${draco_BINARY_DIR}" "${CMAKE_BINARY_DIR}")
        endif()
        pulp_add_test_suite(pulp-test-renderer3d
            SOURCES test_renderer3d_loading.cpp test_renderer3d_scene.cpp test_renderer3d_materials.cpp
            LIBRARIES ${_pulp_renderer3d_test_libs}
            COMPILE_DEFINITIONS ${_pulp_renderer3d_test_definitions}
            INCLUDE_DIRS ${_pulp_renderer3d_test_includes}
            DISCOVERY_ARGS ${PULP_GPU_TEST_DISCOVERY_ARGS})

        if(PULP_HAS_SKIA AND TARGET pulp-renderer3d-probe)
            add_test(NAME scene3d-renderer-probe-hardcoded
                COMMAND $<TARGET_FILE:pulp-renderer3d-probe>
                    --scene hardcoded
                    --width 128
                    --height 128
                    --adapter-scope macos_default_metal)
            set_tests_properties(scene3d-renderer-probe-hardcoded PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "scene=hardcoded.*success=true.*readback_completed=true.*rgba_fingerprint=15925498132503966243.*final_software_golden_eligible=false")

            add_test(NAME scene3d-renderer-probe-boxtextured
                COMMAND $<TARGET_FILE:pulp-renderer3d-probe>
                    --scene boxtextured
                    --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                    --width 128
                    --height 128
                    --adapter-scope macos_default_metal)
            set_tests_properties(scene3d-renderer-probe-boxtextured PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "scene=boxtextured.*success=true.*scene_data_consumed=true.*readback_completed=true.*rgba_fingerprint=5845745157752120258.*final_software_golden_eligible=false")

            if(Python3_Interpreter_FOUND)
                add_test(NAME scene3d-renderer-probe-hardcoded-manifest
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                        --entry-id hardcoded_textured_cube
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal)
                set_tests_properties(scene3d-renderer-probe-hardcoded-manifest
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_verified=hardcoded_textured_cube")

                add_test(NAME scene3d-renderer-probe-boxtextured-manifest
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                        --entry-id official_boxtextured_fixture
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene boxtextured
                        --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal)
        set_tests_properties(scene3d-renderer-probe-boxtextured-manifest
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_verified=official_boxtextured_fixture")

        add_test(NAME scene3d-renderer-probe-material-floor-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-renderer-probe-material-floor-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_material_floor_verified=boxtextured")

        add_test(NAME scene3d-renderer-probe-non-base-texture-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-non-base-texture-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_non_base_texture_route_verified=true")

        add_test(NAME scene3d-renderer-probe-non-base-texture-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-non-base-texture-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_non_base_texture_route_contract_case=valid-current-route.*renderer_probe_non_base_texture_route_contract_case=missing-transform-applied-field.*renderer_probe_non_base_texture_route_contract_case=metallic-roughness-route-drift.*renderer_probe_non_base_texture_route_contract_case=occlusion-strength-drift.*renderer_probe_non_base_texture_route_contract_case=transform-deferral-leak.*renderer_probe_non_base_texture_route_contract_case=texcoord1-deferral-leak.*renderer_probe_non_base_texture_route_contract_case=pixel-output-drift.*renderer_probe_non_base_texture_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-derived-normal-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-derived-normal-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_derived_normal_route_verified=true")

        add_test(NAME scene3d-renderer-probe-derived-normal-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-derived-normal-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_derived_normal_route_contract_case=valid-derived-normal-route.*renderer_probe_derived_normal_route_contract_case=missing-derived-tangent-field.*renderer_probe_derived_normal_route_contract_case=normal-texture-route-drift.*renderer_probe_derived_normal_route_contract_case=normal-scale-route-drift.*renderer_probe_derived_normal_route_contract_case=normal-texture-deferral-leak.*renderer_probe_derived_normal_route_contract_case=normal-scale-deferral-leak.*renderer_probe_derived_normal_route_contract_case=pixel-output-drift.*renderer_probe_derived_normal_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-blend-sort-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-alpha-blend-sort-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_blend_sort_route_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-blend-sort-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-alpha-blend-sort-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_blend_sort_route_contract_case=valid-alpha-blend-sort-route.*renderer_probe_alpha_blend_sort_route_contract_case=primitive-count-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-blend-route-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-depth-write-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-sort-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-mask-leak.*renderer_probe_alpha_blend_sort_route_contract_case=pixel-output-drift.*renderer_probe_alpha_blend_sort_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-mask-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-alpha-mask-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_mask_route_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-mask-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-alpha-mask-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_mask_route_contract_case=valid-alpha-mask-route.*renderer_probe_alpha_mask_route_contract_case=alpha-mask-route-drift.*renderer_probe_alpha_mask_route_contract_case=texture-decode-drift.*renderer_probe_alpha_mask_route_contract_case=base-color-srgb-drift.*renderer_probe_alpha_mask_route_contract_case=alpha-blend-leak.*renderer_probe_alpha_mask_route_contract_case=fallback-texture-leak.*renderer_probe_alpha_mask_route_contract_case=pixel-output-drift.*renderer_probe_alpha_mask_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-vertex-color-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-vertex-color-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_vertex_color_route_verified=true")

        add_test(NAME scene3d-renderer-probe-vertex-color-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-vertex-color-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_vertex_color_route_contract_case=valid-vertex-color-route.*renderer_probe_vertex_color_route_contract_case=vertex-color-route-drift.*renderer_probe_vertex_color_route_contract_case=fallback-texture-route-drift.*renderer_probe_vertex_color_route_contract_case=texture-decode-leak.*renderer_probe_vertex_color_route_contract_case=alpha-mask-leak.*renderer_probe_vertex_color_route_contract_case=primitive-count-drift.*renderer_probe_vertex_color_route_contract_case=pixel-output-drift.*renderer_probe_vertex_color_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-double-sided-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-double-sided-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_double_sided_route_verified=true")

        add_test(NAME scene3d-renderer-probe-double-sided-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-double-sided-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_double_sided_route_contract_case=valid-double-sided-route.*renderer_probe_double_sided_route_contract_case=double-sided-route-drift.*renderer_probe_double_sided_route_contract_case=fallback-texture-route-drift.*renderer_probe_double_sided_route_contract_case=texture-decode-leak.*renderer_probe_double_sided_route_contract_case=alpha-blend-leak.*renderer_probe_double_sided_route_contract_case=primitive-count-drift.*renderer_probe_double_sided_route_contract_case=pixel-output-drift.*renderer_probe_double_sided_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-unlit-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-unlit-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unlit_route_verified=true")

        add_test(NAME scene3d-renderer-probe-unlit-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-unlit-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unlit_route_contract_case=valid-unlit-route.*renderer_probe_unlit_route_contract_case=unlit-route-drift.*renderer_probe_unlit_route_contract_case=fallback-texture-route-drift.*renderer_probe_unlit_route_contract_case=texture-decode-leak.*renderer_probe_unlit_route_contract_case=double-sided-leak.*renderer_probe_unlit_route_contract_case=alpha-blend-leak.*renderer_probe_unlit_route_contract_case=primitive-count-drift.*renderer_probe_unlit_route_contract_case=pixel-output-drift.*renderer_probe_unlit_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-emissive-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-emissive-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_emissive_route_verified=true")

        add_test(NAME scene3d-renderer-probe-emissive-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-emissive-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_emissive_route_contract_case=valid-emissive-route.*renderer_probe_emissive_route_contract_case=emissive-factor-route-drift.*renderer_probe_emissive_route_contract_case=emissive-strength-route-drift.*renderer_probe_emissive_route_contract_case=fallback-texture-route-drift.*renderer_probe_emissive_route_contract_case=texture-decode-leak.*renderer_probe_emissive_route_contract_case=emissive-texture-leak.*renderer_probe_emissive_route_contract_case=unlit-leak.*renderer_probe_emissive_route_contract_case=primitive-count-drift.*renderer_probe_emissive_route_contract_case=pixel-output-drift.*renderer_probe_emissive_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-sampler-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-sampler-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_sampler_route_verified=true")

        add_test(NAME scene3d-renderer-probe-sampler-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-sampler-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_sampler_route_contract_case=valid-sampler-route.*renderer_probe_sampler_route_contract_case=sampler-route-drift.*renderer_probe_sampler_route_contract_case=sampler-clamp-s-drift.*renderer_probe_sampler_route_contract_case=sampler-clamp-t-drift.*renderer_probe_sampler_route_contract_case=sampler-linear-drift.*renderer_probe_sampler_route_contract_case=texture-decode-drift.*renderer_probe_sampler_route_contract_case=fallback-texture-leak.*renderer_probe_sampler_route_contract_case=primitive-count-drift.*renderer_probe_sampler_route_contract_case=pixel-output-drift.*renderer_probe_sampler_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-mipmap-sampler-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-mipmap-sampler-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_mipmap_sampler_route_verified=true")

        add_test(NAME scene3d-renderer-probe-mipmap-sampler-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-mipmap-sampler-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_mipmap_sampler_route_contract_case=valid-mipmap-sampler-route.*renderer_probe_mipmap_sampler_route_contract_case=mipmap-downgrade-drift.*renderer_probe_mipmap_sampler_route_contract_case=sampler-route-drift.*renderer_probe_mipmap_sampler_route_contract_case=sampler-linear-drift.*renderer_probe_mipmap_sampler_route_contract_case=texture-decode-drift.*renderer_probe_mipmap_sampler_route_contract_case=fallback-texture-leak.*renderer_probe_mipmap_sampler_route_contract_case=primitive-count-drift.*renderer_probe_mipmap_sampler_route_contract_case=pixel-output-drift.*renderer_probe_mipmap_sampler_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-base-color-transform-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-base-color-transform-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_base_color_transform_route_verified=true")

        add_test(NAME scene3d-renderer-probe-base-color-transform-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-base-color-transform-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_base_color_transform_route_contract_case=valid-base-color-transform-route.*renderer_probe_base_color_transform_route_contract_case=base-color-transform-drift.*renderer_probe_base_color_transform_route_contract_case=base-color-texcoord1-drift.*renderer_probe_base_color_transform_route_contract_case=texture-decode-drift.*renderer_probe_base_color_transform_route_contract_case=fallback-texture-leak.*renderer_probe_base_color_transform_route_contract_case=non-base-transform-leak.*renderer_probe_base_color_transform_route_contract_case=primitive-count-drift.*renderer_probe_base_color_transform_route_contract_case=pixel-output-drift.*renderer_probe_base_color_transform_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-resource-floor-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-resource-floor-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_resource_floor_route_verified=true")

        add_test(NAME scene3d-renderer-probe-resource-floor-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-resource-floor-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_resource_floor_route_contract_case=valid-resource-floor-route.*renderer_probe_resource_floor_route_contract_case=depth-target-drift.*renderer_probe_resource_floor_route_contract_case=color-target-drift.*renderer_probe_resource_floor_route_contract_case=vertex-buffer-drift.*renderer_probe_resource_floor_route_contract_case=index-buffer-drift.*renderer_probe_resource_floor_route_contract_case=uniform-buffer-drift.*renderer_probe_resource_floor_route_contract_case=metallic-roughness-factor-drift.*renderer_probe_resource_floor_route_contract_case=texture-decode-leak.*renderer_probe_resource_floor_route_contract_case=primitive-count-drift.*renderer_probe_resource_floor_route_contract_case=pixel-output-drift.*renderer_probe_resource_floor_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-adapter-selection-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-adapter-selection-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_adapter_selection_route_case=null-backend.*renderer_probe_adapter_selection_route_case=forced-fallback.*renderer_probe_adapter_selection_route_verified=true")

        add_test(NAME scene3d-renderer-probe-adapter-selection-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-adapter-selection-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_adapter_selection_route_contract_case=valid-adapter-selection-route.*renderer_probe_adapter_selection_route_contract_case=null-backend-request-drift.*renderer_probe_adapter_selection_route_contract_case=null-backend-type-drift.*renderer_probe_adapter_selection_route_contract_case=null-backend-pixel-drift.*renderer_probe_adapter_selection_route_contract_case=fallback-request-drift.*renderer_probe_adapter_selection_route_contract_case=fallback-null-leak.*renderer_probe_adapter_selection_route_contract_case=fallback-command-leak.*renderer_probe_adapter_selection_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-directional-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-directional-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_directional_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-directional-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-directional-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_directional_light_route_contract_case=valid-directional-light-route.*renderer_probe_directional_light_route_contract_case=directional-light-drift.*renderer_probe_directional_light_route_contract_case=geometry-normal-drift.*renderer_probe_directional_light_route_contract_case=directional-transform-leak.*renderer_probe_directional_light_route_contract_case=point-light-leak.*renderer_probe_directional_light_route_contract_case=spot-light-leak.*renderer_probe_directional_light_route_contract_case=primitive-count-drift.*renderer_probe_directional_light_route_contract_case=pixel-output-drift.*renderer_probe_directional_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-point-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-point-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_point_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-point-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-point-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_point_light_route_contract_case=valid-point-light-route.*renderer_probe_point_light_route_contract_case=point-light-drift.*renderer_probe_point_light_route_contract_case=point-range-drift.*renderer_probe_point_light_route_contract_case=point-deferred-leak.*renderer_probe_point_light_route_contract_case=range-deferred-leak.*renderer_probe_point_light_route_contract_case=directional-light-leak.*renderer_probe_point_light_route_contract_case=spot-light-leak.*renderer_probe_point_light_route_contract_case=primitive-count-drift.*renderer_probe_point_light_route_contract_case=pixel-output-drift.*renderer_probe_point_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-spot-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-spot-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_spot_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-spot-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-spot-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_spot_light_route_contract_case=valid-spot-light-route.*renderer_probe_spot_light_route_contract_case=spot-light-drift.*renderer_probe_spot_light_route_contract_case=spot-range-drift.*renderer_probe_spot_light_route_contract_case=spot-deferred-leak.*renderer_probe_spot_light_route_contract_case=spot-cone-deferred-leak.*renderer_probe_spot_light_route_contract_case=range-deferred-leak.*renderer_probe_spot_light_route_contract_case=point-light-leak.*renderer_probe_spot_light_route_contract_case=directional-light-leak.*renderer_probe_spot_light_route_contract_case=primitive-count-drift.*renderer_probe_spot_light_route_contract_case=pixel-output-drift.*renderer_probe_spot_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-transform-animation-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-transform-animation-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_transform_animation_route_verified=true")

        add_test(NAME scene3d-renderer-probe-transform-animation-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-transform-animation-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_transform_animation_route_contract_case=valid-transform-animation-route.*renderer_probe_transform_animation_route_contract_case=transform-initial-pose-drift.*renderer_probe_transform_animation_route_contract_case=transform-deferred-drift.*renderer_probe_transform_animation_route_contract_case=camera-leak.*renderer_probe_transform_animation_route_contract_case=directional-light-leak.*renderer_probe_transform_animation_route_contract_case=skinning-deferred-leak.*renderer_probe_transform_animation_route_contract_case=primitive-count-drift.*renderer_probe_transform_animation_route_contract_case=pixel-output-drift.*renderer_probe_transform_animation_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-advanced-material-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-advanced-material-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_advanced_material_route_verified=true")

        add_test(NAME scene3d-renderer-probe-advanced-material-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-advanced-material-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_advanced_material_route_contract_case=valid-advanced-material-route.*renderer_probe_advanced_material_route_contract_case=advanced-material-deferral-drift.*renderer_probe_advanced_material_route_contract_case=double-sided-route-drift.*renderer_probe_advanced_material_route_contract_case=texture-decode-leak.*renderer_probe_advanced_material_route_contract_case=unlit-leak.*renderer_probe_advanced_material_route_contract_case=camera-leak.*renderer_probe_advanced_material_route_contract_case=primitive-count-drift.*renderer_probe_advanced_material_route_contract_case=pixel-output-drift.*renderer_probe_advanced_material_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-unsupported-mesh-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-unsupported-mesh-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unsupported_mesh_route_verified=true")

        add_test(NAME scene3d-renderer-probe-unsupported-mesh-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-unsupported-mesh-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unsupported_mesh_route_contract_case=valid-unsupported-mesh-route.*renderer_probe_unsupported_mesh_route_contract_case=skinning-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=morph-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=gpu-instancing-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=animation-leak.*renderer_probe_unsupported_mesh_route_contract_case=advanced-material-leak.*renderer_probe_unsupported_mesh_route_contract_case=primitive-count-drift.*renderer_probe_unsupported_mesh_route_contract_case=pixel-output-drift.*renderer_probe_unsupported_mesh_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-perspective-camera-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-perspective-camera-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_perspective_camera_route_verified=true")

        add_test(NAME scene3d-renderer-probe-perspective-camera-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-perspective-camera-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_perspective_camera_route_contract_case=valid-perspective-camera-route.*renderer_probe_perspective_camera_route_contract_case=perspective-camera-drift.*renderer_probe_perspective_camera_route_contract_case=camera-translation-drift.*renderer_probe_perspective_camera_route_contract_case=camera-aspect-drift.*renderer_probe_perspective_camera_route_contract_case=camera-depth-drift.*renderer_probe_perspective_camera_route_contract_case=orthographic-camera-leak.*renderer_probe_perspective_camera_route_contract_case=camera-deferred-leak.*renderer_probe_perspective_camera_route_contract_case=primitive-count-drift.*renderer_probe_perspective_camera_route_contract_case=pixel-output-drift.*renderer_probe_perspective_camera_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-orthographic-camera-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-orthographic-camera-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_orthographic_camera_route_verified=true")

        add_test(NAME scene3d-renderer-probe-orthographic-camera-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-orthographic-camera-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_orthographic_camera_route_contract_case=valid-orthographic-camera-route.*renderer_probe_orthographic_camera_route_contract_case=orthographic-camera-drift.*renderer_probe_orthographic_camera_route_contract_case=perspective-camera-leak.*renderer_probe_orthographic_camera_route_contract_case=camera-depth-drift.*renderer_probe_orthographic_camera_route_contract_case=camera-deferred-leak.*renderer_probe_orthographic_camera_route_contract_case=primitive-count-drift.*renderer_probe_orthographic_camera_route_contract_case=pixel-output-drift.*renderer_probe_orthographic_camera_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-material-floor-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_contract.py"
                --material-floor-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_smoke.py")
        set_tests_properties(scene3d-renderer-probe-material-floor-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_material_floor_contract_case=valid-current-material-floor.*renderer_probe_material_floor_contract_case=missing-base-color-srgb-field.*renderer_probe_material_floor_contract_case=texture-upload-drift.*renderer_probe_material_floor_contract_case=scene-identity-drift.*renderer_probe_material_floor_contract_case=material-factor-drift.*renderer_probe_material_floor_contract_case=geometry-normal-drift.*renderer_probe_material_floor_contract_case=pixel-output-drift.*renderer_probe_material_floor_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-verifier-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json")
        set_tests_properties(scene3d-renderer-probe-verifier-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_contract_case=valid-fake-probe.*renderer_probe_contract_case=missing-resource-field.*renderer_probe_contract_case=scene-data-consumption-drift.*renderer_probe_contract_case=pixel-output-drift.*renderer_probe_contract_case=coverage-floor-drift.*renderer_probe_contract_case=manifest-missing-entry-field.*renderer_probe_contract_case=manifest-extra-entry-field.*renderer_probe_contract_case=manifest-software-adapter-drift.*renderer_probe_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-public-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface.py"
                --renderer-header
                "${CMAKE_SOURCE_DIR}/core/render/include/pulp/render/renderer3d.hpp"
                --probe-source
                "${CMAKE_SOURCE_DIR}/core/render/src/renderer3d_probe.cpp"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
        set_tests_properties(scene3d-renderer-probe-public-surface-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_public_surface_verified=79 bools")

        add_test(NAME scene3d-renderer-probe-public-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface.py"
                --renderer-header
                "${CMAKE_SOURCE_DIR}/core/render/include/pulp/render/renderer3d.hpp"
                --probe-source
                "${CMAKE_SOURCE_DIR}/core/render/src/renderer3d_probe.cpp"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
        set_tests_properties(scene3d-renderer-probe-public-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_public_surface_contract_case=valid-current-surface.*renderer_probe_public_surface_contract_case=header-bool-removed.*renderer_probe_public_surface_contract_case=probe-print-removed.*renderer_probe_public_surface_contract_case=verifier-field-removed.*renderer_probe_public_surface_contract_case=probe-key-mismatch.*renderer_probe_public_surface_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-fake-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --probe-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --final-eligibility-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
        set_tests_properties(scene3d-renderer-probe-fake-surface-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_fake_surface_verified=probe-contract-lines.*renderer_probe_fake_surface_verified=final-eligibility-true.*renderer_probe_fake_surface_verified=final-eligibility-false.*renderer_probe_fake_surfaces_verified=true")

        add_test(NAME scene3d-renderer-probe-fake-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --probe-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --final-eligibility-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
        set_tests_properties(scene3d-renderer-probe-fake-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_fake_surface_contract_case=valid-current-fake-surfaces.*renderer_probe_fake_surface_contract_case=probe-contract-missing-field.*renderer_probe_fake_surface_contract_case=final-eligibility-missing-resource-field.*renderer_probe_fake_surface_contract_case=final-eligibility-missing-final-field.*renderer_probe_fake_surface_contract_case=probe-field-added-without-fakes.*renderer_probe_fake_surface_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-manifest-schema-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest-validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py")
        set_tests_properties(scene3d-renderer-probe-manifest-schema-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_manifest_schema_verified=true")

        add_test(NAME scene3d-renderer-probe-manifest-schema-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema_contract.py"
                --schema-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest-validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py")
        set_tests_properties(scene3d-renderer-probe-manifest-schema-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_manifest_schema_contract_case=valid-current-manifest-schema.*renderer_probe_manifest_schema_contract_case=probe-manifest-key-drift.*renderer_probe_manifest_schema_contract_case=probe-entry-extra-key.*renderer_probe_manifest_schema_contract_case=probe-missing-entry-constant.*renderer_probe_manifest_schema_contract_case=validator-software-key-drift.*renderer_probe_manifest_schema_contract_case=validator-missing-manifest-constant.*renderer_probe_manifest_schema_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-route-inventory-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --tools-dir
                "${CMAKE_SOURCE_DIR}/tools/scene3d"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
        set_tests_properties(scene3d-renderer-probe-route-inventory-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_route_inventory_verified=74 fields 42 route files 21 route pairs")

        add_test(NAME scene3d-renderer-probe-route-inventory-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory_contract.py"
                --inventory-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --tools-dir
                "${CMAKE_SOURCE_DIR}/tools/scene3d"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
        set_tests_properties(scene3d-renderer-probe-route-inventory-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_route_inventory_contract_case=valid-current-route-inventory.*renderer_probe_route_inventory_contract_case=missing-route-field-mention.*renderer_probe_route_inventory_contract_case=missing-route-ctest-registration.*renderer_probe_route_inventory_contract_case=missing-route-negative-contract.*renderer_probe_route_inventory_contract_case=missing-route-positive-smoke.*renderer_probe_route_inventory_contract_verified=true")

        add_test(NAME scene3d-native-slice-handoff-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff.py"
                --repo-root
                "${CMAKE_SOURCE_DIR}"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
                --doc-file
                "${CMAKE_SOURCE_DIR}/docs/analysis/threejs-gltf-bake-native-slice.md"
                --plan-file
                "${CMAKE_SOURCE_DIR}/planning/threejs-webgpu-gltf-bake-plan.md")
        set_tests_properties(scene3d-native-slice-handoff-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_slice_handoff_verified=20 artifacts 21 tests 15 doc tokens 1 ctest tokens 7 plan tokens 8 forbidden plan tokens 1 file tokens")

        add_test(NAME scene3d-native-slice-handoff-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff_contract.py"
                --handoff-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff.py"
                --repo-root
                "${CMAKE_SOURCE_DIR}"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
                --doc-file
                "${CMAKE_SOURCE_DIR}/docs/analysis/threejs-gltf-bake-native-slice.md")
        set_tests_properties(scene3d-native-slice-handoff-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_slice_handoff_contract_case=valid-current-handoff.*native_slice_handoff_contract_case=missing-renderer-probe-artifact.*native_slice_handoff_contract_case=missing-boxtextured-ctest.*native_slice_handoff_contract_case=missing-runtime-issue-link.*native_slice_handoff_contract_case=missing-runtime-evidence-comment-link.*native_slice_handoff_contract_case=missing-runtime-evidence-ctest-link.*native_slice_handoff_contract_case=missing-clean-sidecar-runtime-evidence-link.*native_slice_handoff_contract_case=missing-plan-runtime-boundary.*native_slice_handoff_contract_case=stale-plan-live-gltf-spike.*native_slice_handoff_contract_case=stale-plan-live-exporter-implementation.*native_slice_handoff_contract_case=missing-url-gate-doc.*native_slice_handoff_contract_case=missing-final-gate-doc.*native_slice_handoff_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-final-eligibility-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py"
                --probe-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
                set_tests_properties(scene3d-renderer-probe-final-eligibility-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_final_eligibility_verified=true.*renderer_probe_final_eligibility_rejected=false_probe.*renderer_probe_final_eligibility_contract_verified=true")

                add_test(NAME scene3d-renderer-probe-final-eligibility-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility_contract.py"
                        --probe-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --final-eligibility-contract
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
                set_tests_properties(scene3d-renderer-probe-final-eligibility-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_final_eligibility_contract_case=valid-final-software-entry.*renderer_probe_final_eligibility_contract_case=interim-status-rejects-final-true.*renderer_probe_final_eligibility_contract_case=non-pixel-software-rejects-final-true.*renderer_probe_final_eligibility_contract_case=missing-entry-id-rejects-final-true.*renderer_probe_final_eligibility_contract_case=final-entry-rejects-final-false.*renderer_probe_final_eligibility_contract_verified=true")

                add_test(NAME scene3d-renderer-probe-null-backend-api
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                        --expect-code 2
                        --expect-regex "adapter_backend_type=Null.*adapter_backend_preference=null.*null_backend_requested=true.*pixel_output_produced=false.*final_software_golden_eligible=false"
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 32
                        --height 32
                        --adapter-scope dawn_null_api
                        --adapter-backend null)
                set_tests_properties(scene3d-renderer-probe-null-backend-api
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "adapter_backend_type=Null.*pixel_output_produced=false")

                add_test(NAME scene3d-renderer-probe-final-software-gate
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                        --expect-code 3
                        --expect-regex "final_software_golden_eligible=false"
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal
                        --require-final-software-adapter)
                set_tests_properties(scene3d-renderer-probe-final-software-gate
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "final_software_golden_eligible=false")

                add_test(NAME scene3d-renderer-probe-cli-surface-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface.py"
                        --probe-tool
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --fixture
                        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
                set_tests_properties(scene3d-renderer-probe-cli-surface-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_cli_surface_case=help.*renderer_probe_cli_surface_case=invalid-scene.*renderer_probe_cli_surface_case=invalid-width.*renderer_probe_cli_surface_case=boxtextured-requires-fixture.*renderer_probe_cli_surface_case=null-backend.*renderer_probe_cli_surface_case=final-software-gate.*renderer_probe_cli_surface_case=hardcoded-output-png.*renderer_probe_cli_surface_case=boxtextured-handoff.*renderer_probe_cli_surface_verified=true")

                add_test(NAME scene3d-renderer-probe-cli-surface-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface_contract.py"
                        --cli-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface.py")
                set_tests_properties(scene3d-renderer-probe-cli-surface-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_cli_surface_contract_case=valid-fake-probe.*renderer_probe_cli_surface_contract_case=help-option-drift.*renderer_probe_cli_surface_contract_case=invalid-scene-exit-drift.*renderer_probe_cli_surface_contract_case=invalid-width-usage-drift.*renderer_probe_cli_surface_contract_case=boxtextured-fixture-drift.*renderer_probe_cli_surface_contract_case=null-backend-pixel-drift.*renderer_probe_cli_surface_contract_case=final-gate-exit-drift.*renderer_probe_cli_surface_contract_case=final-gate-eligibility-drift.*renderer_probe_cli_surface_contract_case=boxtextured-consumption-drift.*renderer_probe_cli_surface_contract_case=output-png-missing-file-drift.*renderer_probe_cli_surface_contract_case=output-png-magic-drift.*renderer_probe_cli_surface_contract_verified=true")

                add_test(NAME scene3d-native-renderer-link-boundary
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary.py"
                        --build-dir "${CMAKE_BINARY_DIR}")
                set_tests_properties(scene3d-native-renderer-link-boundary
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "native_renderer_boundary_verified=7 link files")

                add_test(NAME scene3d-native-renderer-link-boundary-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary_contract.py"
                        --boundary-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary.py"
                        --build-dir "${CMAKE_BINARY_DIR}")
                set_tests_properties(scene3d-native-renderer-link-boundary-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "native_renderer_boundary_contract_case=valid-current-link-boundary.*native_renderer_boundary_contract_case=forbidden-view-link-token.*native_renderer_boundary_contract_case=forbidden-widget-link-token.*native_renderer_boundary_contract_case=missing-scene-parser-token.*native_renderer_boundary_contract_case=missing-render-webgpu-token.*native_renderer_boundary_contract_case=missing-required-link-file.*native_renderer_boundary_contract_verified=true")
            endif()
        endif()
    endif()

    # GPU compute staging-buffer pool tests. Reaches into
    # core/render/src/ for the internal pool header — the pool
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

# Meter/scalar host->view source + subscription lifecycle
pulp_add_test_suite(pulp-test-meter-source LIBRARIES pulp::view)

# Motion bedrock tests.
pulp_add_test_suite(pulp-test-motion LIBRARIES pulp::view)

# MotionPreferences reduced-motion policy + animation honoring.
add_executable(pulp-test-motion-preferences test_motion_preferences.cpp)
target_link_libraries(pulp-test-motion-preferences PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-preferences)

# Motion inspector protocol-domain tests.
if(TARGET pulp::inspect)
    add_executable(pulp-test-motion-inspector test_motion_inspector.cpp)
    target_link_libraries(pulp-test-motion-inspector PRIVATE
        pulp::view pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-motion-inspector)

    # Motion scrubber consumes .motion.jsonl fixtures and re-emits events
    # up to a frame playhead.
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

# Motion provenance adapters. One test per
# animation surface (Tween + PULP_MOTION_TWEEN, AnimatorSetBuilder::name,
# CSS TransitionSpec, ambient slot for rAF + design-import). Verifies the
# Provenance envelope round-trips end to end through the publish channel.
add_executable(pulp-test-motion-provenance test_motion_provenance.cpp)
target_link_libraries(pulp-test-motion-provenance PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-provenance)

# Motion input record + replay end-to-end: records a hover -> click ->
# drag against a synthetic view tree, replays the
# same fixture against a fresh tree, asserts identical motion fixture
# emerges (modulo timing tolerance).
add_executable(pulp-test-motion-input-replay test_motion_input_replay.cpp)
target_link_libraries(pulp-test-motion-input-replay PRIVATE
    pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-motion-input-replay)

# Motion cost attribution — correlates per-frame render cost +
# dirty-rect area with the trace_ids that emitted on the same frame
# (carrying their provenance envelopes). The bridge probe
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

# Motion visual-analysis self-check.
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

# Remote View Protocol loopback tests.
pulp_add_test_suite(pulp-test-remote-view LIBRARIES pulp::format pulp::runtime)

# Script engine tests (legacy API)
pulp_add_test_suite(pulp-test-script SOURCES test_script_engine.cpp LIBRARIES pulp::view)

# Scripted UI hot reload/theme reload tests
add_executable(pulp-test-scripted-ui test_scripted_ui.cpp)
target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::view Catch2::Catch2WithMain)
if(TARGET pulp-render)
    target_link_libraries(pulp-test-scripted-ui PRIVATE pulp::render)
endif()
# `slow`: ScriptedUiSession reload tests sleep on file-watcher
# debounce + filesystem mtime; ~0.5-1 sec each. Excluded from PR fast-CI.
catch_discover_tests(pulp-test-scripted-ui PROPERTIES LABELS slow)

# JS engine abstraction tests (shared across all backends)
pulp_add_test_suite(pulp-test-js-engine LIBRARIES pulp::view)

# Theme/design token tests
pulp_add_test_suite(pulp-test-theme LIBRARIES pulp::view)

# Three.js IIFE resources loader contract test. Runs on all platforms;
# iOS-only behavior is gated inside the test.
add_executable(pulp-test-threejs-resources test_threejs_resources.cpp)
target_link_libraries(pulp-test-threejs-resources PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-threejs-resources)

# Node-based smoke test for the IIFE bundler. Only registered when
# Node.js is on PATH — desktop CI hosts have it,
# fresh / offline builds may not (parallels the optional bundle step
# in PulpAuv3.cmake).
find_program(_PULP_NODE_FOR_TESTS NAMES node nodejs)
if(_PULP_NODE_FOR_TESTS)
    add_test(NAME pulp_bundle_threejs_for_jsc_smoke
             COMMAND ${_PULP_NODE_FOR_TESTS}
                     ${CMAKE_SOURCE_DIR}/tools/scripts/test_bundle_threejs_for_jsc.mjs)
    set_tests_properties(pulp_bundle_threejs_for_jsc_smoke PROPERTIES
        TIMEOUT 30
        LABELS "ios-d3b;node;threejs")
endif()

# Screenshot tests. macOS uses the CoreGraphics bitmap context; non-Apple
# builds with Skia use the built-in cross-platform Skia raster backend
# for Windows/Linux parity, so the test runs there too and proves the
# headless render_to_png/rgba path the foreign-host embed depends on. A
# non-Apple build WITHOUT Skia has no backend, so skip there.
if(APPLE OR PULP_HAS_SKIA)
    add_executable(pulp-test-screenshot test_screenshot.cpp)
    target_link_libraries(pulp-test-screenshot PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot)
endif()

# #3329 — native PluginViewHost factory + headless-capture proof on non-Apple.
# Built where core/view compiled a platform host: Windows+Skia or non-Android
# UNIX+Skia+X11. Degrades to capture-only with no display server (headless VM);
# capture_back_buffer_png() still yields a frame. On Linux+X11 the same binary
# also runs the XDND synthetic-source handshake under xvfb: it links
# Xlib directly to act as an XDND source against the host's child window, gated
# by PULP_TEST_HAS_X11 so the handshake TEST_CASE is compiled only there.
if(PULP_HAS_SKIA AND NOT APPLE AND NOT ANDROID)
    set(_pulp_has_platform_pvh OFF)
    set(_pulp_pvh_x11 OFF)
    if(WIN32)
        set(_pulp_has_platform_pvh ON)
    elseif(UNIX)
        find_package(X11 QUIET)
        if(X11_FOUND)
            set(_pulp_has_platform_pvh ON)
            set(_pulp_pvh_x11 ON)
        endif()
    endif()
    if(_pulp_has_platform_pvh)
        add_executable(pulp-test-plugin-view-host-factory test_plugin_view_host_factory.cpp)
        target_link_libraries(pulp-test-plugin-view-host-factory PRIVATE pulp::view Catch2::Catch2WithMain)
        if(_pulp_pvh_x11)
            target_link_libraries(pulp-test-plugin-view-host-factory PRIVATE ${X11_LIBRARIES})
            target_include_directories(pulp-test-plugin-view-host-factory PRIVATE ${X11_INCLUDE_DIR})
            target_compile_definitions(pulp-test-plugin-view-host-factory PRIVATE PULP_TEST_HAS_X11=1)
        endif()
        catch_discover_tests(pulp-test-plugin-view-host-factory)
    endif()
endif()

if(APPLE)
    # P1 — smart capture (capture_view) + offscreen-GPU path (render_to_png_gpu).
    add_executable(pulp-test-screenshot-gpu test_screenshot_gpu.cpp)
    target_link_libraries(pulp-test-screenshot-gpu PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-gpu)

    add_executable(pulp-test-screenshot-cli-contracts test_screenshot_cli_contracts.cpp)
    target_link_libraries(pulp-test-screenshot-cli-contracts PRIVATE
        pulp::view
        pulp::state
        Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-screenshot-cli-contracts)
endif()

# Canvas tests
pulp_add_test_suite(pulp-test-canvas-color LIBRARIES pulp::canvas)

# Outset box-shadow coverage cache (Skia path). Test body is gated on
# PULP_HAS_SKIA, which pulp::canvas propagates; without Skia it has zero cases.
pulp_add_test_suite(pulp-test-box-shadow-cache LIBRARIES pulp::canvas)

add_executable(pulp-test-canvas test_canvas.cpp)
target_link_libraries(pulp-test-canvas PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    # CoreGraphicsCanvas tests build a CGBitmapContext directly to
    # verify CGContextClearRect actually clears destination pixels.
    # ImageIO provides CGImageDestinationCreateWithURL for PNG encoding
    # of a pattern fixture; CoreServices provides the public.png UTI.
    target_link_libraries(pulp-test-canvas PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices")
endif()
# Public font-registration API tests use a bundled .ttf as a
# fixture. external/fonts/Inter-Regular.ttf is the canonical "always
# available" font in the repo (also baked into pulp-canvas via
# pulp_add_binary_data). Pass the absolute path through a compile def so
# the test binary can find it regardless of CWD when run via ctest.
target_compile_definitions(pulp-test-canvas PRIVATE
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\"")
catch_discover_tests(pulp-test-canvas)

# Canvas CG-paths tests. Apple-only TU covering concat_transform,
# Canvas2D path API fill/curves/stroke/gradients, fill_rect /
# fill_path with active gradient, save/restore tracking, and
# BlendMode every-value round-trip. Companion to test_canvas_cg_gradients.cpp;
# the non-Apple parts stay in test_canvas.cpp.
add_executable(pulp-test-canvas-cg-paths test_canvas_cg_paths.cpp)
target_link_libraries(pulp-test-canvas-cg-paths PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(APPLE)
    target_link_libraries(pulp-test-canvas-cg-paths PRIVATE "-framework CoreGraphics" "-framework ImageIO" "-framework CoreServices")
endif()
catch_discover_tests(pulp-test-canvas-cg-paths)

# Canvas font tests. Covers bundled-font registration via
# match_bundled_typeface plus the public register_font(path) API.
# Shares the PULP_TEST_FONT_PATH compile def so test fixtures find
# the bundled Inter-Regular.ttf regardless of CWD.
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
    "PULP_TEST_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/Inter-Regular.ttf\""
    # Deterministic variable font (wght axis 300-800) for the variable-font
    # weight-instancing regression tests. Test-only; not in bundled_blobs().
    "PULP_TEST_VARIABLE_FONT_PATH=\"${CMAKE_SOURCE_DIR}/external/fonts/FunnelDisplay-VariableFont_wght.ttf\"")
catch_discover_tests(pulp-test-canvas-fonts)

# CG-degraded gradient + pattern cluster. Apple-only TU: every
# TEST_CASE drives CoreGraphics directly to prove the CoreGraphicsCanvas
# fallback honours the canvas2d spec — conic / two-circle
# radial / pattern + gradient-anchored fill/stroke text + line-dash CG
# paths.
add_executable(pulp-test-canvas-cg-gradients test_canvas_cg_gradients.cpp)
target_link_libraries(pulp-test-canvas-cg-gradients PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-cg-gradients PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-cg-gradients PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-cg-gradients)

# CSS filter chain coverage: Skia-only pixel readback (contrast /
# invert / opacity ordering with drop-shadow, set_filter drop-shadow
# parser) plus portable filter-chain matrix math (contrast bias,
# invert maps, identity, opacity alpha scaling).
add_executable(pulp-test-canvas-filter-chain test_canvas_filter_chain.cpp)
target_link_libraries(pulp-test-canvas-filter-chain PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-filter-chain PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-filter-chain PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-filter-chain)

# Canvas arcs / path-primitive Skia rasterization fixtures. Native
# arc / arcTo / ellipse / roundRect cluster: RecordingCanvas
# captures of native primitives + Skia rasterization fixtures (full /
# half circle arc matches reference SkPath::arcTo, arc_to collinear
# lineTos, round_rect 4-corner radii, ellipse rotation contour
# continuity).
add_executable(pulp-test-canvas-arcs test_canvas_arcs.cpp)
target_link_libraries(pulp-test-canvas-arcs PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-arcs PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-arcs PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-arcs)

# Canvas::draw_svg — faithful SVG rendering via SkSVGDOM (gradients/filters).
add_executable(pulp-test-canvas-svg test_canvas_svg.cpp)
target_link_libraries(pulp-test-canvas-svg PRIVATE pulp::canvas Catch2::Catch2WithMain)
if(PULP_HAS_SKIA)
    target_compile_definitions(pulp-test-canvas-svg PRIVATE PULP_HAS_SKIA=1)
    target_include_directories(pulp-test-canvas-svg PRIVATE ${SKIA_INCLUDE_DIRS})
endif()
catch_discover_tests(pulp-test-canvas-svg)

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

# SDF glyph atlas exploration
pulp_add_test_suite(pulp-test-sdf-atlas LIBRARIES pulp::canvas)

# MSDF glyph atlas
pulp_add_test_suite(pulp-test-msdf-atlas LIBRARIES pulp::canvas)

# PSDF glyph atlas + vector fallback
pulp_add_test_suite(pulp-test-psdf-atlas LIBRARIES pulp::canvas)

# Shared SDF/MSDF text helpers (pen snap + build_text_quads + fill_text_*)
pulp_add_test_suite(pulp-test-sdf-text LIBRARIES pulp::canvas)

# SDF effects layer — host-side API + presets
pulp_add_test_suite(pulp-test-sdf-effects LIBRARIES pulp::canvas)

# Runtime SDF atlas cache — LRU + frame-based eviction
pulp_add_test_suite(pulp-test-sdf-atlas-cache LIBRARIES pulp::canvas)

# Software SDF renderer (end-to-end headless raster)
pulp_add_test_suite(pulp-test-sdf-software-renderer LIBRARIES pulp::canvas)

# Runtime path → SDF conversion — on-device SDF generation
pulp_add_test_suite(pulp-test-path-to-sdf LIBRARIES pulp::canvas)

# View and layout tests
pulp_add_test_suite(pulp-test-view LIBRARIES pulp::view)

# View corner-radius tests extracted from test_view.cpp.
# Covers Panel + View percent-radius math, per-corner radius
# resolution, reflow-on-bounds-change, view border stroke + outline +
# box-shadow honoring effective_corner_radius.
pulp_add_test_suite(pulp-test-view-corner-radius LIBRARIES pulp::view)

# View z-index + overflow tests extracted from test_view.cpp. Covers
# z-index paint-order + hit-test, overflow:visible default for
# absolute-positioned popovers, and symmetric overflow:visible hit-test
# extension.
pulp_add_test_suite(pulp-test-view-zindex-overflow LIBRARIES pulp::view)

# View CSS mask + overflow:visible hit-test extension. Covers the
# symmetric overflow:visible hit-test extension
# (500px in each direction so absolutely-positioned popovers protrude
# past their parent) plus CSS mask-image + mask-size
# paint routing (save_layer_with_mask routing).
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
# coverage, so that lives in a separate partial-rendering test.
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

# Dawn GPU timestamp queries. The pure tick->ms resolution
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

    # WindowHost::mark_dirty() VBlank-locked safe-repaint routing.
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

# HostQuirks scaffolding + HostVersion detection
pulp_add_test_suite(pulp-test-host-version LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-host-quirks LIBRARIES pulp::format)

# Host-quirks catalog parity (host-quirks integration plan, P1) — the
# machine-readable core/format/host-quirks.json must list EXACTLY the
# flags + tiers in the C++ HostQuirksMeta. Pure-Python parser, no C++
# build needed, so it runs cheaply on every PR and guards against the
# JSON and the struct drifting apart.
if(Python3_Interpreter_FOUND)
    add_test(NAME host-quirks-catalog-parity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/scripts/test_host_quirks_catalog_parity.py)
    set_tests_properties(host-quirks-catalog-parity PROPERTIES
        LABELS "format;host-quirks;catalog"
        TIMEOUT 30)
endif()

# DryWetMixer + Panner pan laws
pulp_add_test_suite(pulp-test-pan-mix-laws LIBRARIES pulp::signal)

# UMP per-note + JR Clock
pulp_add_test_suite(pulp-test-ump-extensions LIBRARIES pulp::midi)

# HMAC + AEAD primitives
pulp_add_test_suite(pulp-test-hmac-aead LIBRARIES pulp::runtime)

# Compressor sidechain HPF + lookahead
pulp_add_test_suite(pulp-test-compressor-sidechain LIBRARIES pulp::signal)

# ARA scaffold validation
pulp_add_test_suite(pulp-test-ara-scaffold LIBRARIES pulp::format)

# MIDI 1.0 backend audit
pulp_add_test_suite(pulp-test-midi1-backend-audit LIBRARIES pulp::midi)

# BufferOps SIMD helpers
pulp_add_test_suite(pulp-test-buffer-ops LIBRARIES pulp::audio pulp::runtime)

# MpeVoiceTracker per-note management + assignable PNC consumption
pulp_add_test_suite(pulp-test-mpe-tracker-per-note-management LIBRARIES pulp::midi)

# Wavetable oscillator
pulp_add_test_suite(pulp-test-wavetable LIBRARIES pulp::signal)

# Resampler
pulp_add_test_suite(pulp-test-resampler LIBRARIES pulp::signal)

# Polyphase IIR half-band filter
pulp_add_test_suite(pulp-test-halfband-iir LIBRARIES pulp::signal)

# Generic Synthesiser polyphony
pulp_add_test_suite(pulp-test-synthesiser
    SOURCES test_synthesiser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# SampleConverter
pulp_add_test_suite(pulp-test-sample-converter LIBRARIES pulp::audio)

# AudioSource hierarchy
pulp_add_test_suite(pulp-test-audio-source LIBRARIES pulp::audio)

# Foundation DSP/runtime/state tests
pulp_add_test_suite(pulp-test-dc-blocker LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-denormal SOURCES test_denormal.cpp test_scoped_flush_denormals.cpp LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-normalisable-range LIBRARIES pulp::state)
pulp_add_test_suite(pulp-test-adsr LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-high-resolution-timer LIBRARIES pulp::runtime)

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
pulp_add_test_suite(pulp-test-platform-maturity LIBRARIES pulp::view)

# Audio bridge tests
pulp_add_test_suite(pulp-test-audio-bridge LIBRARIES pulp::view)
# Widget tests
pulp_add_test_suite(pulp-test-widgets LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-parameter-edit LIBRARIES pulp::view pulp::state)
pulp_add_test_suite(pulp-test-param-host-sync LIBRARIES pulp::state INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/core/format/include)
pulp_add_test_suite(pulp-test-midi-binding LIBRARIES pulp::view pulp::state pulp::midi)
# Widget tests — Label cluster extracted from test_widgets.cpp. Label
# intrinsic_width / intrinsic_height /
# line-height multiplier / line_clamp / measured_height under
# bounded width / baseline_y from text metrics / vertical text
# direction / letter_spacing glyph counting.
pulp_add_test_suite(pulp-test-widgets-label LIBRARIES pulp::view)
# Hot-reload tests
add_executable(pulp-test-hot-reload test_hot_reload.cpp)
target_link_libraries(pulp-test-hot-reload PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow`: each HotReloader scenario sleeps on file-watcher
# debounce + filesystem mtime resolution (~1-1.5 sec each).
catch_discover_tests(pulp-test-hot-reload
    PROPERTIES
        RESOURCE_LOCK hot-reload-file-watcher
        LABELS slow)

# Non-GPU inspector helpers. The full inspector-domain suite is GPU-gated
# because it exercises View/Render integration, but these domain helpers are
# plain data/StateStore contracts and should stay covered in CPU-only builds.
add_executable(pulp-test-inspector-domain-helpers
    test_inspector_domain_helpers.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/audio_inspector.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/console_capture.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/editor_url.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/state_inspector.cpp)
target_include_directories(pulp-test-inspector-domain-helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/inspect/include)
target_link_libraries(pulp-test-inspector-domain-helpers PRIVATE
    pulp::audio pulp::canvas pulp::state pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-domain-helpers)

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

    # Inspector sibling TUs: GPU pass attribution, source-jump,
    # drift/reconcile, and atlas viewer.
    # Each mirrors pulp-test-inspector's exact registration (own executable
    # + catch_discover_tests, same libraries, same NO_LAUNCH env guard).
    add_executable(pulp-test-inspector-gpu-passes test_inspector_gpu_passes.cpp)
    target_link_libraries(pulp-test-inspector-gpu-passes PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-gpu-passes
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Wiring tab — lists design-sourced (Figma) overlays + wired/unwired badge.
    add_executable(pulp-test-inspector-wiring test_inspector_wiring.cpp)
    target_link_libraries(pulp-test-inspector-wiring PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-wiring
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
    if(WIN32)
        catch_discover_tests(pulp-test-inspector-server
            PROPERTIES
                ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1"
                LABELS "windows-pr-quarantine")
    else()
        catch_discover_tests(pulp-test-inspector-server
            PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
    endif()

    # Additional inspector sibling TUs. Same registration as
    # pulp-test-inspector.
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

    # TweakStore + Inspector.applyTweak protocol surface.
    # planning/2026-05-18-inspector-direct-manipulation-roadmap.md
    add_executable(pulp-test-tweak-store test_tweak_store.cpp)
    target_link_libraries(pulp-test-tweak-store PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-tweak-store)

    # Editor URI plumbing for the source-jump action.
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
set(_pulp_widget_bridge_test_libs pulp::view)
if(TARGET pulp-render)
    list(APPEND _pulp_widget_bridge_test_libs pulp::render)
endif()
pulp_add_test_suite(pulp-test-widget-bridge LIBRARIES ${_pulp_widget_bridge_test_libs})

# Widget bridge — source-level API contract. Keeps JS-native registrations
# unique and matched to the reviewed bridge API manifest so future registrar
# splits cannot add unclassified bridge entries.
pulp_add_test_suite(pulp-test-widget-bridge-api-contracts
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# Widget bridge — no-GPU gate enforcement.
# Pure static scan: walks widget_bridge.cpp line-by-line and asserts every
# `gpu_surface_->` dereference is inside a PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
# gate (or the #else of an #ifndef PULP_HAS_SKIA block, which implies the
# render module's include path is present). Catches the iOS Simulator
# regression class where GpuSurface is forward-declared but its members are
# called from configures that did not link the render module. Runs in the
# default macOS PR lane in milliseconds — no Xcode / iOS SDK required, so it
# closes the gap the slow `cmake-ios-auv3-configure` test left behind for PR
# validation.
pulp_add_test_suite(pulp-test-widget-bridge-no-gpu-gates
    COMPILE_DEFINITIONS PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Widget bridge — runtime-import handlers.
pulp_add_test_suite(pulp-test-widget-bridge-runtime-import LIBRARIES pulp::view)

# Widget bridge — Canvas2D surface. Covers canvasSetTransform /
# canvasClip / canvasGlobalCompositeOperation, canvasMeasureText /
# canvasSetLineDash / canvasDrawImage, canvasGetImageData /
# canvasPutImageData, and 4-arg canvasFillText prior-state preservation.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d LIBRARIES pulp::view)

# Widget bridge — Yoga layer. Three coherent clusters: (1) dimension
# percent strings (width/height + min/max via Yoga's percent API),
# (2) flexBasis% promotion (basis "NN%" + "flex 1 1 NN%"
# decomposition), and (3) yoga value-aliasing
# (flexDirection / justifyContent / alignItems / alignSelf / order /
# flexWrap value translations).
pulp_add_test_suite(pulp-test-widget-bridge-yoga LIBRARIES pulp::view)

# Widget bridge — recovered Canvas2D/CSS compatibility regressions.
# The canonical Wave 2 Canvas2D cheap-wiring surface is in
# pulp-test-widget-bridge-wave2-cheap below; this older split keeps
# recovered CSS cases and later Canvas2D bridge regressions.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-wave2 LIBRARIES pulp::view)

# Widget bridge — yoga logical-edge fan-out + A4 OOS pins. Covers yoga
# logical-edge marginInline/Block + paddingInline/Block + inset
# shorthand plus CSS NOT-IMPL closure catalog hygiene.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-a4-oos LIBRARIES pulp::view)

# Widget bridge — compatibility cheap-wiring. Bundles the Canvas2D
# wiring (DIVERGE → PASS) + CSS value-coverage entries from compat.json.
pulp_add_test_suite(pulp-test-widget-bridge-wave2-cheap LIBRARIES pulp::view)

# Widget bridge — RN style-prop bridge primitives.
# setShadow / setOpacity / setTransform RN-shaped style
# functions flow through bridge into View's slots. Includes RN's
# shadowOpacity-into-color-alpha composition + transform-prop
# aggregation.
pulp_add_test_suite(pulp-test-widget-bridge-rn-style LIBRARIES pulp::view)

# Widget bridge — Tier-4 OOS / perf-hints / interaction misc pins.
# Pins the no-op / fallback contract for properties
# Pulp deliberately doesn't paint: 3D transforms, generated content,
# scroll-snap, will-change, contain, touch-action
# secondary keywords. Catalog hygiene tests.
pulp_add_test_suite(pulp-test-widget-bridge-tier4-oos LIBRARIES pulp::view)

# Widget bridge — RN outline cluster.
# outlineColor / outlineOffset / outlineStyle / outlineWidth longhands
# + `outline` shorthand decomposition through the RN style shim.
pulp_add_test_suite(pulp-test-widget-bridge-rn-outline LIBRARIES pulp::view)

# Widget bridge — clip-path + mask cluster.
# clip-path: inset/circle/polygon/url + shape coordinate parsing;
# mask-image: url + linear-gradient + transforms; mask-size /
# -position / -repeat / -origin / -clip / -composite; mask shorthand.
pulp_add_test_suite(pulp-test-widget-bridge-clip-mask LIBRARIES pulp::view)

# Widget bridge — SVG widgets. Three clusters: SvgPathWidget JS bridge
# integration, SvgRectWidget + SvgLineWidget JS bridge integration,
# and compound-path
# parser regression (Spectr PEAK / AVG / BOTH / OFF analyzer icons).
pulp_add_test_suite(pulp-test-widget-bridge-svg LIBRARIES pulp::view)

# Widget bridge — CSS compatibility audit. Runtime-path coverage for
# 49 entries flipped from partial/DIVERGE to supported:
# backgroundPosition / backgroundSize / textShadow /
# border / border-side / borderRadius / per-corner radius / boxShadow
# / opacity / outline / textOverflow / transformOrigin / zIndex /
# backdropFilter / display / overflow / overflow per-axis / and
# many more. 45 TEST_CASEs each exercising JS shim → bridge → View
# slot round-trip.
pulp_add_test_suite(pulp-test-widget-bridge-wave5-css LIBRARIES pulp::view)

# Widget bridge — HTML ARIA + querySelector. aria-label / role flow
# into View accessibility slots;
# document.querySelector accepts attribute selectors + combinators +
# :hover / :disabled / :checked / :enabled / :not / :first-child /
# :nth-child / :empty pseudo-classes.
pulp_add_test_suite(pulp-test-widget-bridge-html-aria LIBRARIES pulp::view)

# Widget bridge — CSS animations + transitions.
# animation-* longhands + shorthand decomposition; transition-*
# longhands + shorthand round-trip through the CSS shim and bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-animations LIBRARIES pulp::view)

# Widget bridge — CSS Grid extended surface.
# grid-template-columns / -rows / -areas, grid-column / -row / -area
# placement shorthand, gap longhands + shorthand, justify-* / align-* /
# place-* alignment, repeat() + minmax() + fr-unit + auto sizing tokens
# round-trip through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-grid LIBRARIES pulp::view)

# Widget bridge — animation API cluster. Bridge ↔ MotionEngine
# plumbing for setMotionToken, animate(), the Web
# Animations API surface (Element.animate / KeyframeEffect), motion
# provenance, and pulp-motion-bench harness output. Self-contained
# ~800-line cluster from test_widget_bridge.cpp.
pulp_add_test_suite(pulp-test-widget-bridge-animation LIBRARIES pulp::view)

# Widget bridge — per-edge margin / padding. marginTop /
# marginRight / marginBottom / marginLeft (and padding counterparts)
# each route to their own Yoga edge enum without cross-contamination
# through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-per-edge LIBRARIES pulp::view)

# Widget bridge — RN-OOS-fixup catalog audit tail. Material elevation
# shim, includeFontPadding round-trip, borderCurve
# squircle paint dispatch, isolation honest CSS-subset, and other
# RN-side OOS catalog hygiene checks.
pulp_add_test_suite(pulp-test-widget-bridge-rn-oos-fixup LIBRARIES pulp::view)

# Widget bridge — Canvas2D bridge-fn cluster. canvasSetFontFull,
# fillRule, and canvasSetDirection / canvasSetFilter are closely related
# bridge entry-points that thread JS-side Canvas2D semantics through
# WidgetBridge into the native canvas pipeline.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-bridge-fns LIBRARIES pulp::view)

# Widget bridge — Yoga borderWidth wiring.
# Pins YGNodeStyleSetBorder integration with Yoga 3.x default box-sizing
# (border-box); the companion content-box test lives in #1516 once
# setBoxSizing is wired.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-border LIBRARIES pulp::view)

# Widget bridge — CSS-misc cluster. Text-decoration longhands,
# line-clamp, and background-repeat are two coherent
# small CSS clusters that keep test_widget_bridge.cpp under the
# 3,000-line target.
pulp_add_test_suite(pulp-test-widget-bridge-css-misc LIBRARIES pulp::view)

# Autonomous Spectr regression suite. Composes the six
# [contract] invariants from
# test_widget_bridge.cpp into a Spectr-shaped mini-scenario so a future
# PR that keeps each unit-level invariant passing but breaks their
# *interaction* still fails first.
pulp_add_test_suite(pulp-test-spectr-regression LIBRARIES pulp::view)

# Typography inheritance for the CSS-style cascade
pulp_add_test_suite(pulp-test-typography-inheritance LIBRARIES pulp::view)

# Text-overflow ellipsis helper
pulp_add_test_suite(pulp-test-text-overflow LIBRARIES pulp::view)

# Editor bridge tests for the renderer-agnostic envelope/dispatcher
pulp_add_test_suite(pulp-test-editor-bridge LIBRARIES pulp::view)

# Input events tests
pulp_add_test_suite(pulp-test-input-events LIBRARIES pulp::view)

# Text editor tests
pulp_add_test_suite(pulp-test-text-editor LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-mouse LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)
pulp_add_test_suite(pulp-test-text-editor-paint LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-policy LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)

# TextEditor multi-line coverage (audit P3: wrap, click-to-caret in
# wrapped rows, caret_rect pixel positioning, single-vs-multi Enter
# contract). Keeps the original test_text_editor.cpp file unchanged
# so the existing single-line surface stays pinned in isolation.
pulp_add_test_suite(pulp-test-text-editor-multiline LIBRARIES pulp::view)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
pulp_add_test_suite(pulp-test-text-input LIBRARIES pulp::view)

# W3C Layout parity tests (flexbox, box model, visual properties)
pulp_add_test_suite(pulp-test-layout-w3c LIBRARIES pulp::view)

# W3C design-token runtime pair (parse/export) via the self-contained
# w3c_tokens.hpp — stays always-compiled when design-import is gated.
pulp_add_test_suite(pulp-test-w3c-tokens LIBRARIES pulp::view)

# LottieView playback logic (opt-in PULP_LOTTIE). Passes whether or not Lottie
# is compiled in: LottieView::supported() gates the playback assertions.
pulp_add_test_suite(pulp-test-lottie-view LIBRARIES pulp::view)

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

# CanvasWidget NaN/Infinity sanitization cluster.
# Pins that JS-supplied NaN / ±Inf coords land as 0 in the recorded
# draw command, while finite values pass through.
pulp_add_test_suite(pulp-test-canvas-widget-sanitize LIBRARIES pulp::view)

# CanvasWidget + SkiaCanvas Canvas2D shadow-state cluster. Replay +
# clear + transparent/zero short-circuit of sticky Canvas2D shadow*
# state.
pulp_add_test_suite(pulp-test-canvas-widget-shadow LIBRARIES pulp::view)

# Canvas2D JS-shim coverage. Drives the full
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
# toJSON honoring actual is2D state.
pulp_add_test_suite(pulp-test-canvas2d-dommatrix LIBRARIES pulp::view)

# Canvas2D shim coverage extracted from test_canvas2d_shim.cpp.
# Bundles #1525 (fillText / strokeText maxWidth + glyph cluster
# handling) + #1521 (arc-as-path DIVERGE → PASS) + #1520
# (ctx.direction / ctx.filter) + #1526 (catalog hygiene round-trip) +
# lineDashOffset re-flush + #1527 (getTransform / resetTransform + isPointInPath /
# isPointInStroke).
pulp_add_test_suite(pulp-test-canvas2d-shim-late LIBRARIES pulp::view)

# SvgPathWidget tests
pulp_add_test_suite(pulp-test-svg-path-widget LIBRARIES pulp::view)

# SvgRect + SvgLine widget tests
pulp_add_test_suite(pulp-test-svg-rect-widget LIBRARIES pulp::view)

# ScrollView tests
pulp_add_test_suite(pulp-test-scroll-view LIBRARIES pulp::view)

# Standalone ScrollBar widget tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-scroll-bar LIBRARIES pulp::view)

# SidePanel slide-in animation tests (macos plugin-authoring item 6.3)
pulp_add_test_suite(pulp-test-side-panel LIBRARIES pulp::view)

# ComboBox dropdown interaction tests
pulp_add_test_suite(pulp-test-combo-dropdown LIBRARIES pulp::view)

# Generalized overlay-click routing (View::active_overlay_)
pulp_add_test_suite(pulp-test-overlay-routing LIBRARIES pulp::view)

# Auto-clearing input-focus slot (View::focused_input_)
pulp_add_test_suite(pulp-test-focused-input LIBRARIES pulp::view)

# Auto-claim active_overlay_ from CSS shape
# (`position:absolute` + `z-index >= 10`) or `data-overlay` author hint
# detected by the web-compat layer (web-compat-style-decl.js +
# web-compat-element.js). Uses the real WidgetBridge so the heuristic
# runs against the same prelude stack the runtime ships.
pulp_add_test_suite(pulp-test-web-compat-overlay LIBRARIES pulp::view)

# Panel widget tests (styled containers)
pulp_add_test_suite(pulp-test-panel LIBRARIES pulp::view)

# ComponentDragger — drag-to-move helper for any View.
pulp_add_test_suite(pulp-test-component-dragger LIBRARIES pulp::view)

# TooltipWindow — transient floating tooltip near cursor.
pulp_add_test_suite(pulp-test-tooltip-window LIBRARIES pulp::view)

# BubbleMessageComponent — floating message bubble anchored to a source
# view with auto-dismiss.
pulp_add_test_suite(pulp-test-bubble-message LIBRARIES pulp::view)

# PreferencesPanel — sidebar-of-pages container.
pulp_add_test_suite(pulp-test-preferences-panel LIBRARIES pulp::view)

# PropertyPanel — typed property editor sister widget. Header-only, links state
# for PropertiesFile persistence.
pulp_add_test_suite(pulp-test-property-panel
    SOURCES test_property_panel.cpp
    LIBRARIES pulp::view pulp::state)

# UI components tests (ComboBox, TabPanel, ListBox, ScrollView, Tooltip, ProgressBar, CallOutBox)
pulp_add_test_suite(pulp-test-ui-components LIBRARIES pulp::view)

# GraphEditorView tests
pulp_add_test_suite(pulp-test-graph-editor-view LIBRARIES pulp::view)

# Modal overlay + ContextMenu (view-drawn popup menu) tests
pulp_add_test_suite(pulp-test-modal LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-context-menu LIBRARIES pulp::view)
# Design export tests
add_executable(pulp-test-design-export test_design_export.cpp)
target_link_libraries(pulp-test-design-export PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-export
    PROPERTIES LABELS "parser-import")

# Design import tests (Figma/Stitch/v0/Pencil -> Pulp, W3C tokens)
pulp_add_test_suite(pulp-test-design-import
    SOURCES test_design_import_ir.cpp test_design_import_codegen.cpp test_design_import_sources.cpp test_design_import_fidelity.cpp test_recognition_resolver.cpp
    LIBRARIES pulp::view
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    TEST_SPEC "~[network]"
    LABELS "parser-import")

# Reference-free import-fidelity self-checks (core/view/src/design_fidelity.cpp).
add_executable(pulp-test-design-fidelity test_design_fidelity.cpp)
target_link_libraries(pulp-test-design-fidelity PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-fidelity PROPERTIES LABELS "parser-import")

# Drift guard for the compat.json imports/object-coverage matrix. Reads the
# matrix (PULP_SRC_DIR/compat.json) and validates each `types` row's codegen
# claim against the real generate_pulp_js lowering.
add_executable(pulp-test-import-object-coverage test_import_object_coverage.cpp)
target_link_libraries(pulp-test-import-object-coverage PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-import-object-coverage PRIVATE
    PULP_SRC_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-import-object-coverage PROPERTIES LABELS "parser-import")
catch_discover_tests(pulp-test-design-import
    TEST_SPEC "[network]"
    PROPERTIES
        LABELS "parser-import"
        RESOURCE_LOCK "design-import-network")

add_executable(pulp-test-design-import-native-common test_design_import_native_common.cpp)
target_link_libraries(pulp-test-design-import-native-common PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-native-common
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-native-materializer
    test_design_import_native_materializer.cpp
    fixtures/design_import_generated_binding_runtime_fixture.cpp)
target_link_libraries(pulp-test-design-import-native-materializer
    PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-native-materializer PRIVATE
    PULP_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    PULP_TEST_OSX_SYSROOT="${CMAKE_OSX_SYSROOT}"
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-native-materializer
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-view-core-link test_view_core_link.cpp)
target_link_libraries(pulp-test-view-core-link PRIVATE pulp::view-core Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-core-link
    PROPERTIES LABELS "parser-import")

# RelativePoint expression layout (parser + evaluator + serializer).
# Lives under pulp::view-core so the test binary doesn't transitively
# need the JS engine. See planning/2026-05-24-reference-framework-
# gap-analysis.md "Relative coordinates" row.
add_executable(pulp-test-relative-point test_relative_point.cpp)
target_link_libraries(pulp-test-relative-point
    PRIVATE pulp::view-core Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-relative-point
    PROPERTIES LABELS "view")

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

# The generated-import C++ test targets below consume sources from the
# private `planning/` submodule (planning/artifacts/native-ui/nv0/reports/
# generated/...). The normal CI checkout does not initialize that
# submodule, so guard the whole block on the sentinel artifact's
# presence — when planning isn't checked out, the targets are skipped
# cleanly instead of failing CMake configure with a missing-source error.
if(EXISTS ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-f-hybrid.cpp)

set(CHAINER_PHASE_E_FADERS_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-faders.cpp)
set_source_files_properties(${CHAINER_PHASE_E_FADERS_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_fader_ui;bake_asset_manifest=bake_fader_asset_manifest;bind_imported_ui=bind_imported_fader_ui")

set(CHAINER_PHASE_E_XY_PAD_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-xy-pad.cpp)
set_source_files_properties(${CHAINER_PHASE_E_XY_PAD_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_xy_pad_ui;bake_asset_manifest=bake_xy_pad_asset_manifest;bind_imported_ui=bind_imported_xy_pad_ui")

set(CHAINER_PHASE_E_TOGGLE_BUTTONS_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-toggle-buttons.cpp)
set_source_files_properties(${CHAINER_PHASE_E_TOGGLE_BUTTONS_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_toggle_buttons_ui;bake_asset_manifest=bake_toggle_buttons_asset_manifest;bind_imported_ui=bind_imported_toggle_buttons_ui")

set(CHAINER_PHASE_E_WAVEFORM_CHOICES_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-waveform-choices.cpp)
set_source_files_properties(${CHAINER_PHASE_E_WAVEFORM_CHOICES_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_waveform_choices_ui;bake_asset_manifest=bake_waveform_choices_asset_manifest;bind_imported_ui=bind_imported_waveform_choices_ui")

set(CHAINER_PHASE_E_WAVEFORM_DISPLAY_CHOICES_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-waveform-display-choices.cpp)
set_source_files_properties(${CHAINER_PHASE_E_WAVEFORM_DISPLAY_CHOICES_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_waveform_display_choices_ui;bake_asset_manifest=bake_waveform_display_choices_asset_manifest;bind_imported_ui=bind_imported_waveform_display_choices_ui")

set(CHAINER_PHASE_E_METER_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-meter.cpp)
set_source_files_properties(${CHAINER_PHASE_E_METER_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_meter_ui;bake_asset_manifest=bake_meter_asset_manifest;bind_imported_ui=bind_imported_meter_ui")

set(CHAINER_PHASE_E_CHAIN_SELECTION_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-e-chain-selection.cpp)
set_source_files_properties(${CHAINER_PHASE_E_CHAIN_SELECTION_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_imported_chain_selection_ui;bake_asset_manifest=bake_chain_selection_asset_manifest;bind_imported_ui=bind_imported_chain_selection_ui")

set(CHAINER_PHASE_F_HYBRID_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-f-hybrid.cpp)

set(PHASE_H_COMPRESSOR_STRIP_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/compressor-strip-af557c0a.cpp)
set_source_files_properties(${PHASE_H_COMPRESSOR_STRIP_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_compressor_strip_ui;bake_asset_manifest=bake_phase_h_compressor_strip_asset_manifest")
set(PHASE_H_ENVELOPE_SHAPER_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/envelope-shaper-dac8070a.cpp)
set_source_files_properties(${PHASE_H_ENVELOPE_SHAPER_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_envelope_shaper_ui;bake_asset_manifest=bake_phase_h_envelope_shaper_asset_manifest")
set(PHASE_H_EQ_CURVE_PANEL_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/eq-curve-panel-328aec03.cpp)
set_source_files_properties(${PHASE_H_EQ_CURVE_PANEL_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_eq_curve_panel_ui;bake_asset_manifest=bake_phase_h_eq_curve_panel_asset_manifest")
set(PHASE_H_FILTER_MATRIX_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/filter-matrix-212a6450.cpp)
set_source_files_properties(${PHASE_H_FILTER_MATRIX_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_filter_matrix_ui;bake_asset_manifest=bake_phase_h_filter_matrix_asset_manifest")
set(PHASE_H_MIXER_SEND_BANK_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/mixer-send-bank-25199df9.cpp)
set_source_files_properties(${PHASE_H_MIXER_SEND_BANK_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_mixer_send_bank_ui;bake_asset_manifest=bake_phase_h_mixer_send_bank_asset_manifest")
set(PHASE_H_MODULATION_GRID_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/modulation-grid-ebdc2fd6.cpp)
set_source_files_properties(${PHASE_H_MODULATION_GRID_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_modulation_grid_ui;bake_asset_manifest=bake_phase_h_modulation_grid_asset_manifest")
set(PHASE_H_OSC_BANK_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/osc-bank-797fb21c.cpp)
set_source_files_properties(${PHASE_H_OSC_BANK_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_osc_bank_ui;bake_asset_manifest=bake_phase_h_osc_bank_asset_manifest")
set(PHASE_H_PRESET_BROWSER_STRIP_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/preset-browser-strip-da8feca8.cpp)
set_source_files_properties(${PHASE_H_PRESET_BROWSER_STRIP_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_preset_browser_strip_ui;bake_asset_manifest=bake_phase_h_preset_browser_strip_asset_manifest")
set(PHASE_H_SAMPLER_PAD_GRID_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/sampler-pad-grid-61913f62.cpp)
set_source_files_properties(${PHASE_H_SAMPLER_PAD_GRID_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_sampler_pad_grid_ui;bake_asset_manifest=bake_phase_h_sampler_pad_grid_asset_manifest")
set(PHASE_H_SCOPE_METER_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/scope-meter-3ec5c9f2.cpp)
set_source_files_properties(${PHASE_H_SCOPE_METER_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_scope_meter_ui;bake_asset_manifest=bake_phase_h_scope_meter_asset_manifest")
set(PHASE_H_TRANSPORT_LOOP_PANEL_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/transport-loop-panel-406e254b.cpp)
set_source_files_properties(${PHASE_H_TRANSPORT_LOOP_PANEL_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_transport_loop_panel_ui;bake_asset_manifest=bake_phase_h_transport_loop_panel_asset_manifest")
set(PHASE_H_UTILITY_SETTINGS_PANEL_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/utility-settings-panel-2f1bc7f9.cpp)
set_source_files_properties(${PHASE_H_UTILITY_SETTINGS_PANEL_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_utility_settings_panel_ui;bake_asset_manifest=bake_phase_h_utility_settings_panel_asset_manifest")
set(PHASE_H_LEVEL_METER_PANEL_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/level-meter-panel-facea92b.cpp)
set_source_files_properties(${PHASE_H_LEVEL_METER_PANEL_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_level_meter_panel_ui;bake_asset_manifest=bake_phase_h_level_meter_panel_asset_manifest")
set(PHASE_H_GAIN_STAGE_CARD_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/gain-stage-card-0931cecc.cpp)
set_source_files_properties(${PHASE_H_GAIN_STAGE_CARD_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_gain_stage_card_ui;bake_asset_manifest=bake_phase_h_gain_stage_card_asset_manifest")
set(PHASE_H_GAIN_STAGE_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/gain-stage-ac2bbdaa.cpp)
set_source_files_properties(${PHASE_H_GAIN_STAGE_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_gain_stage_ui;bake_asset_manifest=bake_phase_h_gain_stage_asset_manifest")
set(PHASE_H_TRANSPORT_BAR_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/transport-bar-81c21381.cpp)
set_source_files_properties(${PHASE_H_TRANSPORT_BAR_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_transport_bar_ui;bake_asset_manifest=bake_phase_h_transport_bar_asset_manifest")
set(PHASE_H_AUDIO_CONTROL_PANEL_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/audio-control-panel-adc7dbbd.cpp)
set_source_files_properties(${PHASE_H_AUDIO_CONTROL_PANEL_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_audio_control_panel_ui;bake_asset_manifest=bake_phase_h_audio_control_panel_asset_manifest")
set(PHASE_H_SETTINGS_STRIP_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/settings-strip-73630672.cpp)
set_source_files_properties(${PHASE_H_SETTINGS_STRIP_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_settings_strip_ui;bake_asset_manifest=bake_phase_h_settings_strip_asset_manifest")
set(PHASE_H_TRANSPORT_METER_CPP
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/import-cpp-probes/generated/transport-meter-0d11742c.cpp)
set_source_files_properties(${PHASE_H_TRANSPORT_METER_CPP} PROPERTIES
    COMPILE_DEFINITIONS "build_imported_ui=build_phase_h_transport_meter_ui;bake_asset_manifest=bake_phase_h_transport_meter_asset_manifest")

add_executable(pulp-test-design-import-cpp-codegen
    test_design_import_cpp_codegen.cpp
    fixtures/design_import_generated_cpp_fixture.cpp
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-d-all-knobs.cpp
    ${CHAINER_PHASE_E_FADERS_CPP}
    ${CHAINER_PHASE_E_XY_PAD_CPP}
    ${CHAINER_PHASE_E_TOGGLE_BUTTONS_CPP}
    ${CHAINER_PHASE_E_WAVEFORM_CHOICES_CPP}
    ${CHAINER_PHASE_E_WAVEFORM_DISPLAY_CHOICES_CPP}
    ${CHAINER_PHASE_E_METER_CPP}
    ${CHAINER_PHASE_E_CHAIN_SELECTION_CPP}
    ${CHAINER_PHASE_F_HYBRID_CPP}
    ${PHASE_H_COMPRESSOR_STRIP_CPP}
    ${PHASE_H_ENVELOPE_SHAPER_CPP}
    ${PHASE_H_EQ_CURVE_PANEL_CPP}
    ${PHASE_H_FILTER_MATRIX_CPP}
    ${PHASE_H_MIXER_SEND_BANK_CPP}
    ${PHASE_H_MODULATION_GRID_CPP}
    ${PHASE_H_OSC_BANK_CPP}
    ${PHASE_H_PRESET_BROWSER_STRIP_CPP}
    ${PHASE_H_SAMPLER_PAD_GRID_CPP}
    ${PHASE_H_SCOPE_METER_CPP}
    ${PHASE_H_TRANSPORT_LOOP_PANEL_CPP}
    ${PHASE_H_UTILITY_SETTINGS_PANEL_CPP}
    ${PHASE_H_LEVEL_METER_PANEL_CPP}
    ${PHASE_H_GAIN_STAGE_CARD_CPP}
    ${PHASE_H_GAIN_STAGE_CPP}
    ${PHASE_H_TRANSPORT_BAR_CPP}
    ${PHASE_H_AUDIO_CONTROL_PANEL_CPP}
    ${PHASE_H_SETTINGS_STRIP_CPP}
    ${PHASE_H_TRANSPORT_METER_CPP})
target_link_libraries(pulp-test-design-import-cpp-codegen
    PRIVATE pulp::view pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-cpp-codegen PRIVATE
    PULP_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(pulp-test-design-import-cpp-codegen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    catch_discover_tests(pulp-test-design-import-cpp-codegen
        PROPERTIES LABELS "parser-import;windows-pr-quarantine")
else()
    catch_discover_tests(pulp-test-design-import-cpp-codegen
        PROPERTIES LABELS "parser-import")
endif()

add_executable(pulp-test-design-import-cpp-core-link
    test_design_import_cpp_core_link.cpp
    ${CHAINER_PHASE_F_HYBRID_CPP})
target_link_libraries(pulp-test-design-import-cpp-core-link
    PRIVATE pulp::view-core Catch2::Catch2WithMain)
target_include_directories(pulp-test-design-import-cpp-core-link PRIVATE
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated)
catch_discover_tests(pulp-test-design-import-cpp-core-link
    PROPERTIES LABELS "parser-import")

add_executable(pulp-test-design-import-cpp-only
    test_design_import_cpp_only.cpp
    ${CHAINER_PHASE_F_HYBRID_CPP})
target_link_libraries(pulp-test-design-import-cpp-only
    PRIVATE pulp::view-core Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-cpp-only PRIVATE
    PULP_UI_RUNTIME_CPP_ONLY=1
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_include_directories(pulp-test-design-import-cpp-only PRIVATE
    ${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated)
catch_discover_tests(pulp-test-design-import-cpp-only
    PROPERTIES LABELS "parser-import")

else()
    message(STATUS
        "Pulp: planning/ submodule not initialized — skipping generated-import C++ test targets "
        "(pulp-test-design-import-cpp-codegen, pulp-test-design-import-cpp-core-link, "
        "pulp-test-design-import-cpp-only). Run `git submodule update --init planning` to enable.")
endif()

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

# Value-driven silhouette fill (design-import shape-fill — item 3): exercises
# ImageView::set_fill_value + the canvas url() image mask on the Skia raster
# backend (no GPU window).
add_executable(pulp-test-image-view-fill
    test_image_view_fill.cpp)
target_link_libraries(pulp-test-image-view-fill
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-image-view-fill
    PROPERTIES LABELS "view")

# GPU regression for the same silhouette fill on the LIVE Graphite path: the
# url() mask shader must upload its image to a GPU texture or Graphite drops the
# masked draw every frame (ELYSIUM "shapes don't fill" bug). macOS-only; the
# offscreen GPU backend soft-skips at runtime when no Dawn adapter is present.
if(APPLE)
    add_executable(pulp-test-image-view-fill-gpu
        test_image_view_fill_gpu.cpp)
    target_link_libraries(pulp-test-image-view-fill-gpu
        PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-image-view-fill-gpu
        PROPERTIES LABELS "view")
endif()

# DesignFrameView (Plan B / B1) — faithful SVG render + typed interactive knobs.
add_executable(pulp-test-design-frame-view
    test_design_frame_view.cpp)
target_link_libraries(pulp-test-design-frame-view
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-frame-view
    PROPERTIES LABELS "view")

# HostParamSurface / HostActionSurface — the SDK runtime host-param + action
# surfaces: StateStore backing, DesignFrameView
# sync/route/re-key, action forwarding, and cross-host binding via a fake.
add_executable(pulp-test-host-param-surface
    test_host_param_surface.cpp)
target_link_libraries(pulp-test-host-param-surface
    PRIVATE pulp::view pulp::state Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-host-param-surface
    PROPERTIES LABELS "view")

# Faithful port toolkit — SDK primitives
# (SVG fragment handles, anchored popover, drag-to-reorder, paint-space painters).
add_executable(pulp-test-faithful-port-toolkit
    test_faithful_port_toolkit.cpp)
target_include_directories(pulp-test-faithful-port-toolkit PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-faithful-port-toolkit
    PRIVATE pulp::view pulp-annotated-capture Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-faithful-port-toolkit
    PROPERTIES LABELS "view")

# MusicalTypingKeyboard — Ink & Signal catalog component (faithful Figma SVG
# via DesignFrameView). Pins SVG load, headless render, catalog registration.
add_executable(pulp-test-musical-typing-keyboard
    test_musical_typing_keyboard.cpp)
target_link_libraries(pulp-test-musical-typing-keyboard
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-musical-typing-keyboard
    PROPERTIES LABELS "view")

# ChannelStripView — faithful Figma-vector catalog component.
add_executable(pulp-test-channel-strip-view
    test_channel_strip_view.cpp)
target_link_libraries(pulp-test-channel-strip-view
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-channel-strip-view
    PROPERTIES LABELS "view")

# Faithful Figma-vector specimen catalog components (generated).
add_executable(pulp-test-faithful-specimens
    test_faithful_specimens.cpp)
target_link_libraries(pulp-test-faithful-specimens
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-faithful-specimens
    PROPERTIES LABELS "view")

# W3C Design Tokens cluster extracted from test_design_import.cpp.
# Covers parse_w3c_tokens, export_w3c_tokens,
# composite typography/shadow shapes, alias resolution, math
# expressions, group $type inheritance, ir_tokens_to_theme +
# theme_to_ir_tokens round-trips.
add_executable(pulp-test-design-import-w3c-tokens test_design_import_w3c_tokens.cpp)
target_link_libraries(pulp-test-design-import-w3c-tokens PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-w3c-tokens PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-w3c-tokens
    PROPERTIES LABELS "parser-import")

# Baked SwiftUI emitter. Golden-string asserts plus a swiftc type-check
# gate (find_program; the test skips when swiftc / the SwiftUI SDK is
# unavailable, e.g. the Linux lane). Registered here, outside the
# planning-artifact-guarded cpp block above, since it needs no
# generated artifacts.
find_program(PULP_SWIFTC swiftc)
add_executable(pulp-test-design-swift-codegen test_design_swift_codegen.cpp)
target_link_libraries(pulp-test-design-swift-codegen
    PRIVATE pulp::view-core pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-swift-codegen PRIVATE
    PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    PULP_TEST_SWIFTC="${PULP_SWIFTC}")
catch_discover_tests(pulp-test-design-swift-codegen
    PROPERTIES LABELS "parser-import")

# React-runtime parser cluster extracted from test_design_import.cpp.
# Covers TSX/runtime React parsers for every
# supported design-tool source: parse_v0_tsx / parse_v0_dev_react,
# parse_figma_make_react, parse_stitch_react, parse_react_native_export,
# parse_pencil_react. Shared contract: parse fixture, materialize via
# host React shim, accept sanitized TSX, reject out-of-matrix surfaces.
add_executable(pulp-test-design-import-react-runtime test_design_import_react_runtime.cpp)
target_link_libraries(pulp-test-design-import-react-runtime PRIVATE pulp::view Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-design-import-react-runtime PRIVATE PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")
catch_discover_tests(pulp-test-design-import-react-runtime
    PROPERTIES LABELS "parser-import")

# stable_anchor_id assignment per
# planning/2026-05-18-inspector-direct-manipulation-roadmap.md.
add_executable(pulp-test-design-import-anchors test_design_import_anchors.cpp)
target_link_libraries(pulp-test-design-import-anchors PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-anchors
    PROPERTIES LABELS "parser-import")

# Inspector lock-to-source, Path A (generated-TSX/JS rewrite).
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

# DESIGN.md import (Google design.md, Apache-2.0)
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

# Token lock-to-source via DESIGN.md rewrite
# (planning/2026-05-18-inspector-direct-manipulation-roadmap.md).
add_executable(pulp-test-token-lock test_token_lock.cpp)
target_link_libraries(pulp-test-token-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-token-lock
    PROPERTIES LABELS "parser-import")

# Inspector lock-to-source, Path B (hand-authored JSX/TSX patch).
# Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md.
# Proves the tweak -> JSX/TSX surgical patch -> formatting-preserving
# round-trip, plus the ambiguous / not-found / too-dynamic failure paths.
add_executable(pulp-test-jsx-lock test_jsx_lock.cpp)
target_link_libraries(pulp-test-jsx-lock PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-jsx-lock
    PROPERTIES LABELS "parser-import")

# Library-backed post-parse widget promotion: <div onClick> / role=button /
# cursor:pointer → button. The pass now lives in pulp::view so
# parser API users and the CLI share one normalization path.
add_executable(pulp-test-widget-promotion
    test_widget_promotion.cpp)
target_include_directories(pulp-test-widget-promotion PRIVATE
    ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-test-widget-promotion PRIVATE
    pulp::view
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-promotion
    PROPERTIES LABELS "parser-import")

# Web-compat preludes shipped for bundled-React imports
# (nodeType / nodeName, observer no-ops, scheduler shims). Uses
# WidgetBridge to evaluate the same prelude stack the runtime ships.
add_executable(pulp-test-web-compat-react-shims test_web_compat_react_shims.cpp)
target_link_libraries(pulp-test-web-compat-react-shims PRIVATE pulp::view Catch2::Catch2WithMain)
# Bound-pump test runs the 1M-job cap; budget headroom for slow
# CI runners and sanitizer builds. Default discover timeout is too tight
# when a regression of the bound would hang indefinitely. Other shim
# scenarios in this binary stay in PR fast-CI — they're cheap and
# important — so this binary is *not* labelled `slow` even though one
# of its tests pays a 1-3 sec wall cost.
catch_discover_tests(pulp-test-web-compat-react-shims
    PROPERTIES TIMEOUT 180
)

# Claude Design bundle envelope parser (base64+gzip JSON
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

# Design-tool platform key wire-up E2E. Pins the
# `WindowHost → root.on_global_key → bridge.forward_key_event →
# registerShortcut + __dispatch__('__global__','keydown',...)` chain that
# every auto-bound default chord (and Spectr's mode-switch) depends on.
add_executable(pulp-test-platform-key-wireup test_platform_key_wireup.cpp)
target_link_libraries(pulp-test-platform-key-wireup
    PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-platform-key-wireup
    PROPERTIES LABELS "parser-import")

# Pin WidgetBridge::dispatch_global_key fan-out
# across the static all_bridges_ registry. Every live bridge gets the
# key without any per-app on_global_key wiring; this is the framework
# contract platform hosts depend on.
add_executable(pulp-test-widget-bridge-dispatch-global-key
    test_widget_bridge_dispatch_global_key.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-global-key
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-global-key)

# Pin WidgetBridge::dispatch_document_event and the real
# document.addEventListener. Platform hosts fire synthetic
# outside-click events on Esc through this path so React popovers
# (Spectr's PickerDropdown, ContextMenu, etc.) close automatically.
add_executable(pulp-test-widget-bridge-dispatch-document-event
    test_widget_bridge_dispatch_document_event.cpp)
target_link_libraries(pulp-test-widget-bridge-dispatch-document-event
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-widget-bridge-dispatch-document-event)

# `pulp import-design --from claude` classnames.json
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
# The `import-design` command delegates to the standalone pulp-import-design
# helper; without it the revived shell-out cases fail (exit 1) instead of
# exercising the real path, so build it alongside this suite.
if(TARGET pulp-import-design)
    add_dependencies(pulp-test-cli-import-design pulp-import-design)
endif()
catch_discover_tests(pulp-test-cli-import-design
    PROPERTIES
        ENVIRONMENT "PULP_REPO_ROOT=${CMAKE_SOURCE_DIR}"
        LABELS "parser-import")

# Direct shell-out coverage for the standalone import-design tool
# executable. These tests hit tools/import-design/pulp_import_design.cpp
# without going through the top-level pulp CLI delegate.
add_executable(pulp-test-import-design-tool test_import_design_tool.cpp
    # Same miniz subset the CLI links — `.pulp.zip` auto-unpack
    # test needs to assemble a tiny ZIP fixture from C++ rather than
    # shelling out to `zip(1)` (which is not portable).
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tdef.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_zip.c)
target_include_directories(pulp-test-import-design-tool PRIVATE
    ${CMAKE_SOURCE_DIR}/external/miniz)
target_link_libraries(pulp-test-import-design-tool
    PRIVATE pulp::platform Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-import-design-tool PRIVATE
    PULP_IMPORT_DESIGN_TOOL_PATH="$<TARGET_FILE:pulp-import-design>")
add_dependencies(pulp-test-import-design-tool pulp-import-design)
if(WIN32)
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import;windows-pr-quarantine")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            LABELS "parser-import;windows-pr-quarantine"
            RESOURCE_LOCK "design-import-network")
else()
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "~[network]"
        PROPERTIES LABELS "parser-import")
    catch_discover_tests(pulp-test-import-design-tool
        TEST_SPEC "[network]"
        PROPERTIES
            LABELS "parser-import"
            RESOURCE_LOCK "design-import-network")
endif()

# `--execute-bundle` harness end-to-end. Boots the
# ScriptEngine + WidgetBridge, builds a synthetic React-like bundle on
# the fly, asserts the materialized DOM walker produces an IR deeper
# than the loader-shell baseline. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (Spectr's editor.html).
add_executable(pulp-test-design-import-claude-runtime test_design_import_claude_runtime.cpp)
target_link_libraries(pulp-test-design-import-claude-runtime
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-claude-runtime
    PROPERTIES LABELS "parser-import")

# Inline `<script>` evaluation in the `--execute-bundle`
# harness. Asserts that inline `text/javascript`, inline `text/babel`
# (via a Babel-standalone shim), DOMContentLoaded dispatch, and the
# layered async drain all fire. The optional [.fixture] case runs
# against PULP_CLAUDE_BUNDLE_FIXTURE if set (canonical Spectr
# editor.html — should now produce >20 nodes, not the 11-element
# loader shell previously seen before the full editor materialized).
add_executable(pulp-test-design-import-inline-babel test_design_import_inline_babel.cpp)
target_link_libraries(pulp-test-design-import-inline-babel
    PRIVATE pulp::view pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-import-inline-babel
    PROPERTIES LABELS "parser-import")

# JSX instrument import harness. Loads a
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
        PROPERTIES LABELS "parser-import" TIMEOUT 240)
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

# Plugin manager panel UI tests
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
pulp_add_test_suite(pulp-test-load-measurer
    SOURCES test_load_measurer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)

# FilterDesign tests
pulp_add_test_suite(pulp-test-filter-design LIBRARIES pulp::signal)

# IIR design tests (Chebyshev I/II, Elliptic)
pulp_add_test_suite(pulp-test-iir-design LIBRARIES pulp::signal)

# TreeView tests
pulp_add_test_suite(pulp-test-tree-view LIBRARIES pulp::view)

# Toolbar tests (UTF-8 icon-label extraction, custom-item width, hit-testing)
pulp_add_test_suite(pulp-test-toolbar LIBRARIES pulp::view)

# File browser, file tree, and multi-document panel tests
pulp_add_test_suite(pulp-test-file-browser LIBRARIES pulp::view)

# FileChooser widget builder wrapper over pulp::platform::FileDialog
# with async callback delivery.
pulp_add_test_suite(pulp-test-file-chooser LIBRARIES pulp::view pulp::platform)

# Undo manager tests
pulp_add_test_suite(pulp-test-undo-manager LIBRARIES pulp::state)

# BufferingReader tests
pulp_add_test_suite(pulp-test-buffering-reader LIBRARIES pulp::audio)

# AudioWorkgroup tests
pulp_add_test_suite(pulp-test-workgroup LIBRARIES pulp::audio)

# AudioWorkgroup ↔ AudioDevice wiring.
pulp_add_test_suite(pulp-test-audio-workgroup-wiring
    SOURCES test_audio_workgroup_wiring.cpp
    LIBRARIES pulp::audio)

# CoreAudio follow-default output-device live switch (macOS only).
if(APPLE AND NOT PULP_IOS)
    pulp_add_test_suite(pulp-test-coreaudio-default-follow
        SOURCES test_coreaudio_default_follow.mm
        LIBRARIES pulp::audio)
    target_link_libraries(pulp-test-coreaudio-default-follow PRIVATE "-framework CoreAudio")
endif()

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
# Harness support lib: Processor-driven helpers; file-analysis lives in pulp::audio-analysis (tools/audio/analysis). See test/support/README.md.
add_library(pulp-audio-test-support STATIC support/audio_signal_generators.cpp support/render_scenario.cpp support/audio_contracts.cpp support/audio_doctor.cpp)
target_link_libraries(pulp-audio-test-support PUBLIC pulp::format pulp::signal pulp::audio-analysis)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-golden)
add_executable(pulp-test-audio-tone-regression test_audio_tone_regression.cpp)
target_link_libraries(pulp-test-audio-tone-regression PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-tone-regression PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-audio-tone-regression)
add_executable(pulp-test-render-scenario test_render_scenario.cpp)
target_link_libraries(pulp-test-render-scenario PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-render-scenario PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-render-scenario)
add_executable(pulp-test-audio-support test_audio_support.cpp)
target_link_libraries(pulp-test-audio-support PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-support PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-audio-support)
add_executable(pulp-test-audio-contracts test_audio_contracts.cpp test_audio_contracts_effect.cpp)
target_link_libraries(pulp-test-audio-contracts PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-contracts PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-audio-contracts)
add_executable(pulp-test-audio-doctor test_audio_doctor.cpp)
target_link_libraries(pulp-test-audio-doctor PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-doctor PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-audio-doctor)
add_executable(pulp-test-cli-audio-validate test_cli_audio_validate.cpp)
target_link_libraries(pulp-test-cli-audio-validate PRIVATE pulp::audio pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-audio-validate)
catch_discover_tests(pulp-test-cli-audio-validate)

add_executable(pulp-test-cli-audio-compare test_cli_audio_compare.cpp)
target_link_libraries(pulp-test-cli-audio-compare PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-audio-compare)
catch_discover_tests(pulp-test-cli-audio-compare)
# Audio-probe RT output-boundary allocation counters.
add_executable(pulp-test-audio-probe test_audio_probe.cpp harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-audio-probe PRIVATE pulp::audio pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-probe)
# Audio determinism matrix: SR × block sweep, framework invariants (pass-through, silence, block invariance, re-prepare).
add_executable(pulp-test-audio-matrix test_audio_determinism_matrix.cpp)
target_link_libraries(pulp-test-audio-matrix PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-matrix PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-audio-matrix)
# Audio edge-case corpus: zero blocks, denormals, impulse pos, DC extremes, mid-block param change, re-prepare. Framework contract only.
add_executable(pulp-test-audio-edge-cases test_audio_edge_cases.cpp)
target_link_libraries(pulp-test-audio-edge-cases PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-edge-cases PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-audio-edge-cases)
# Golden-file tests for PulpEffect (separate to avoid enum collision with PulpGain)
add_executable(pulp-test-golden-effect test_golden_effect.cpp)
target_link_libraries(pulp-test-golden-effect PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-golden-effect)
# Negative-path tests (NaN, Inf, extreme sample rates, edge cases)
add_executable(pulp-test-negative-path test_negative_path.cpp)
target_link_libraries(pulp-test-negative-path PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-negative-path PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-negative-path)
# iOS foundation tests (platform detection, safe area geometry, touch events,
# AUv3 HostApp template shape).
pulp_add_test_suite(pulp-test-ios-foundation LIBRARIES pulp::view pulp::platform)
target_compile_definitions(pulp-test-ios-foundation PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Identity/UUID tests
pulp_add_test_suite(pulp-test-identity LIBRARIES pulp::runtime)
# WebView tests (requires PULP_BUILD_WEBVIEW — WebViewPanel::create is only compiled when ON)
if(PULP_BUILD_WEBVIEW)
    add_executable(pulp-test-webview test_web_view.cpp)
    target_link_libraries(pulp-test-webview PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-webview)
endif()

# WebView backend detection smoke — always built. Validates that
# `pulp::view::detect_webview_backend()` returns one of the documented
# identifiers ("wkwebview", "webview2", "webkitgtk", "chromium", "none")
# on every supported OS and keeps the native WebView backend identifier
# contract aligned with the public WebView guide.
if(WIN32)
    pulp_add_test_suite(pulp-test-webview-backend-detect
        LIBRARIES pulp::view
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-webview-backend-detect LIBRARIES pulp::view)
endif()

# MIDI 2.0 UMP/MPE tests
pulp_add_test_suite(pulp-test-ump-mpe LIBRARIES pulp::midi)

# MPE voice tracker tests
if(WIN32)
    pulp_add_test_suite(pulp-test-mpe-voice-tracker
        LIBRARIES pulp::midi
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-mpe-voice-tracker LIBRARIES pulp::midi)
endif()

# MPE synth voice helpers (voice, allocator, glide detector)
pulp_add_test_suite(pulp-test-mpe-synth-voice
    SOURCES test_mpe_synth_voice.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# Plugin hosting tests (scanner, signal graph)
add_executable(pulp-test-host test_host.cpp harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-host PRIVATE pulp::host Catch2::Catch2WithMain)
# On Apple, the AU host-slot tests (test_plugin_slot_au.mm) ride along — they
# reuse pulp::host + rt_allocation_probe here and must stay off non-Apple (.mm/MSVC).
if(APPLE)
  target_sources(pulp-test-host PRIVATE test_plugin_slot_au.mm)
endif()
# PulpGain_CLAP fixture wiring lives in the ROOT CMakeLists.txt after
# add_subdirectory(examples) (test/ registered first, so a TARGET guard here
# never sees it). The stress case stays in the binary for manual replay, out
# of the default CTest set (flaky; gated target below).
catch_discover_tests(pulp-test-host TEST_SPEC "~[flaky]")

# SignalGraph tests carved out of test_host.cpp to keep the parent
# Focused. No CLAP fixture needed — these exercise pure graph
# routing/topology, not plugin loading.
add_executable(pulp-test-host-signal-graph test_host_signal_graph.cpp)
target_sources(pulp-test-host-signal-graph PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-host-signal-graph PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-host-signal-graph)

# ExtensionsVisitor pure-header pattern test. No plugin loading
# required; uses lightweight mock slots to exercise dispatch.
add_executable(pulp-test-extensions-visitor test_extensions_visitor.cpp)
target_link_libraries(pulp-test-extensions-visitor PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-extensions-visitor)

# Opt-in real-plugin integration runner. Builds only when
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

# Fixture-resolver unit test. Always built (no real plugin binaries
# required, no pulp::host link), exercises the
# pinned-vs-developer-supplied lane logic that
# `pulp-test-real-plugins` shares via integration/real_plugin_fixture.hpp.
add_executable(pulp-test-real-plugin-runner-cache
    integration/test_real_plugin_fixture.cpp)
target_link_libraries(pulp-test-real-plugin-runner-cache
    PRIVATE Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-real-plugin-runner-cache PRIVATE
    PULP_REAL_PLUGINS_TOML="${CMAKE_CURRENT_SOURCE_DIR}/integration/real_plugins.toml")
catch_discover_tests(pulp-test-real-plugin-runner-cache)

# Host regression coverage — end-to-end scan / load / process /
# unload lifecycle, scanner failure modes (moduleinfo.json + manifest.ttl),
# state round-trip, and hot-reload race-cleanliness. Separate binary so a
# failing regression case fingers the guilty surface without clouding
# pulp-test-host's scope.
add_executable(pulp-test-host-regression test_host_regression.cpp)
target_link_libraries(pulp-test-host-regression
    PRIVATE pulp::host Catch2::Catch2WithMain)
# PulpGain_CLAP fixture wiring for pulp-test-host-regression lives in the ROOT
# CMakeLists.txt block after add_subdirectory(examples) (see note above).
catch_discover_tests(pulp-test-host-regression)

# Stress test for the SignalGraph hot-reload race (the tracked issue). Runs the
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
        # and only prints it on failure. The earlier `>/dev/null` shape
        # discarded failure diagnostics; the whole point of the stress
        # harness is to capture a replayable seed when an iteration
        # fails, so silently dropping Catch2's assertion context defeats
        # it.
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

# NetworkServiceDiscovery backend-dispatch tests
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
    ${CMAKE_SOURCE_DIR}/examples/pulp-pluck
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
# File a new focused test if a C++
# token-diff or Showcase class is ever added.

# WindowManager multi-window framework tests
pulp_add_test_suite(pulp-test-window-manager LIBRARIES pulp::view)

# Multi-window rendering fairness + per-window RenderLoop isolation
# Mirrors pulp-test-render-loop:
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

# ── Theme management, widgets, asset system ─────────────────────────────────

# Theme contrast utilities (WCAG AA)
pulp_add_test_suite(pulp-test-theme-contrast LIBRARIES pulp::view)

# Theme preset library (38 tweakcn + derivation)
pulp_add_test_suite(pulp-test-theme-presets LIBRARIES pulp::view)

# Button widgets — theme-token wiring + rgba()-clamp bug-fix regression guard
pulp_add_test_suite(pulp-test-buttons LIBRARIES pulp::view)

# Gap widgets — Badge/Banner/Toast/EmptyState/Stepper/Pan/Popover/Dialog/ChannelStrip
pulp_add_test_suite(pulp-test-gap-widgets LIBRARIES pulp::view)

# Widget gallery — themed board of every primitive; builds + renders
pulp_add_test_suite(pulp-test-widget-gallery LIBRARIES pulp::view)

# Design system — pulp::design umbrella module + component catalog
pulp_add_test_suite(pulp-test-design-system LIBRARIES pulp::view)

# Sampler starter — real sampler UI built from the design catalog
pulp_add_test_suite(pulp-test-sampler-starter LIBRARIES pulp::view)

# Design-system interaction — verifies the widgets are wired (knob moves, etc.)
pulp_add_test_suite(pulp-test-design-system-interaction LIBRARIES pulp::view)

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

# Web-compat test suite — CSS parsing, layout, events, visual regression
add_subdirectory(web-compat)

# ── Zero-copy benchmark ─────────────────────────────────────────────────────
#
# Tooling-only tests, gated on PULP_BENCHMARK. The perf-counter unit
# test links pulp::render, which is only added to the build when
# PULP_ENABLE_GPU=ON (see root CMakeLists.txt). Gating the test on the
# render target's presence lets -DPULP_BENCHMARK=ON -DPULP_ENABLE_GPU=OFF
# configure cleanly without a missing-target error. The integration
# test shell-outs to pulp-ui-preview which is currently
# Apple-desktop-only (same guard as the target).
# Benchmark coverage remains opt-in so normal test builds stay cheap.
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

# ── Mac platform-test harness ────────────────────────────────
#
# Catch2 fixture that brings up a hidden NSWindow + CAMetalLayer host
# WITHOUT calling makeKeyAndOrderFront / activateIgnoringOtherApps, so
# tests can exercise window_host_mac.mm code paths (set_design_viewport,
# paint_scene, mouse-coordinate inverse mapping, resize → contentsScale)
# that today are structurally untestable from plain Catch2.
#
# Covers the hidden window / capture_back_buffer_png contract plus
# synthetic mouse, wheel, and context-menu consumers.
# The target is macOS+Skia-only because it drives real NSWindow and
# CAMetalLayer objects rather than a mock platform backend.
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
    target_include_directories(pulp-test-mac-platform-harness PRIVATE
        ${CMAKE_SOURCE_DIR}/external/miniz)
    target_compile_definitions(pulp-test-mac-platform-harness PRIVATE
        PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    catch_discover_tests(pulp-test-mac-platform-harness)
    # Pin the invariant -[PulpView liveFocusedView] depends on:
    # ~View() must auto-clear focused_input_ so the
    # accessor can safely re-sync the ivar to nullptr before any deref.
    add_executable(pulp-test-mac-mousedown-stale-focus
        test_mac_mousedown_stale_focus.mm
    )
    target_link_libraries(pulp-test-mac-mousedown-stale-focus PRIVATE
        pulp::view
        Catch2::Catch2WithMain
    )
    catch_discover_tests(pulp-test-mac-mousedown-stale-focus)
endif()
if(APPLE AND NOT PULP_IOS)
    # Pin macOS performKeyEquivalent routing, text-input
    # protocol conformance, and TextEditor-specific command priority.
    add_executable(pulp-test-mac-perform-key-equivalent
        test_mac_perform_key_equivalent.mm
    )
    target_link_libraries(pulp-test-mac-perform-key-equivalent PRIVATE
        pulp::view
        Catch2::Catch2WithMain
        "-framework AppKit"
    )
    # Force-load the view object so static-lib pruning does not drop Obj-C
    # class registration; window_host_mac.mm anchors the text-input category.
    target_link_options(pulp-test-mac-perform-key-equivalent PRIVATE
        "LINKER:-force_load,${CMAKE_BINARY_DIR}/core/view/CMakeFiles/pulp-view-core.dir/platform/mac/window_host_mac.mm.o"
    )
    catch_discover_tests(pulp-test-mac-perform-key-equivalent)
endif()
