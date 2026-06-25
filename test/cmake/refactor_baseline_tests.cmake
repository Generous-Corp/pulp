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

set(PULP_DOWNSTREAM_CONSUMERS_FIXTURE
    "${CMAKE_CURRENT_LIST_DIR}/../fixtures/refactor-baselines/downstream-consumers.json")

add_test(NAME refactor-downstream-consumers-manifest
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_downstream_consumers.py"
        "${PULP_DOWNSTREAM_CONSUMERS_FIXTURE}")
set_tests_properties(refactor-downstream-consumers-manifest PROPERTIES
    PASS_REGULAR_EXPRESSION
        "downstream_validation_manifest_verified=true consumers=3 commands=9")

add_test(NAME refactor-downstream-consumers-negative-contract
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_downstream_consumers_contract.py"
        --validator
        "${CMAKE_SOURCE_DIR}/tools/scripts/verify_downstream_consumers.py"
        --manifest
        "${PULP_DOWNSTREAM_CONSUMERS_FIXTURE}")
set_tests_properties(refactor-downstream-consumers-negative-contract
    PROPERTIES
    PASS_REGULAR_EXPRESSION
        "downstream_manifest_contract_case=valid-current.*downstream_manifest_contract_case=missing-root-key.*downstream_manifest_contract_case=invalid-sdk-recipe.*downstream_manifest_contract_case=duplicate-consumer.*downstream_manifest_contract_case=invalid-sha.*downstream_manifest_contract_case=missing-command.*downstream_manifest_contract_case=missing-expected.*downstream_manifest_contract_case=project-design-merge.*downstream_manifest_contract_case=adapter-without-sdk.*downstream_manifest_contract_verified=true")

unset(PULP_DOWNSTREAM_CONSUMERS_FIXTURE)
