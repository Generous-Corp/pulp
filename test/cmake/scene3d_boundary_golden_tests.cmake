# Scene3D boundary and golden contract test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

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
                "final renderer goldens require a pixel-producing software adapter")

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
