# Renderer3D probe route, manifest, fixture, and negative test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

                add_test(NAME scene3d-renderer-probe-hardcoded-manifest
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                        --entry-id hardcoded_textured_cube
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene hardcoded
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal)
                set_tests_properties(scene3d-renderer-probe-hardcoded-manifest
                    PROPERTIES
                    PASS_REGULAR_EXPRESSION
                        "renderer_probe_verified=hardcoded_textured_cube")

                add_test(NAME scene3d-renderer-probe-boxtextured-manifest
                    COMMAND ${Python3_EXECUTABLE}
                        "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_renderer_probe.py"
                        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/renderer3d-goldens.json"
                        --entry-id official_boxtextured_fixture
                        --
                        $<TARGET_FILE:pulp-renderer3d-probe>
                        --scene boxtextured
                        --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                        --width 128
                        --height 128
                        --adapter-scope macos_default_metal)
        set_tests_properties(scene3d-renderer-probe-boxtextured-manifest
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_verified=official_boxtextured_fixture")

        add_test(NAME scene3d-renderer-probe-material-floor-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-renderer-probe-material-floor-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_material_floor_verified=boxtextured")

        add_test(NAME scene3d-renderer-probe-non-base-texture-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-non-base-texture-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_non_base_texture_route_verified=true")

        add_test(NAME scene3d-renderer-probe-non-base-texture-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_non_base_texture_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-non-base-texture-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_non_base_texture_route_contract_case=valid-current-route.*renderer_probe_non_base_texture_route_contract_case=missing-transform-applied-field.*renderer_probe_non_base_texture_route_contract_case=metallic-roughness-route-drift.*renderer_probe_non_base_texture_route_contract_case=occlusion-strength-drift.*renderer_probe_non_base_texture_route_contract_case=transform-deferral-leak.*renderer_probe_non_base_texture_route_contract_case=texcoord1-deferral-leak.*renderer_probe_non_base_texture_route_contract_case=pixel-output-drift.*renderer_probe_non_base_texture_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-derived-normal-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-derived-normal-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_derived_normal_route_verified=true")

        add_test(NAME scene3d-renderer-probe-derived-normal-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_derived_normal_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-derived-normal-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_derived_normal_route_contract_case=valid-derived-normal-route.*renderer_probe_derived_normal_route_contract_case=missing-derived-tangent-field.*renderer_probe_derived_normal_route_contract_case=normal-texture-route-drift.*renderer_probe_derived_normal_route_contract_case=normal-scale-route-drift.*renderer_probe_derived_normal_route_contract_case=normal-texture-deferral-leak.*renderer_probe_derived_normal_route_contract_case=normal-scale-deferral-leak.*renderer_probe_derived_normal_route_contract_case=pixel-output-drift.*renderer_probe_derived_normal_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-blend-sort-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-alpha-blend-sort-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_blend_sort_route_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-blend-sort-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_blend_sort_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-alpha-blend-sort-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_blend_sort_route_contract_case=valid-alpha-blend-sort-route.*renderer_probe_alpha_blend_sort_route_contract_case=primitive-count-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-blend-route-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-depth-write-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-sort-drift.*renderer_probe_alpha_blend_sort_route_contract_case=alpha-mask-leak.*renderer_probe_alpha_blend_sort_route_contract_case=pixel-output-drift.*renderer_probe_alpha_blend_sort_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-mask-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-alpha-mask-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_mask_route_verified=true")

        add_test(NAME scene3d-renderer-probe-alpha-mask-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_alpha_mask_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-alpha-mask-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_alpha_mask_route_contract_case=valid-alpha-mask-route.*renderer_probe_alpha_mask_route_contract_case=alpha-mask-route-drift.*renderer_probe_alpha_mask_route_contract_case=texture-decode-drift.*renderer_probe_alpha_mask_route_contract_case=base-color-srgb-drift.*renderer_probe_alpha_mask_route_contract_case=alpha-blend-leak.*renderer_probe_alpha_mask_route_contract_case=fallback-texture-leak.*renderer_probe_alpha_mask_route_contract_case=pixel-output-drift.*renderer_probe_alpha_mask_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-vertex-color-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-vertex-color-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_vertex_color_route_verified=true")

        add_test(NAME scene3d-renderer-probe-vertex-color-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_vertex_color_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-vertex-color-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_vertex_color_route_contract_case=valid-vertex-color-route.*renderer_probe_vertex_color_route_contract_case=vertex-color-route-drift.*renderer_probe_vertex_color_route_contract_case=fallback-texture-route-drift.*renderer_probe_vertex_color_route_contract_case=texture-decode-leak.*renderer_probe_vertex_color_route_contract_case=alpha-mask-leak.*renderer_probe_vertex_color_route_contract_case=primitive-count-drift.*renderer_probe_vertex_color_route_contract_case=pixel-output-drift.*renderer_probe_vertex_color_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-double-sided-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-double-sided-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_double_sided_route_verified=true")

        add_test(NAME scene3d-renderer-probe-double-sided-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_double_sided_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-double-sided-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_double_sided_route_contract_case=valid-double-sided-route.*renderer_probe_double_sided_route_contract_case=double-sided-route-drift.*renderer_probe_double_sided_route_contract_case=fallback-texture-route-drift.*renderer_probe_double_sided_route_contract_case=texture-decode-leak.*renderer_probe_double_sided_route_contract_case=alpha-blend-leak.*renderer_probe_double_sided_route_contract_case=primitive-count-drift.*renderer_probe_double_sided_route_contract_case=pixel-output-drift.*renderer_probe_double_sided_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-unlit-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-unlit-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unlit_route_verified=true")

        add_test(NAME scene3d-renderer-probe-unlit-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unlit_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-unlit-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unlit_route_contract_case=valid-unlit-route.*renderer_probe_unlit_route_contract_case=unlit-route-drift.*renderer_probe_unlit_route_contract_case=fallback-texture-route-drift.*renderer_probe_unlit_route_contract_case=texture-decode-leak.*renderer_probe_unlit_route_contract_case=double-sided-leak.*renderer_probe_unlit_route_contract_case=alpha-blend-leak.*renderer_probe_unlit_route_contract_case=primitive-count-drift.*renderer_probe_unlit_route_contract_case=pixel-output-drift.*renderer_probe_unlit_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-emissive-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-emissive-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_emissive_route_verified=true")

        add_test(NAME scene3d-renderer-probe-emissive-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_emissive_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-emissive-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_emissive_route_contract_case=valid-emissive-route.*renderer_probe_emissive_route_contract_case=emissive-factor-route-drift.*renderer_probe_emissive_route_contract_case=emissive-strength-route-drift.*renderer_probe_emissive_route_contract_case=fallback-texture-route-drift.*renderer_probe_emissive_route_contract_case=texture-decode-leak.*renderer_probe_emissive_route_contract_case=emissive-texture-leak.*renderer_probe_emissive_route_contract_case=unlit-leak.*renderer_probe_emissive_route_contract_case=primitive-count-drift.*renderer_probe_emissive_route_contract_case=pixel-output-drift.*renderer_probe_emissive_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-sampler-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-sampler-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_sampler_route_verified=true")

        add_test(NAME scene3d-renderer-probe-sampler-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_sampler_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-sampler-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_sampler_route_contract_case=valid-sampler-route.*renderer_probe_sampler_route_contract_case=sampler-route-drift.*renderer_probe_sampler_route_contract_case=sampler-clamp-s-drift.*renderer_probe_sampler_route_contract_case=sampler-clamp-t-drift.*renderer_probe_sampler_route_contract_case=sampler-linear-drift.*renderer_probe_sampler_route_contract_case=texture-decode-drift.*renderer_probe_sampler_route_contract_case=fallback-texture-leak.*renderer_probe_sampler_route_contract_case=primitive-count-drift.*renderer_probe_sampler_route_contract_case=pixel-output-drift.*renderer_probe_sampler_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-mipmap-sampler-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-mipmap-sampler-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_mipmap_sampler_route_verified=true")

        add_test(NAME scene3d-renderer-probe-mipmap-sampler-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_mipmap_sampler_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-mipmap-sampler-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_mipmap_sampler_route_contract_case=valid-mipmap-sampler-route.*renderer_probe_mipmap_sampler_route_contract_case=mipmap-downgrade-drift.*renderer_probe_mipmap_sampler_route_contract_case=sampler-route-drift.*renderer_probe_mipmap_sampler_route_contract_case=sampler-linear-drift.*renderer_probe_mipmap_sampler_route_contract_case=texture-decode-drift.*renderer_probe_mipmap_sampler_route_contract_case=fallback-texture-leak.*renderer_probe_mipmap_sampler_route_contract_case=primitive-count-drift.*renderer_probe_mipmap_sampler_route_contract_case=pixel-output-drift.*renderer_probe_mipmap_sampler_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-base-color-transform-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-base-color-transform-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_base_color_transform_route_verified=true")

        add_test(NAME scene3d-renderer-probe-base-color-transform-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_base_color_transform_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-base-color-transform-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_base_color_transform_route_contract_case=valid-base-color-transform-route.*renderer_probe_base_color_transform_route_contract_case=base-color-transform-drift.*renderer_probe_base_color_transform_route_contract_case=base-color-texcoord1-drift.*renderer_probe_base_color_transform_route_contract_case=texture-decode-drift.*renderer_probe_base_color_transform_route_contract_case=fallback-texture-leak.*renderer_probe_base_color_transform_route_contract_case=non-base-transform-leak.*renderer_probe_base_color_transform_route_contract_case=primitive-count-drift.*renderer_probe_base_color_transform_route_contract_case=pixel-output-drift.*renderer_probe_base_color_transform_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-resource-floor-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-resource-floor-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_resource_floor_route_verified=true")

        add_test(NAME scene3d-renderer-probe-resource-floor-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_resource_floor_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-resource-floor-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_resource_floor_route_contract_case=valid-resource-floor-route.*renderer_probe_resource_floor_route_contract_case=depth-target-drift.*renderer_probe_resource_floor_route_contract_case=color-target-drift.*renderer_probe_resource_floor_route_contract_case=vertex-buffer-drift.*renderer_probe_resource_floor_route_contract_case=index-buffer-drift.*renderer_probe_resource_floor_route_contract_case=uniform-buffer-drift.*renderer_probe_resource_floor_route_contract_case=metallic-roughness-factor-drift.*renderer_probe_resource_floor_route_contract_case=texture-decode-leak.*renderer_probe_resource_floor_route_contract_case=primitive-count-drift.*renderer_probe_resource_floor_route_contract_case=pixel-output-drift.*renderer_probe_resource_floor_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-adapter-selection-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-adapter-selection-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_adapter_selection_route_case=null-backend.*renderer_probe_adapter_selection_route_case=forced-fallback.*renderer_probe_adapter_selection_route_verified=true")

        add_test(NAME scene3d-renderer-probe-adapter-selection-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_adapter_selection_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-adapter-selection-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_adapter_selection_route_contract_case=valid-adapter-selection-route.*renderer_probe_adapter_selection_route_contract_case=null-backend-request-drift.*renderer_probe_adapter_selection_route_contract_case=null-backend-type-drift.*renderer_probe_adapter_selection_route_contract_case=null-backend-pixel-drift.*renderer_probe_adapter_selection_route_contract_case=fallback-request-drift.*renderer_probe_adapter_selection_route_contract_case=fallback-null-leak.*renderer_probe_adapter_selection_route_contract_case=fallback-command-leak.*renderer_probe_adapter_selection_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-directional-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-directional-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_directional_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-directional-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_directional_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-directional-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_directional_light_route_contract_case=valid-directional-light-route.*renderer_probe_directional_light_route_contract_case=directional-light-drift.*renderer_probe_directional_light_route_contract_case=geometry-normal-drift.*renderer_probe_directional_light_route_contract_case=directional-transform-leak.*renderer_probe_directional_light_route_contract_case=point-light-leak.*renderer_probe_directional_light_route_contract_case=spot-light-leak.*renderer_probe_directional_light_route_contract_case=primitive-count-drift.*renderer_probe_directional_light_route_contract_case=pixel-output-drift.*renderer_probe_directional_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-point-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-point-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_point_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-point-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_point_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-point-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_point_light_route_contract_case=valid-point-light-route.*renderer_probe_point_light_route_contract_case=point-light-drift.*renderer_probe_point_light_route_contract_case=point-range-drift.*renderer_probe_point_light_route_contract_case=point-deferred-leak.*renderer_probe_point_light_route_contract_case=range-deferred-leak.*renderer_probe_point_light_route_contract_case=directional-light-leak.*renderer_probe_point_light_route_contract_case=spot-light-leak.*renderer_probe_point_light_route_contract_case=primitive-count-drift.*renderer_probe_point_light_route_contract_case=pixel-output-drift.*renderer_probe_point_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-spot-light-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-spot-light-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_spot_light_route_verified=true")

        add_test(NAME scene3d-renderer-probe-spot-light-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_spot_light_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-spot-light-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_spot_light_route_contract_case=valid-spot-light-route.*renderer_probe_spot_light_route_contract_case=spot-light-drift.*renderer_probe_spot_light_route_contract_case=spot-range-drift.*renderer_probe_spot_light_route_contract_case=spot-deferred-leak.*renderer_probe_spot_light_route_contract_case=spot-cone-deferred-leak.*renderer_probe_spot_light_route_contract_case=range-deferred-leak.*renderer_probe_spot_light_route_contract_case=point-light-leak.*renderer_probe_spot_light_route_contract_case=directional-light-leak.*renderer_probe_spot_light_route_contract_case=primitive-count-drift.*renderer_probe_spot_light_route_contract_case=pixel-output-drift.*renderer_probe_spot_light_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-transform-animation-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-transform-animation-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_transform_animation_route_verified=true")

        add_test(NAME scene3d-renderer-probe-transform-animation-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_transform_animation_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-transform-animation-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_transform_animation_route_contract_case=valid-transform-animation-route.*renderer_probe_transform_animation_route_contract_case=transform-initial-pose-drift.*renderer_probe_transform_animation_route_contract_case=transform-deferred-drift.*renderer_probe_transform_animation_route_contract_case=camera-leak.*renderer_probe_transform_animation_route_contract_case=directional-light-leak.*renderer_probe_transform_animation_route_contract_case=skinning-deferred-leak.*renderer_probe_transform_animation_route_contract_case=primitive-count-drift.*renderer_probe_transform_animation_route_contract_case=pixel-output-drift.*renderer_probe_transform_animation_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-advanced-material-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-advanced-material-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_advanced_material_route_verified=true")

        add_test(NAME scene3d-renderer-probe-advanced-material-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_advanced_material_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-advanced-material-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_advanced_material_route_contract_case=valid-advanced-material-route.*renderer_probe_advanced_material_route_contract_case=advanced-material-deferral-drift.*renderer_probe_advanced_material_route_contract_case=double-sided-route-drift.*renderer_probe_advanced_material_route_contract_case=texture-decode-leak.*renderer_probe_advanced_material_route_contract_case=unlit-leak.*renderer_probe_advanced_material_route_contract_case=camera-leak.*renderer_probe_advanced_material_route_contract_case=primitive-count-drift.*renderer_probe_advanced_material_route_contract_case=pixel-output-drift.*renderer_probe_advanced_material_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-unsupported-mesh-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-unsupported-mesh-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unsupported_mesh_route_verified=true")

        add_test(NAME scene3d-renderer-probe-unsupported-mesh-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_unsupported_mesh_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-unsupported-mesh-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_unsupported_mesh_route_contract_case=valid-unsupported-mesh-route.*renderer_probe_unsupported_mesh_route_contract_case=skinning-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=morph-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=gpu-instancing-deferral-drift.*renderer_probe_unsupported_mesh_route_contract_case=animation-leak.*renderer_probe_unsupported_mesh_route_contract_case=advanced-material-leak.*renderer_probe_unsupported_mesh_route_contract_case=primitive-count-drift.*renderer_probe_unsupported_mesh_route_contract_case=pixel-output-drift.*renderer_probe_unsupported_mesh_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-perspective-camera-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-perspective-camera-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_perspective_camera_route_verified=true")

        add_test(NAME scene3d-renderer-probe-perspective-camera-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_perspective_camera_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-perspective-camera-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_perspective_camera_route_contract_case=valid-perspective-camera-route.*renderer_probe_perspective_camera_route_contract_case=perspective-camera-drift.*renderer_probe_perspective_camera_route_contract_case=camera-translation-drift.*renderer_probe_perspective_camera_route_contract_case=camera-aspect-drift.*renderer_probe_perspective_camera_route_contract_case=camera-depth-drift.*renderer_probe_perspective_camera_route_contract_case=orthographic-camera-leak.*renderer_probe_perspective_camera_route_contract_case=camera-deferred-leak.*renderer_probe_perspective_camera_route_contract_case=primitive-count-drift.*renderer_probe_perspective_camera_route_contract_case=pixel-output-drift.*renderer_probe_perspective_camera_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-orthographic-camera-route-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_smoke.py"
                --probe-tool $<TARGET_FILE:pulp-renderer3d-probe>)
        set_tests_properties(scene3d-renderer-probe-orthographic-camera-route-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_orthographic_camera_route_verified=true")

        add_test(NAME scene3d-renderer-probe-orthographic-camera-route-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_contract.py"
                --route-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_orthographic_camera_route_smoke.py")
        set_tests_properties(scene3d-renderer-probe-orthographic-camera-route-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_orthographic_camera_route_contract_case=valid-orthographic-camera-route.*renderer_probe_orthographic_camera_route_contract_case=orthographic-camera-drift.*renderer_probe_orthographic_camera_route_contract_case=perspective-camera-leak.*renderer_probe_orthographic_camera_route_contract_case=camera-depth-drift.*renderer_probe_orthographic_camera_route_contract_case=camera-deferred-leak.*renderer_probe_orthographic_camera_route_contract_case=primitive-count-drift.*renderer_probe_orthographic_camera_route_contract_case=pixel-output-drift.*renderer_probe_orthographic_camera_route_contract_verified=true")

        add_test(NAME scene3d-renderer-probe-material-floor-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_contract.py"
                --material-floor-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/renderer_probe_material_floor_smoke.py")
        set_tests_properties(scene3d-renderer-probe-material-floor-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "renderer_probe_material_floor_contract_case=valid-current-material-floor.*renderer_probe_material_floor_contract_case=missing-base-color-srgb-field.*renderer_probe_material_floor_contract_case=texture-upload-drift.*renderer_probe_material_floor_contract_case=scene-identity-drift.*renderer_probe_material_floor_contract_case=material-factor-drift.*renderer_probe_material_floor_contract_case=geometry-normal-drift.*renderer_probe_material_floor_contract_case=pixel-output-drift.*renderer_probe_material_floor_contract_verified=true")
