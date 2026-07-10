# Build checks, Python ratchets, and test-manifest refactor guards.
# Included by test/CMakeLists.txt; keep related test registrations here.

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

    # Thread-safe assertions: fail on a Catch2 assertion macro invoked inside a
    # worker-thread lambda. Catch2 3.7.1's macros aren't thread-safe; off-main
    # assertions are UB that bare metal tolerates but VM scheduler timing trips.
    add_test(NAME thread-safe-assertions COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/thread_assert_check.py")

    # Build-parallelism guard: fail on a bare `--parallel` / `-j` (no job count)
    # in any tracked build command. Bare `--parallel` maps to unbounded `make
    # -j`, which can exhaust memory / oversubscribe cores on a shared machine.
    add_test(NAME build-parallelism-guard COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/build_parallelism_guard.py")
    add_test(NAME build-parallelism-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_build_parallelism_guard.py")

    # Planning-gitlink guard: reject an accidental `planning` submodule pointer
    # bump (a `git reset --hard` + `git add -A` re-staging the drifted gitlink);
    # a deliberate re-pin passes with a `Planning-Bump:` trailer.
    add_test(NAME planning-gitlink-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_planning_gitlink_guard.py")

    # PR-check triage: pure comparison logic that labels a red PR check as
    # pre-existing-on-main vs regressed-by-this-PR (the "also-red-on-main"
    # diagnostic). The CLI half is a thin gh-api wrapper (not unit-tested).
    add_test(NAME pr-check-triage-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_pr_check_triage.py")

    # Version-at-land (T1.1): single-writer version assignment from Version-Bump
    # intent trailers. Pure aggregate_intent / plan_assignments logic (the bot
    # is dry-run only at this stage).
    add_test(NAME version-at-land-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_version_at_land.py")

    # min-OS measurement: --measure/--elf floor derivation over a built binary
    # (magic-byte format detection + Mach-O/ELF/PE/ar readers). The primitive the
    # SDK-consumer sweep calls per artifact.
    add_test(NAME measure-min-os-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_measure_min_os.py")

    # SDK-consumer sweep: the WHAT-builds / WHICH-binaries-measured / HOW-reported
    # decision logic of the turnkey runner that rebuilds every downstream consumer
    # against one installed SDK and checks the min-OS floor propagated.
    add_test(NAME sdk-consumer-sweep-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_sdk_consumer_sweep.py")

    # SDK-consumer update: SDK-pin detection/rewrite across pin forms, per-repo
    # update planning, the buildable-consumer filter, and the publish runbook —
    # the pure logic behind `pulp minos update` / `publish-runbook`.
    add_test(NAME sdk-consumer-update-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_sdk_consumer_update.py")

    # The consumer registry lives in the private `planning` submodule. Both the
    # scripts and the two CTest cases that read it must agree on how its absence
    # is announced, or those cases fail on every checkout that lacks planning.
    add_test(NAME minos-registry-absent-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_minos_registry_absent.py")

    # Skills catalog: docs/reference/skills.md is generated from every skill's
    # SKILL.md frontmatter. This runs in the required gate so adding or renaming a
    # skill without regenerating the catalog (or leaving one with no real
    # description) fails CI with the exact `--write` fix — the catalog can't drift.
    add_test(NAME skills-doc-sync COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/skills_doc_check.py" --check)
    add_test(NAME skills-doc-check-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_skills_doc_check.py")

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

endif()
