# Scene3D inspect CLI test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

    add_executable(pulp-test-scene3d test_scene3d.cpp)
    target_link_libraries(pulp-test-scene3d PRIVATE
        pulp::scene Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-scene3d PRIVATE
        PULP_TEST_SCENE3D_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d")
    catch_discover_tests(pulp-test-scene3d)

    pulp_add_test_suite(pulp-test-scene3d-renderer-characterization SOURCES test_scene3d_renderer_characterization.cpp LIBRARIES pulp::scene)

    add_test(NAME scene3d-inspect-boxtextured-stats
        COMMAND $<TARGET_FILE:pulp-scene3d-inspect>
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set_tests_properties(scene3d-inspect-boxtextured-stats PROPERTIES
        PASS_REGULAR_EXPRESSION
            "meshes=1.*vertices=24.*indices=36.*textures=1")

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-scene-stats-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_stats.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_stats.cpp")
        set_tests_properties(scene3d-scene-stats-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_stats_surface_verified=true")

        add_test(NAME scene3d-scene-stats-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene_stats_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/scene_stats.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/scene_stats.cpp")
        set_tests_properties(scene3d-scene-stats-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene_stats_surface_contract_case=valid-current-stats.*scene_stats_surface_contract_case=missing-field.*scene_stats_surface_contract_case=field-order-drift.*scene_stats_surface_contract_case=text-key-drift.*scene_stats_surface_contract_case=primitive-count-drift.*scene_stats_surface_contract_case=vertex-count-drift.*scene_stats_surface_contract_case=texture-byte-drift.*scene_stats_surface_contract_case=advanced-extension-drift.*scene_stats_surface_contract_case=error-diagnostic-drift.*scene_stats_surface_contract_verified=true")
    endif()

    add_test(NAME scene3d-inspect-boxtextured-render-packet
        COMMAND $<TARGET_FILE:pulp-scene3d-inspect>
            --render-packet
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
    set_tests_properties(scene3d-inspect-boxtextured-render-packet PROPERTIES
        PASS_REGULAR_EXPRESSION
            "render_packet transformed_nodes=2 primitives=1.*primitive node=1 mesh=0 primitive=0 material=0.*features=normals,texcoord0,indexed,base_color_texture")

    if(Python3_Interpreter_FOUND AND TARGET pulp-scene3d-inspect-native)
        add_test(NAME scene3d-inspect-native-draco-diagnostic
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect-native>)
        set_tests_properties(scene3d-inspect-native-draco-diagnostic PROPERTIES
            PASS_REGULAR_EXPRESSION
                "draco_inspect_default_not_wired=true.*(native_draco_decoder_available=false.*gltf.draco_unavailable|native_draco_decoder_available=true.*gltf.draco_decode_failed).*draco_inspect_native_callback=true")
        add_test(NAME scene3d-inspect-native-draco-diagnostic-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke_contract.py"
                --smoke "${CMAKE_SOURCE_DIR}/tools/scene3d/draco_inspect_smoke.py")
        set_tests_properties(scene3d-inspect-native-draco-diagnostic-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "draco_inspect_smoke_contract_case=valid-native-disabled.*draco_inspect_smoke_contract_case=valid-native-enabled.*draco_inspect_smoke_contract_case=default-exit-drift.*draco_inspect_smoke_contract_case=default-diagnostic-drift.*draco_inspect_smoke_contract_case=native-exit-drift.*draco_inspect_smoke_contract_case=native-missing-availability.*draco_inspect_smoke_contract_case=native-ambiguous-availability.*draco_inspect_smoke_contract_case=native-disabled-diagnostic-drift.*draco_inspect_smoke_contract_case=native-enabled-diagnostic-drift.*draco_inspect_smoke_contract_case=native-not-wired-leak.*draco_inspect_smoke_contract_verified=true")
    endif()

    if(Python3_Interpreter_FOUND)
        add_test(NAME scene3d-inspect-boxtextured-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb")
        set_tests_properties(scene3d-inspect-boxtextured-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_inspect_verified=boxtextured")

        add_test(NAME scene3d-inspect-verifier-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect_contract.py"
                --inspect-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_scene3d_inspect.py")
        set_tests_properties(scene3d-inspect-verifier-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_inspect_contract_case=valid-fake-inspect.*scene3d_inspect_contract_case=missing-render-packet.*scene3d_inspect_contract_case=duplicate-primitive.*scene3d_inspect_contract_case=stats-drift.*scene3d_inspect_contract_case=primitive-feature-drift.*scene3d_inspect_contract_verified=true")

        add_test(NAME scene3d-render-packet-material-key-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-material-key-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_material_key_verified=true")
        add_test(NAME scene3d-render-packet-material-key-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_contract.py"
                --material-key-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_material_key_smoke.py")
        set_tests_properties(scene3d-render-packet-material-key-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_material_key_contract_case=valid-fake-inspect.*render_packet_material_key_contract_case=inspect-exit-drift.*render_packet_material_key_contract_case=stats-texture-count-drift.*render_packet_material_key_contract_case=stats-texture-bytes-drift.*render_packet_material_key_contract_case=stats-extra-key.*render_packet_material_key_contract_case=missing-render-packet.*render_packet_material_key_contract_case=packet-has-errors-drift.*render_packet_material_key_contract_case=missing-primitive.*render_packet_material_key_contract_case=duplicate-primitive.*render_packet_material_key_contract_case=feature-mask-drift.*render_packet_material_key_contract_case=feature-name-missing.*render_packet_material_key_contract_case=feature-name-order-drift.*render_packet_material_key_contract_case=material-index-drift.*render_packet_material_key_contract_case=world-transform-drift.*render_packet_material_key_contract_verified=true")

        add_test(NAME scene3d-material-feature-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/material_key.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/material_key.cpp")
        set_tests_properties(scene3d-material-feature-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "material_feature_surface_verified=true")

        add_test(NAME scene3d-material-feature-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_material_feature_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/material_key.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/material_key.cpp")
        set_tests_properties(scene3d-material-feature-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "material_feature_surface_contract_case=valid-current-material-feature.*material_feature_surface_contract_case=bit-assignment-drift.*material_feature_surface_contract_case=missing-feature.*material_feature_surface_contract_case=feature-order-drift.*material_feature_surface_contract_case=feature-name-drift.*material_feature_surface_contract_case=missing-feature-name.*material_feature_surface_contract_case=duplicate-feature-name.*material_feature_surface_contract_verified=true")

        add_test(NAME scene3d-render-packet-graph-error-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-graph-error-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_graph_error_verified=true")
        add_test(NAME scene3d-render-packet-graph-error-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_contract.py"
                --graph-error-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_graph_error_smoke.py")
        set_tests_properties(scene3d-render-packet-graph-error-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_graph_error_contract_case=valid-fake-inspect.*render_packet_graph_error_contract_case=inspect-exit-drift.*render_packet_graph_error_contract_case=stats-diagnostics-drift.*render_packet_graph_error_contract_case=stats-error-diagnostics-drift.*render_packet_graph_error_contract_case=missing-render-packet.*render_packet_graph_error_contract_case=packet-primitive-count-drift.*render_packet_graph_error_contract_case=packet-diagnostic-count-drift.*render_packet_graph_error_contract_case=packet-has-errors-drift.*render_packet_graph_error_contract_case=missing-diagnostic.*render_packet_graph_error_contract_case=diagnostic-severity-drift.*render_packet_graph_error_contract_case=diagnostic-code-drift.*render_packet_graph_error_contract_case=primitive-leak-after-error.*render_packet_graph_error_contract_verified=true")

        add_test(NAME scene3d-render-packet-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/render_packet.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/render_packet.cpp"
                --inspect-source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_inspect.cpp")
        set_tests_properties(scene3d-render-packet-surface-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_surface_verified=true")

        add_test(NAME scene3d-render-packet-surface-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface_contract.py"
                --surface-verifier
                "${CMAKE_SOURCE_DIR}/tools/scene3d/verify_render_packet_surface.py"
                --header "${CMAKE_SOURCE_DIR}/core/scene/include/pulp/scene/render_packet.hpp"
                --source "${CMAKE_SOURCE_DIR}/core/scene/src/render_packet.cpp"
                --inspect-source "${CMAKE_SOURCE_DIR}/core/scene/src/scene3d_inspect.cpp")
        set_tests_properties(scene3d-render-packet-surface-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_surface_contract_case=valid-current-render-packet.*render_packet_surface_contract_case=missing-primitive-field.*render_packet_surface_contract_case=packet-field-order-drift.*render_packet_surface_contract_case=has-errors-delegation-drift.*render_packet_surface_contract_case=validation-flow-drift.*render_packet_surface_contract_case=empty-packet-diagnostic-drift.*render_packet_surface_contract_case=cli-feature-mask-key-drift.*render_packet_surface_contract_case=cli-feature-name-handoff-drift.*render_packet_surface_contract_verified=true")

        add_test(NAME scene3d-render-packet-matrix-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_smoke.py"
                --inspect-tool $<TARGET_FILE:pulp-scene3d-inspect>)
        set_tests_properties(scene3d-render-packet-matrix-contract PROPERTIES
            PASS_REGULAR_EXPRESSION
                "scene3d_render_packet_matrix_verified=true")
        add_test(NAME scene3d-render-packet-matrix-negative-contract
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_contract.py"
                --matrix-smoke
                "${CMAKE_SOURCE_DIR}/tools/scene3d/render_packet_matrix_smoke.py")
        set_tests_properties(scene3d-render-packet-matrix-negative-contract
            PROPERTIES
            PASS_REGULAR_EXPRESSION
                "render_packet_matrix_contract_case=valid-fake-inspect.*render_packet_matrix_contract_case=inspect-exit-drift.*render_packet_matrix_contract_case=stats-count-drift.*render_packet_matrix_contract_case=stats-extra-key.*render_packet_matrix_contract_case=missing-render-packet.*render_packet_matrix_contract_case=packet-has-errors-drift.*render_packet_matrix_contract_case=missing-primitive.*render_packet_matrix_contract_case=duplicate-primitive.*render_packet_matrix_contract_case=world-transform-drift.*render_packet_matrix_contract_case=feature-mask-drift.*render_packet_matrix_contract_case=feature-name-drift.*render_packet_matrix_contract_case=material-fallback-drift.*render_packet_matrix_contract_verified=true")
    endif()
