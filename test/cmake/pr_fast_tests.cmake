# Fast deterministic tier run on every pull request head.
#
# The required macOS gate runs the full suite only in the merge queue, where a
# failure ejects a whole batch. These tests are static repository contracts:
# lint, drift, registry-completeness and generated-manifest checks. Each reads
# the source tree (or a configured build tree), finishes in seconds, and asserts
# no wall-clock or load-dependent bound, so running them on the pull request
# head surfaces the defect on the pull request that caused it.
#
# The tier is selected with `ctest -L '^pr-fast$'`. Some members are registered
# only on some platforms or configurations, so a name that is not registered
# here is reported, not fatal. `pr-fast-tier-contract` is what keeps the tier
# honest: it fails when the label selects fewer than 50 tests or loses one of
# the members the merge queue has historically been ejected on.
#
# Adding a member: it must be deterministic, finish in a few seconds on a
# loaded gate VM, and depend on nothing but the checkout and the build tree.
# Anything timing-, device-, network- or load-sensitive stays out.
# The rack-* generator tests are out for that reason: their shared module
# index is fetched from the network into a user cache on a cold host and is
# written in place, so two of them started together can read a partial file.
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

add_test(NAME pr-fast-tier-contract
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/pr_fast_tier_check.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --ctest "${CMAKE_CTEST_COMMAND}")
add_test(NAME pr-fast-tier-selftest
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/test_pr_fast_tier_check.py")

# The native-build and iOS-compile skip classifier, including the check that
# every test/ or docs/ path non-test CMake names stays denied from the iOS skip
# allowlist. Needs tomllib.
if(Python3_VERSION VERSION_GREATER_EQUAL 3.11)
    add_test(NAME classify-changes-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_classify_changes.py")
endif()

set(PULP_PR_FAST_TESTS
    pr-fast-tier-contract
    pr-fast-tier-selftest
    advisory-macos-runner-policy
    agent-capability-manifest-check
    agent-hook-paths
    build-parallelism-guard
    bundled-font-lists-agree
    canvas-path-flush-lint
    capability-contract-gate-wiring
    catch-discover-timeout-guard
    catch-discover-timeout-guard-selftest
    classify-changes-selftest
    cmake-catch-multilabel-properties
    cmake-manifest-parse
    consumption-census-contract-control-note
    consumption-census-drift
    consumption-census-drift-description
    consumption-census-negative-contract
    consumption-census-schema
    control-authoring-examples
    ctest-duplicate-registration
    ctest-label-exclusion-guard
    ctest-measured-budgets
    decisions-contract-validate
    doxygen-installed-header-check
    dsp-provenance-audit
    example-validation-paths
    figma-rest-export
    forced-restore-lint
    fork-pr-runner-routing
    framework-neutrality
    gpu-probe-historical-v1-acceptance
    gpu-test-resource-locks
    host-quirks-catalog-parity
    import-layout-parity
    importer-differential-lab
    inspector-protocol-registry-check-selftest
    inspector-protocol-registry-complete
    interchange-concepts-drift
    interchange-matrix-docs-drift
    interchange-tables-drift
    linux-runner-lane-contract
    mac-objc-source-list-guard
    minblep-table-reproducible
    msvc-string-literal-guard
    odr-macro-gated-headers
    overlay-dismissal-wiring
    param-designator-order
    playback-negative-capability
    prepush-cannot-measure
    prepush-format-gate-wiring
    project-package-mutation-control
    pulp-uninstaller-contract
    raw-this-async-check
    refactor-baseline-manifest
    refactor-baseline-manifest-negative-contract
    refactor-downstream-consumers-manifest
    refactor-downstream-consumers-negative-contract
    repo-source-scan
    sample-region-catalog-contract
    sample-region-i2-rt02-process-contract
    scene3d-cmake-boundary
    scene3d-cmake-boundary-negative-contract
    sdk-plist-templates-installed
    sequencer-control-skill-coverage
    setup-bootstrap-plan-guard
    skills-doc-sync
    skip-not-pass-lint
    skip-not-pass-lint-selftest
    style-dedup-table-sync
    tart-home-resolution
    thread-safe-assertions
    timeline-command-doc-coverage
    timeline-engine-dependency-floor
    timeline-fixture-coverage
    timeline-launcher-bulk-build-contract
    timeline-launcher-resource-contract-mutation-control
    timeline-launcher-retained-size-contract
    timeline-mcp-drift
    timeline-schema-cli-drift
    timeline-schema-js-drift
    timeline-schema-ts-drift
    timeline-transaction-launcher-complexity
    timeline-transaction-launcher-complexity-mutation-control
    token-coverage-ratchet
    token-key-correctness
    tools-registry-check
    trace-span-category-lint
    vellum-workflow-dispatch
    vst3-bundle-layout
    wasm-skia-fetch-mapping
    web-timeline-source-closure
    yoga-oracle-pin-lockstep
)

set(_pulp_pr_fast_missing "")
foreach(_pulp_pr_fast_test IN LISTS PULP_PR_FAST_TESTS)
    if(TEST "${_pulp_pr_fast_test}")
        set_property(TEST "${_pulp_pr_fast_test}" APPEND PROPERTY LABELS pr-fast)
    else()
        list(APPEND _pulp_pr_fast_missing "${_pulp_pr_fast_test}")
    endif()
endforeach()
if(_pulp_pr_fast_missing)
    message(STATUS
        "pr-fast tier: not registered in this configuration: "
        "${_pulp_pr_fast_missing}")
endif()
