set(PULP_REFACTOR_BASELINE_FIXTURE
    "${CMAKE_CURRENT_LIST_DIR}/../fixtures/refactor-baselines/phase0-hotspots.json")

add_test(NAME refactor-baseline-manifest
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_refactor_baselines.py"
        "${PULP_REFACTOR_BASELINE_FIXTURE}"
        --repo-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(refactor-baseline-manifest PROPERTIES
    PASS_REGULAR_EXPRESSION
        "refactor_baselines_verified=true hotspots=2 renderer_golden_entries=2")

add_test(NAME refactor-baseline-manifest-negative-contract
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_refactor_baselines_contract.py"
        --validator
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_refactor_baselines.py"
        --manifest
        "${PULP_REFACTOR_BASELINE_FIXTURE}"
        --repo-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(refactor-baseline-manifest-negative-contract
    PROPERTIES
    PASS_REGULAR_EXPRESSION
        "refactor_baseline_contract_case=missing-root-key.*refactor_baseline_contract_case=invalid-build-type.*refactor_baseline_contract_case=missing-hotspot.*refactor_baseline_contract_case=missing-ccache-disable.*refactor_baseline_contract_case=median-drift.*refactor_baseline_contract_case=missing-source.*refactor_baseline_contract_case=missing-ctest.*refactor_baseline_contract_case=missing-renderer-manifest.*refactor_baseline_contract_case=interim-without-final-gate.*refactor_baseline_contract_verified=true")

unset(PULP_REFACTOR_BASELINE_FIXTURE)
