# Renderer3D probe test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

        if(PULP_HAS_SKIA AND TARGET pulp-renderer3d-probe)
            add_test(NAME scene3d-renderer-probe-hardcoded
                COMMAND $<TARGET_FILE:pulp-renderer3d-probe>
                    --scene hardcoded
                    --width 128
                    --height 128
                    --adapter-scope macos_default_metal)
            set_tests_properties(scene3d-renderer-probe-hardcoded PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "scene=hardcoded.*success=true.*readback_completed=true.*rgba_fingerprint=15925498132503966243.*final_software_golden_eligible=false")

            add_test(NAME scene3d-renderer-probe-boxtextured
                COMMAND $<TARGET_FILE:pulp-renderer3d-probe>
                    --scene boxtextured
                    --fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d/BoxTextured/BoxTextured.glb"
                    --width 128
                    --height 128
                    --adapter-scope macos_default_metal)
            set_tests_properties(scene3d-renderer-probe-boxtextured PROPERTIES
                PASS_REGULAR_EXPRESSION
                    "scene=boxtextured.*success=true.*scene_data_consumed=true.*readback_completed=true.*rgba_fingerprint=5845745157752120258.*final_software_golden_eligible=false")

    if(Python3_Interpreter_FOUND)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/renderer3d_probe_route_tests.cmake")
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/renderer3d_probe_contract_tests.cmake")
    endif()
endif()
