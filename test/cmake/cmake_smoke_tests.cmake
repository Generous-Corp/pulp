# CMake, install-layout, SDK, signing, and configure smoke tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

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
    # all three platforms. Excluded from fast-CI.
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
    # but pays a configure-amortised cost — pull it out of fast-CI.
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
# Install-layout regression for optional MIDI tuning provider wrapper sources
# used by installed-SDK consumers through pulp_enable_midi_tuning_provider().
add_test(NAME cmake-pulp-install-midi-tuning-sources
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_install_midi_tuning_sources.cmake)
set_tests_properties(cmake-pulp-install-midi-tuning-sources PROPERTIES
    LABELS "cmake;sdk;midi;tuning;slow"
    TIMEOUT 120)
# Install-layout regression for Linux Skia raw_ptr compatibility source used
# by FindSkia.cmake for standalone Skia archives that omit PartitionAlloc.
add_test(NAME cmake-pulp-install-skia-compat-source
    COMMAND ${CMAKE_COMMAND}
        -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_pulp_install_skia_compat_source.cmake)
set_tests_properties(cmake-pulp-install-skia-compat-source PROPERTIES
    LABELS "cmake;sdk;skia;linux;slow"
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

    # governed-build.sh — the shipyard local-backend build wrapper. Hermetic
    # (stub tartci): asserts bounded parallelism with no tartci, lease
    # acquire+release when granted, and a non-failing leaseless fallback when
    # the lease is denied.
    add_test(NAME governed-build-wrapper
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_governed_build.sh)
    set_tests_properties(governed-build-wrapper PROPERTIES TIMEOUT 30)
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
