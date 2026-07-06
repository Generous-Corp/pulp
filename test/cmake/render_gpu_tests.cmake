# GPU, renderer, probe, and render-loop test registrations.
# Included by test/CMakeLists.txt; keep this file as shared setup plus focused render/GPU includes only.

if(PULP_ENABLE_GPU)
    set(PULP_GPU_TEST_DISCOVERY_ARGS)
    if(WIN32 AND TARGET webgpu)
        list(APPEND PULP_GPU_TEST_DISCOVERY_ARGS
            DISCOVERY_MODE PRE_TEST
            DL_PATHS "$<TARGET_FILE_DIR:webgpu>"
        )
    endif()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/render_gpu_surface_tests.cmake")

    if(PULP_ENABLE_SCENE3D)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/renderer3d_core_tests.cmake")
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/renderer3d_probe_tests.cmake")
    endif()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/render_gpu_compute_pool_tests.cmake")
endif()
