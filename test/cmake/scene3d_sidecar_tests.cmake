# Scene3D sidecar and adapter test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

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
