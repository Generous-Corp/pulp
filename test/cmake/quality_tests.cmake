# Build checks, Python ratchets, and test-manifest refactor guards.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Build check (no Catch2 dependency)
add_executable(pulp-test-build-check test_build_check.cpp)
target_link_libraries(pulp-test-build-check PRIVATE pulp::platform pulp::runtime)
add_test(NAME build-check COMMAND pulp-test-build-check)

# Installed agent capability manifest: curated snapshot, negative validation,
# and compile proof for every advertised include/symbol pair.
add_executable(pulp-test-agent-capability-compile test_agent_capability_compile.cpp)
target_link_libraries(pulp-test-agent-capability-compile PRIVATE
    pulp::audio
    pulp::midi
    pulp::music
    pulp::sequence
    pulp::signal
    pulp::timebase)
add_test(NAME agent-capability-symbols-compile COMMAND pulp-test-agent-capability-compile)

if(Python3_Interpreter_FOUND)
    add_test(NAME agent-capability-manifest-check
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/agent_capability_manifest.py" --check)
    add_test(NAME agent-capability-manifest-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_agent_capability_manifest.py")
    set_tests_properties(agent-capability-manifest-selftest PROPERTIES PROCESSORS 8)
    add_test(NAME agent-capability-rederive-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_agent_capability_rederive.py")
    # The rederive self-test deliberately rewrites the manifest script's two
    # generated counters before restoring them. Keep readers from observing
    # that temporary state while retaining parallelism for unrelated tests.
    set_tests_properties(
        agent-capability-manifest-check
        agent-capability-manifest-selftest
        agent-capability-rederive-selftest
        PROPERTIES RESOURCE_LOCK agent-capability-manifest-source)
    add_test(NAME agent-capability-sdk-handoff-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_sdk_capability_handoff.py")
    add_test(NAME agent-capability-installed-sdk-helper-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_agent_capability_installed_sdk_helpers.py")
    set(_pulp_agent_capability_installed_args
        --build-dir "${CMAKE_BINARY_DIR}"
        --cmake "${CMAKE_COMMAND}"
        --generator "${CMAKE_GENERATOR}"
        "--config=$<CONFIG>")
    set(_pulp_agent_capability_instrumentation_compile_flags
        ${PULP_SANITIZER_COMPILE_FLAGS}
        ${PULP_COVERAGE_COMPILE_FLAGS})
    set(_pulp_agent_capability_instrumentation_link_flags
        ${PULP_SANITIZER_LINK_FLAGS}
        ${PULP_COVERAGE_LINK_FLAGS})
    string(JOIN " " _pulp_agent_capability_instrumentation_compile_flags
        ${_pulp_agent_capability_instrumentation_compile_flags})
    string(JOIN " " _pulp_agent_capability_instrumentation_link_flags
        ${_pulp_agent_capability_instrumentation_link_flags})
    if(_pulp_agent_capability_instrumentation_compile_flags)
        list(APPEND _pulp_agent_capability_installed_args
            "--instrumentation-cxx-flags=${_pulp_agent_capability_instrumentation_compile_flags}")
    endif()
    if(_pulp_agent_capability_instrumentation_link_flags)
        list(APPEND _pulp_agent_capability_installed_args
            "--instrumentation-linker-flags=${_pulp_agent_capability_instrumentation_link_flags}")
    endif()
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND _pulp_agent_capability_installed_args
            --generator-platform "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND _pulp_agent_capability_installed_args
            --generator-toolset "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _pulp_agent_capability_installed_args
            --toolchain-file "${CMAKE_TOOLCHAIN_FILE}")
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        string(REPLACE ";" "\\;" _pulp_agent_capability_architectures
            "${CMAKE_OSX_ARCHITECTURES}")
        list(APPEND _pulp_agent_capability_installed_args
            --osx-architectures "${_pulp_agent_capability_architectures}")
        unset(_pulp_agent_capability_architectures)
    endif()
    if(CMAKE_OSX_SYSROOT)
        list(APPEND _pulp_agent_capability_installed_args
            --osx-sysroot "${CMAKE_OSX_SYSROOT}")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND _pulp_agent_capability_installed_args
            --osx-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    add_test(NAME agent-capability-installed-sdk
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_agent_capability_installed_sdk.py"
            ${_pulp_agent_capability_installed_args})
    set_tests_properties(agent-capability-installed-sdk PROPERTIES
        LABELS "slow;agent-capability-installed-sdk"
        RESOURCE_LOCK agent-capability-manifest-source)
    unset(_pulp_agent_capability_installed_args)
    unset(_pulp_agent_capability_instrumentation_compile_flags)
    unset(_pulp_agent_capability_instrumentation_link_flags)
    # This installs the SDK, then configures, builds, and runs an independent
    # consumer for every capability row and typed binding.
    # A cold Linux runner configures, builds, and runs hundreds of isolated
    # capability/binding consumers. Ten minutes is below the observed clean
    # runtime and turns ctest's retry into cross-test pollution. Keep it out of
    # the ordinary PR corpus via the `slow` label; build.yml restores it on the
    # required macOS leg when an installed-capability surface actually changes,
    # and nightly-full-build still runs the complete unfiltered inventory.
    set_tests_properties(agent-capability-installed-sdk PROPERTIES TIMEOUT 1200)

    add_test(NAME control-authoring-examples
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/test/test_control_authoring_examples.py")
    set_tests_properties(control-authoring-examples PROPERTIES
        LABELS "inspect;control;docs;examples")

    add_test(NAME ci-python-selector-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_find_python311.py")
    if(UNIX)
        add_test(NAME linux-runner-lane-contract COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_linux_runner_lane_contract.py")
    endif()
    add_test(NAME inspector-protocol-registry-complete
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tools/scripts/check_inspector_protocol_registry.py
            --root ${PROJECT_SOURCE_DIR})
    add_test(NAME inspector-protocol-registry-check-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tools/scripts/check_inspector_protocol_registry.py
            --self-test)
    add_test(NAME auval-helper-worker-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_run_auval_component.py")
    if(UNIX)
        add_test(NAME process-deadline-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_run_with_deadline.py")
    endif()

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

    # Unbounded-wait lint: a test wait that cannot time out turns a real
    # regression into a CI job timeout with no output. The selftest is the
    # load-bearing part — it scans the SAME wait unbounded and bounded, so the
    # gate is proven to distinguish them rather than proven to be quiet.
    add_test(NAME unbounded-wait-lint-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_unbounded_wait_lint.py")

    # Build-parallelism guard: fail on a bare `--parallel` / `-j` (no job count)
    # in any tracked build command. Bare `--parallel` maps to unbounded `make
    # -j`, which can exhaust memory / oversubscribe cores on a shared machine.
    add_test(NAME build-parallelism-guard COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/build_parallelism_guard.py")
    add_test(NAME build-parallelism-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_build_parallelism_guard.py")
    # A fork's code must never be routed onto the self-hosted Macs, which hold
    # the signing keychain. Runs the resolver build.yml actually embeds.
    add_test(NAME fork-pr-runner-routing COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_fork_pr_runner_routing.py")
    set_tests_properties(fork-pr-runner-routing PROPERTIES TIMEOUT 120)
    # Workflow-lint installs PyYAML and owns the YAML-dependent throughput
    # contracts. Do not register that script here: required macOS CTest hosts
    # lack PyYAML, and a skipped unittest suite would report a false green.
    add_test(NAME example-validation-paths COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_example_validation_paths.py")
    set_tests_properties(example-validation-paths PROPERTIES TIMEOUT 120)
    # Both Vellum gates post or gate a required check and grew a manual
    # dispatch path. Runs the resolve step the workflow actually embeds.
    add_test(NAME vellum-workflow-dispatch COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_vellum_workflow_dispatch.py")
    set_tests_properties(vellum-workflow-dispatch PROPERTIES TIMEOUT 120)

    # Development-inspector truth gate: mutation tests prove capability/profile
    # and user-facing runtime claims fail when either side drifts.
    add_test(NAME inspector-truth-check-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_inspector_truth_check.py")

    # Product B collaboration remains an explicit NO-GO. Scan the complete
    # shipped source/docs surface and the CLI/broker binaries after the build.
    set(_control_product_b_absence_args --root "${CMAKE_SOURCE_DIR}")
    if(TARGET pulp-cli)
        list(APPEND _control_product_b_absence_args
            --binary "$<TARGET_FILE:pulp-cli>")
    endif()
    if(TARGET pulp-control-broker)
        list(APPEND _control_product_b_absence_args
            --binary "$<TARGET_FILE:pulp-control-broker>")
    endif()
    add_test(NAME control-product-b-absence COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/control_product_b_absence_check.py"
        ${_control_product_b_absence_args})
    set_tests_properties(control-product-b-absence PROPERTIES LABELS "inspect;control;docs")
    add_test(NAME control-product-b-absence-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_control_product_b_absence_check.py")

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

    # ODR macro-gated-header guard. A macro-gated inline/template function in a
    # header, plus a TU that redefines that macro, is an ODR violation a Release
    # lane provably CANNOT see: at -O3 each TU inlines its own copy so the A/B
    # test appears to work, and Release is green by construction.
    #
    # `.shipyard/config.toml` states this class is "Guarded by
    # tools/scripts/test_odr_macro_gated_headers.py". Until this registration
    # that script ran NOWHERE — not a ctest, not a workflow, not check-docs.sh —
    # so the claim was true of the file's existence and false of its execution.
    # The only live enforcement was the Shipyard mac lane's Debug build, which is
    # `backend = local` and so yields the signal only on hosts big enough to
    # finish it; on a small one the class simply goes unchecked. A static check on
    # the required gate makes the enforcement host-independent, which is what the
    # config always implied. Stdlib-only and runs on Python 3.9, so it carries no
    # false-green risk on the required macOS hosts.
    add_test(NAME odr-macro-gated-headers COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_odr_macro_gated_headers.py")
    set_tests_properties(odr-macro-gated-headers PROPERTIES TIMEOUT 120)

    # Live-build check: reports a governed build running in THIS checkout, which
    # Shipyard's local mac backend does by design. Its one job is to tell a live
    # marker from the one a killed build necessarily leaves behind, so the test
    # asserts both directions — a presence-keyed check would pass a one-way test
    # while warning on every dead build until nobody reads it. Pure Python, no
    # lease store, no build.
    add_test(NAME live-build-check-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_live_build_check.py")
    set_tests_properties(live-build-check-selftest PROPERTIES TIMEOUT 120)

    # Combined installer graph: fake the macOS signing/package tools and inspect
    # the generated Distribution XML. This pins unique plugin+format package IDs
    # and the multi-plugin nested outline without using credentials or bundles.
    if(APPLE)
        add_test(NAME combined-installer-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_build_combined_installer.py")
    endif()

    # An Info.plist template reachable by a format helper but absent from the
    # SDK install list does not error — PulpPluginFormats selects with
    # `elseif(EXISTS ...)`, so the helper falls through and consumer builds get
    # bundles with an empty CFBundleIdentifier. Pure text comparison, so it runs
    # everywhere rather than only on Apple.
    add_test(NAME sdk-plist-templates-installed COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_sdk_plist_templates_installed.py")

    # TART_HOME resolution: the Tart VM store is a per-host value, so the VM
    # tooling must read it from the host (env, else the tartci profile) and hard
    # error otherwise. A repo-side default is wrong on some host and wrong
    # silently — `tart list` against the wrong store is an empty list, not an
    # error, so the stray-VM reaper would report success while reaping nothing.
    add_test(NAME tart-home-resolution COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_tart_home_resolution.py")

    # Fleet runner policy: pin the Actions runner without auto-update, keep
    # system tools ahead of Homebrew, isolate Rust state under each runner, and
    # refuse recovery while a Worker is active.
    add_test(NAME fleet-runner-policy-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/fleet/test_fleet_lib.py")

    # Planning-gitlink guard: reject an accidental `planning` submodule pointer
    # bump (a `git reset --hard` + `git add -A` re-staging the drifted gitlink);
    # a deliberate re-pin passes with a `Planning-Bump:` trailer.
    add_test(NAME planning-gitlink-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_planning_gitlink_guard.py")

    # Gate substrate: the trailer collection and glob matching every gate
    # shares. test_gates.py re-exports this module's TestCase classes by name,
    # so a class added here is invisible to CI until that import list is also
    # updated; registering the file directly covers it either way.
    add_test(NAME gate-common-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_gate_common.py")

    # Runner-topology guard: pure reconciliation logic (label matching, the
    # black-hole / offline / ephemeral-idle distinction, contract drift) plus a
    # well-formedness check of the shipped routing contract. No network — the
    # live-fleet half runs on a schedule in runner-topology-check.yml, since
    # that invariant breaks with no commit involved.
    add_test(NAME runner-topology-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_runner_topology_check.py")
    add_test(NAME native-intel-runner-group-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_verify_native_intel_runner_group.py")
    add_test(NAME linux-runner-group-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_verify_linux_runner_group.py")
    if(UNIX)
        add_test(NAME native-intel-runner-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_native_intel_runner.py")
        add_test(NAME portable-ci-timeout-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_run_with_timeout.py")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME proxmox-ephemeral-linux-runner-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_proxmox_ephemeral_runner_linux.py")
        add_test(NAME proxmox-ephemeral-linux-reaper-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_proxmox_ephemeral_reap_linux.py")
        add_test(NAME proxmox-ci-host-network-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/ci/test_configure_proxmox_ci_network.py")
    endif()

    # Silent-revert guard: reject a push whose diff byte-exactly restores the
    # pre-landing bytes of every file a recent commit changed. Includes a replay
    # of the real shape from this repo's own history; that half self-skips on a
    # shallow clone where the commits are absent.
    add_test(NAME silent-revert-guard-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_silent_revert_guard.py")

    # GPU resource-lock policy: a suite that stands up a real Dawn/Metal device
    # must serialize on RESOURCE_LOCK pulp_gpu. Reads the test manifests only —
    # no GPU, no configure — so it runs on every host, including the GPU-off
    # builds where the guarded suites are not even registered. That matters:
    # a missing lock shows up as an intermittent short readback on somebody
    # else's unrelated PR, which is exactly the signal nobody attributes here.
    add_test(NAME gpu-test-resource-locks COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_gpu_test_resource_locks.py")

    # PR-check triage: comparison logic that labels a red PR check as
    # pre-existing-on-main vs regressed-by-this-PR (the "also-red-on-main"
    # diagnostic), plus the CLI's paginated check-run decoding.
    add_test(NAME pr-check-triage-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_pr_check_triage.py")

    # Decisions contract: assert the shipped .agents/contract.toml is always
    # schema-valid (a broken contract must fail the build, not CI).
    add_test(NAME decisions-contract-validate COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/decisions_contract.py" --mode validate)
    # Self-test: the read surface (surface/list/validate), the external-contributor
    # no-op (non-fleet paths surface nothing), the agent-neutral hint hook, and
    # the AGENTS.md + CLAUDE.md pointers.
    add_test(NAME decisions-contract-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_decisions_contract.py")

    # Changed-surface selection remains shadow-only. Python 3.11 is required
    # for stdlib tomllib. Ordinary developer configurations run the static
    # policy contract only; the exact inventory check is explicitly enabled by
    # Shipyard's declared target so a different optional feature set cannot be
    # mistaken for inventory drift.
    if(Python3_VERSION VERSION_GREATER_EQUAL 3.11)
        set(_changed_surface_policy_args)
        if(PULP_CHANGED_SURFACE_INVENTORY_TARGET)
            list(APPEND _changed_surface_policy_args --build-dir "${CMAKE_BINARY_DIR}")
        endif()
        add_test(NAME changed-surface-policy-selftest COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_changed_surface_policy.py"
            ${_changed_surface_policy_args})
    endif()

    # Format-baseline diff: exit-code routing (skip vs fail vs diff) and the
    # --diag-dir contract that copies captured validator output out of the temp
    # dir before it is deleted. Runs the validators nowhere — the capture
    # subprocess is mocked — so it needs no plugin bundles or macOS validators.
    add_test(NAME format-baseline-diff-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_format_baseline_diff.py")

    # Advisory macOS jobs must never consume the required merge-gate labels.
    # This validates both the fail-closed resolver and every workflow that uses
    # the advisory selector.
    add_test(NAME advisory-macos-runner-policy COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_advisory_macos_runner_policy.py")

    # DSP vocabulary: derives what pulp::signal exposes by parsing its headers,
    # for anything that has to tell a model or a generator which DSP exists.
    # The parse is regex-based, so a break would shrink the vocabulary silently
    # and its consumers would quietly stop reaching for the SDK -- which has
    # happened. The self-test pins a floor on the totals and on the classes
    # that are actually depended on.
    add_test(NAME dsp-vocabulary-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/test_dsp_vocabulary.py")

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

        # The two build-directory reapers. Both DELETE directories, so their
        # gates are the thing under test: each builds a throwaway git
        # repository with real worktrees in every state its gate
        # distinguishes, and asserts what survives as hard as what goes.
        # Fixture-only; nothing outside the temporary directory is reachable.
        # clean_build_cov.sh's suite existed but was registered nowhere, so it
        # had never run in CI — a reaper whose guard nothing exercises is the
        # dangerous kind.
        add_test(NAME clean-build-cov-selftest
            COMMAND ${Python3_EXECUTABLE} -m unittest test_clean_build_cov
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/scripts")
        set_tests_properties(clean-build-cov-selftest PROPERTIES TIMEOUT 300)

        add_test(NAME clean-worktree-builds-selftest
            COMMAND ${Python3_EXECUTABLE} -m unittest test_clean_worktree_builds
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/scripts")
        set_tests_properties(clean-worktree-builds-selftest PROPERTIES TIMEOUT 600)
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
    add_test(NAME verify-rendered-panel-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_verify_rendered_panel.py")
    # Presence checks over a rendered panel: every one of the five is proved in
    # both directions on a synthetic capture — green on the reference as its
    # own render, red on the same render with one defect painted in. A check
    # nobody has watched fail is not known to be able to fail, which is how
    # several instruments in this area read as clean while measuring nothing.
    # 77 is SKIPPED, not passed: without numpy and Pillow not one of the seeded
    # defects can be painted, and a green tick over a suite that ran nothing is
    # the exact failure this suite exists to rule out.
    add_test(NAME check-panel-presence-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_check_panel_presence.py")
    set_tests_properties(check-panel-presence-selftest PROPERTIES
        TIMEOUT 300 SKIP_RETURN_CODE 77)

    # The agent-panel gate's own failure classifier. The negative fixture
    # passes by being REFUSED, so which of three verdicts this returns is the
    # only thing between "the clipping gate still fires" and "nobody noticed it
    # stopped". Pure Python, no fixtures, no browser -- so unlike the gate it
    # guards, this one always runs.
    add_test(NAME agent-panel-gate-classifier-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/import-validation/test_check_agent_panel_invariants.py")
    set_tests_properties(agent-panel-gate-classifier-selftest PROPERTIES
        TIMEOUT 120)

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
# Catch2 discovery must preserve multi-label lists as one CTest property value.
if(Python3_Interpreter_FOUND)
    add_test(
        NAME cmake-catch-multilabel-properties
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tools/scripts/test_catch_multilabel_properties.py)
    set_tests_properties(cmake-catch-multilabel-properties PROPERTIES
        LABELS "cmake;ci"
        TIMEOUT 120)
endif()
