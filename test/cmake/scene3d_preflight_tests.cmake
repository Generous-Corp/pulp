# Scene3D preflight and native-gap warning test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

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
