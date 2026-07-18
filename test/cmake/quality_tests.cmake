# Build checks, Python ratchets, and test-manifest refactor guards.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

if(Python3_Interpreter_FOUND)
    # Playback is engine-core: format/host/view may consume it, but it may not
    # include or link back upward. The selftest proves every forbidden layer is
    # actually detected in both source includes and CMake linkage.
    add_test(NAME timeline-engine-dependency-floor COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/playback_dependency_floor_check.py")
    add_test(NAME timeline-engine-dependency-floor-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/playback_dependency_floor_check.py"
        --selftest)

    add_test(NAME timeline-dependency-floor COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_dependency_floor_check.py")
    add_test(NAME timeline-dependency-floor-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/timeline_dependency_floor_check.py" --selftest)
    # Reskinnability ratchet: fail on a NEW hardcoded theme color.
    add_test(NAME token-coverage-ratchet COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/token_coverage_check.py")

    # Token-key correctness: fail on resolve_color("typo.key", …) that isn't a
    # real theme token (silently renders the hardcoded fallback, breaks reskin).
    add_test(NAME token-key-correctness COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/token_key_check.py")

    # Framework neutrality: fail on a foreign framework named in Pulp's own
    # source, or on a foreign class name adopted into Pulp's API. The selftest
    # seeds a violation of each half and asserts the checker flags it — a gate
    # that cannot fail is not a gate.
    add_test(NAME framework-neutrality COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/framework_neutrality_check.py")
    add_test(NAME framework-neutrality-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/framework_neutrality_check.py"
        --selftest)

    # Lifetime guard: a newly introduced worker thread may not capture a raw
    # owner without a stable, reasoned review entry. The selftest proves the
    # lexical gate can detect all supported hazardous shapes.
    add_test(NAME raw-this-async-check COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/raw_this_async_check.py")
    add_test(NAME raw-this-async-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/raw_this_async_check.py" --selftest)

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

    # Governed-build wrapper: the bound on Shipyard's `local` mac backend, which
    # runs the build string directly on the host and so never sees the pulp
    # CLI's lease integration. Pins the lease-denial contract (back off to the
    # store's reported capacity, floor when none) with a stub tartci — no
    # compile, no lease store, no host-size dependency. UNIX-only: it drives a
    # bash script, and the wrapper only runs on the macOS/Linux CI hosts.
    if(UNIX)
        add_test(NAME governed-build-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_governed_build.py")
        set_tests_properties(governed-build-selftest PROPERTIES TIMEOUT 120)
    endif()

    # Combined installer graph: fake the macOS signing/package tools and inspect
    # the generated Distribution XML. This pins unique plugin+format package IDs
    # and the multi-plugin nested outline without using credentials or bundles.
    if(APPLE)
        add_test(NAME combined-installer-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_build_combined_installer.py")
    endif()

    # TART_HOME resolution: the Tart VM store is a per-host value, so the VM
    # tooling must read it from the host (env, else the tartci profile) and hard
    # error otherwise. A repo-side default is wrong on some host and wrong
    # silently — `tart list` against the wrong store is an empty list, not an
    # error, so the stray-VM reaper would report success while reaping nothing.
    add_test(NAME tart-home-resolution COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_tart_home_resolution.py")

    # Planning-gitlink guard: reject an accidental `planning` submodule pointer
    # bump (a `git reset --hard` + `git add -A` re-staging the drifted gitlink);
    # a deliberate re-pin passes with a `Planning-Bump:` trailer.
    add_test(NAME planning-gitlink-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_planning_gitlink_guard.py")

    # Runner-topology guard: pure reconciliation logic (label matching, the
    # black-hole / offline / ephemeral-idle distinction, contract drift) plus a
    # well-formedness check of the shipped routing contract. No network — the
    # live-fleet half runs on a schedule in runner-topology-check.yml, since
    # that invariant breaks with no commit involved.
    add_test(NAME runner-topology-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_runner_topology_check.py")

    # PR-check triage: pure comparison logic that labels a red PR check as
    # pre-existing-on-main vs regressed-by-this-PR (the "also-red-on-main"
    # diagnostic). The CLI half is a thin gh-api wrapper (not unit-tested).
    add_test(NAME pr-check-triage-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_pr_check_triage.py")

    # Format-baseline diff: exit-code routing (skip vs fail vs diff) and the
    # --diag-dir contract that copies captured validator output out of the temp
    # dir before it is deleted. Runs the validators nowhere — the capture
    # subprocess is mocked — so it needs no plugin bundles or macOS validators.
    add_test(NAME format-baseline-diff-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_format_baseline_diff.py")

    # Version-at-land: single-writer version assignment on main. Drives
    # plan_assignments over throwaway git ranges, asserting it reproduces the
    # same path + conventional-commit heuristic the hand-bump model uses (the
    # live assess_surfaces heuristic, not positive intent trailers), and covers
    # the drain-base / apply_and_push transaction (two concurrent post-merge
    # drains apply a version exactly once, guarded by the Version-Bump-Applied
    # idempotency marker). Needs a working `git`.
    add_test(NAME version-at-land-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_version_at_land.py")

    # min-OS measurement: --measure/--elf floor derivation over a built binary
    # (magic-byte format detection + Mach-O/ELF/PE/ar readers). The primitive the
    # SDK-consumer sweep calls per artifact.
    add_test(NAME measure-min-os-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_measure_min_os.py")

    # Bundle-architecture gate (G3): the pure arch/signature decision core plus
    # a real thin/fat Mach-O fixture pass (the fixture layer skips when the
    # Apple toolchain is absent). Guards macOS universal builds against a thin
    # embedded dylib or an unsigned raw-lipo fat dylib.
    add_test(NAME check-bundle-architectures-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_check_bundle_architectures.py")

    # FindSkia macOS arch assertion (G3): a real cmake-configure negative test
    # proving the lipo -archs guard FATALs on a wrong-arch Skia archive and
    # passes on a matching one (skips off-macOS / without clang+ar+lipo).
    add_test(NAME findskia-arch-assert-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_findskia_arch_assert.py")

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

    # local_diff_cover.sh contracts: config-key consumption, the per-worktree
    # report path, and build-cov mutual exclusion. Fixture-only — it never runs
    # a coverage build. The lock cases spawn real concurrent shells and wait on
    # each other, so this is seconds rather than milliseconds.
    if(UNIX)
        add_test(NAME local-diff-cover-selftest
            COMMAND ${Python3_EXECUTABLE} -m unittest test_local_diff_cover
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/scripts")
        set_tests_properties(local-diff-cover-selftest PROPERTIES TIMEOUT 300)
    endif()
    # Tool registry: docs/status/tools.yaml must stay valid (every path and
    # invocation resolves) AND complete (every committed entry point under the
    # swept dirs is registered or excluded), and the CLAUDE.md digest generated
    # from it must be in sync. This is what keeps agents from hand-rolling a
    # script for a job a shipped tool already does.
    add_test(NAME tools-registry-check COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/tools_registry_check.py" --check)
    add_test(NAME tools-registry-check-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_tools_registry_check.py")

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

# setup.sh's shared source cache: seeding a re-pinned dependency from a cache
# of a DIFFERENT version that is already on the machine. Drives throwaway
# file:// repos, so it needs no network and touches no real cache.
if(UNIX)
    add_test(NAME setup-source-cache
        COMMAND bash "${CMAKE_SOURCE_DIR}/tools/scripts/test_setup_source_cache.sh")
endif()
