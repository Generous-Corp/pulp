# Renderer3D probe contract, inventory, and native-boundary checks.
# Included by test/CMakeLists.txt; keep related test registrations here.

        add_test(NAME scene3d-renderer-probe-verifier-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest
                "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json")
        set_tests_properties(scene3d-renderer-probe-verifier-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_contract_case=valid-fake-probe.*renderer_probe_contract_case=missing-resource-field.*renderer_probe_contract_case=scene-data-consumption-drift.*renderer_probe_contract_case=pixel-output-drift.*renderer_probe_contract_case=coverage-floor-drift.*renderer_probe_contract_case=manifest-missing-entry-field.*renderer_probe_contract_case=manifest-extra-entry-field.*renderer_probe_contract_case=manifest-software-adapter-drift.*renderer_probe_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-public-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface.py"
                --renderer-header
                "${CMAKE_SOURCE_DIR}/core/render/include/pulp/render/renderer3d.hpp"
                --probe-source
                "${CMAKE_SOURCE_DIR}/core/render/src/renderer3d_probe.cpp"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
        set_tests_properties(scene3d-renderer-probe-public-surface-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_public_surface_verified=79 bools")

        add_test(NAME scene3d-renderer-probe-public-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_public_surface.py"
                --renderer-header
                "${CMAKE_SOURCE_DIR}/core/render/include/pulp/render/renderer3d.hpp"
                --probe-source
                "${CMAKE_SOURCE_DIR}/core/render/src/renderer3d_probe.cpp"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
        set_tests_properties(scene3d-renderer-probe-public-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_public_surface_contract_case=valid-current-surface.*renderer_probe_public_surface_contract_case=header-bool-removed.*renderer_probe_public_surface_contract_case=probe-print-removed.*renderer_probe_public_surface_contract_case=verifier-field-removed.*renderer_probe_public_surface_contract_case=probe-key-mismatch.*renderer_probe_public_surface_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-fake-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --probe-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --final-eligibility-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
        set_tests_properties(scene3d-renderer-probe-fake-surface-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_fake_surface_verified=probe-contract-lines.*renderer_probe_fake_surface_verified=final-eligibility-true.*renderer_probe_fake_surface_verified=final-eligibility-false.*renderer_probe_fake_surfaces_verified=true")

        add_test(NAME scene3d-renderer-probe-fake-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_fake_surfaces.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --probe-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_contract.py"
                --final-eligibility-contract
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
        set_tests_properties(scene3d-renderer-probe-fake-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_fake_surface_contract_case=valid-current-fake-surfaces.*renderer_probe_fake_surface_contract_case=probe-contract-missing-field.*renderer_probe_fake_surface_contract_case=final-eligibility-missing-resource-field.*renderer_probe_fake_surface_contract_case=final-eligibility-missing-final-field.*renderer_probe_fake_surface_contract_case=probe-field-added-without-fakes.*renderer_probe_fake_surface_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-manifest-schema-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest-validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py")
        set_tests_properties(scene3d-renderer-probe-manifest-schema-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_manifest_schema_verified=true")

        add_test(NAME scene3d-renderer-probe-manifest-schema-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema_contract.py"
                --schema-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_manifest_schema.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --manifest-validator
                "${CMAKE_SOURCE_DIR}/tools/scene3d/validate_renderer_golden_manifest.py")
        set_tests_properties(scene3d-renderer-probe-manifest-schema-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_manifest_schema_contract_case=valid-current-manifest-schema.*renderer_probe_manifest_schema_contract_case=probe-manifest-key-drift.*renderer_probe_manifest_schema_contract_case=probe-entry-extra-key.*renderer_probe_manifest_schema_contract_case=probe-missing-entry-constant.*renderer_probe_manifest_schema_contract_case=validator-software-key-drift.*renderer_probe_manifest_schema_contract_case=validator-missing-manifest-constant.*renderer_probe_manifest_schema_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-route-inventory-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --tools-dir
                "${CMAKE_SOURCE_DIR}/tools/scene3d"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
        set_tests_properties(scene3d-renderer-probe-route-inventory-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_route_inventory_verified=74 fields 42 route files 21 route pairs")

        add_test(NAME scene3d-renderer-probe-route-inventory-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory_contract.py"
                --inventory-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_route_inventory.py"
                --probe-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                --tools-dir
                "${CMAKE_SOURCE_DIR}/tools/scene3d"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
        set_tests_properties(scene3d-renderer-probe-route-inventory-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_route_inventory_contract_case=valid-current-route-inventory.*renderer_probe_route_inventory_contract_case=missing-route-field-mention.*renderer_probe_route_inventory_contract_case=missing-route-ctest-registration.*renderer_probe_route_inventory_contract_case=missing-route-negative-contract.*renderer_probe_route_inventory_contract_case=missing-route-positive-smoke.*renderer_probe_route_inventory_contract_verified=true")

        add_test(NAME scene3d-native-slice-handoff-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff.py"
                --repo-root
                "${CMAKE_SOURCE_DIR}"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
                --doc-file
                "${CMAKE_SOURCE_DIR}/docs/analysis/threejs-gltf-bake-native-slice.md"
                --plan-file
                "${CMAKE_SOURCE_DIR}/planning/threejs-webgpu-gltf-bake-plan.md")
        set_tests_properties(scene3d-native-slice-handoff-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_slice_handoff_verified=20 artifacts 21 tests 15 doc tokens 1 ctest tokens 7 plan tokens 8 forbidden plan tokens 1 file tokens")

        add_test(NAME scene3d-native-slice-handoff-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff_contract.py"
                --handoff-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_slice_handoff.py"
                --repo-root
                "${CMAKE_SOURCE_DIR}"
                --ctest-file
                "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
                --doc-file
                "${CMAKE_SOURCE_DIR}/docs/analysis/threejs-gltf-bake-native-slice.md")
        set_tests_properties(scene3d-native-slice-handoff-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "native_slice_handoff_contract_case=valid-current-handoff.*native_slice_handoff_contract_case=missing-renderer-probe-artifact.*native_slice_handoff_contract_case=missing-boxtextured-ctest.*native_slice_handoff_contract_case=missing-runtime-issue-link.*native_slice_handoff_contract_case=missing-runtime-evidence-comment-link.*native_slice_handoff_contract_case=missing-runtime-evidence-ctest-link.*native_slice_handoff_contract_case=missing-clean-sidecar-runtime-evidence-link.*native_slice_handoff_contract_case=missing-plan-runtime-boundary.*native_slice_handoff_contract_case=stale-plan-live-gltf-spike.*native_slice_handoff_contract_case=stale-plan-live-exporter-implementation.*native_slice_handoff_contract_case=missing-url-gate-doc.*native_slice_handoff_contract_case=missing-final-gate-doc.*native_slice_handoff_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-final-eligibility-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py"
                --probe-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py")
                set_tests_properties(scene3d-renderer-probe-final-eligibility-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_final_eligibility_verified=true.*renderer_probe_final_eligibility_rejected=false_probe.*renderer_probe_final_eligibility_contract_verified=true")

                add_test(NAME scene3d-renderer-probe-final-eligibility-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility_contract.py"
                        --probe-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --final-eligibility-contract
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_final_eligibility.py")
                set_tests_properties(scene3d-renderer-probe-final-eligibility-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_final_eligibility_contract_case=valid-final-software-entry.*renderer_probe_final_eligibility_contract_case=interim-status-rejects-final-true.*renderer_probe_final_eligibility_contract_case=non-pixel-software-rejects-final-true.*renderer_probe_final_eligibility_contract_case=missing-entry-id-rejects-final-true.*renderer_probe_final_eligibility_contract_case=final-entry-rejects-final-false.*renderer_probe_final_eligibility_contract_verified=true")

                add_test(NAME scene3d-renderer-probe-null-backend-api
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                        --expect-code 2
                        --expect-regex "adapter_backend_type=Null.*adapter_backend_preference=null.*null_backend_requested=true.*pixel_output_produced=false.*final_software_golden_eligible=false"
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 32
                        --height 32
                        --adapter-scope dawn_null_api
                        --adapter-backend null)
                set_tests_properties(scene3d-renderer-probe-null-backend-api
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "adapter_backend_type=Null.*pixel_output_produced=false")

                add_test(NAME scene3d-renderer-probe-final-software-gate
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/expect_command.py"
                        --expect-code 3
                        --expect-regex "final_software_golden_eligible=false"
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal
                        --require-final-software-adapter)
                set_tests_properties(scene3d-renderer-probe-final-software-gate
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "final_software_golden_eligible=false")

                add_test(NAME scene3d-renderer-probe-cli-surface-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface.py"
                        --probe-tool
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --fixture
                        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
                set_tests_properties(scene3d-renderer-probe-cli-surface-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_cli_surface_case=help.*renderer_probe_cli_surface_case=invalid-scene.*renderer_probe_cli_surface_case=invalid-width.*renderer_probe_cli_surface_case=boxtextured-requires-fixture.*renderer_probe_cli_surface_case=null-backend.*renderer_probe_cli_surface_case=final-software-gate.*renderer_probe_cli_surface_case=hardcoded-output-png.*renderer_probe_cli_surface_case=boxtextured-handoff.*renderer_probe_cli_surface_verified=true")

                add_test(NAME scene3d-renderer-probe-cli-surface-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface_contract.py"
                        --cli-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe_cli_surface.py")
                set_tests_properties(scene3d-renderer-probe-cli-surface-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_cli_surface_contract_case=valid-fake-probe.*renderer_probe_cli_surface_contract_case=help-option-drift.*renderer_probe_cli_surface_contract_case=invalid-scene-exit-drift.*renderer_probe_cli_surface_contract_case=invalid-width-usage-drift.*renderer_probe_cli_surface_contract_case=boxtextured-fixture-drift.*renderer_probe_cli_surface_contract_case=null-backend-pixel-drift.*renderer_probe_cli_surface_contract_case=final-gate-exit-drift.*renderer_probe_cli_surface_contract_case=final-gate-eligibility-drift.*renderer_probe_cli_surface_contract_case=boxtextured-consumption-drift.*renderer_probe_cli_surface_contract_case=output-png-missing-file-drift.*renderer_probe_cli_surface_contract_case=output-png-magic-drift.*renderer_probe_cli_surface_contract_verified=true")

                add_test(NAME scene3d-native-renderer-link-boundary
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary.py"
                        --build-dir "${CMAKE_BINARY_DIR}")
                set_tests_properties(scene3d-native-renderer-link-boundary
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "native_renderer_boundary_verified=7 link files")

                add_test(NAME scene3d-native-renderer-link-boundary-negative-contract
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary_contract.py"
                        --boundary-verifier
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_native_renderer_boundary.py"
                        --build-dir "${CMAKE_BINARY_DIR}")
                set_tests_properties(scene3d-native-renderer-link-boundary-negative-contract
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "native_renderer_boundary_contract_case=valid-current-link-boundary.*native_renderer_boundary_contract_case=forbidden-view-link-token.*native_renderer_boundary_contract_case=forbidden-widget-link-token.*native_renderer_boundary_contract_case=missing-scene-parser-token.*native_renderer_boundary_contract_case=missing-render-webgpu-token.*native_renderer_boundary_contract_case=missing-required-link-file.*native_renderer_boundary_contract_verified=true")
            endif()
