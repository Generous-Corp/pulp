# Renderer3D core test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

        set(_pulp_renderer3d_test_libs pulp::render pulp::scene)
        set(_pulp_renderer3d_test_definitions PULP_TEST_SCENE3D_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scene3d")
        if(PULP_HAS_DRACO)
            list(APPEND _pulp_renderer3d_test_libs draco::draco)
            list(APPEND _pulp_renderer3d_test_definitions PULP_TEST_HAS_DRACO=1)
            list(APPEND _pulp_renderer3d_test_includes "${draco_SOURCE_DIR}/src" "${draco_BINARY_DIR}" "${CMAKE_BINARY_DIR}")
        endif()
        pulp_add_test_suite(pulp-test-renderer3d
            SOURCES test_renderer3d_loading.cpp test_renderer3d_scene.cpp test_renderer3d_materials.cpp
            LIBRARIES ${_pulp_renderer3d_test_libs}
            COMPILE_DEFINITIONS ${_pulp_renderer3d_test_definitions}
            INCLUDE_DIRS ${_pulp_renderer3d_test_includes}
            DISCOVERY_ARGS ${PULP_GPU_TEST_DISCOVERY_ARGS})
