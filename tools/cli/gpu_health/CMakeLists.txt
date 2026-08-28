add_library(pulp-tool-gpu-health-model STATIC
    src/health_result_json.cpp
    src/health_read_result_json.cpp)
add_library(pulp::tool-gpu-health-model ALIAS pulp-tool-gpu-health-model)
target_compile_features(pulp-tool-gpu-health-model PUBLIC cxx_std_20)
target_include_directories(pulp-tool-gpu-health-model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>)

if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS AND PROJECT_IS_TOP_LEVEL)
    add_library(pulp-tool-gpu-health-probes STATIC
        src/gpu_health.cpp)
    add_library(pulp::tool-gpu-health-probes ALIAS pulp-tool-gpu-health-probes)
    target_compile_features(pulp-tool-gpu-health-probes PUBLIC cxx_std_20)
    target_include_directories(pulp-tool-gpu-health-probes
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(pulp-tool-gpu-health-probes
        PUBLIC
            pulp::tool-gpu-health-model
        PRIVATE
            pulp::render)
    if(PULP_ENABLE_SCENE3D)
        target_link_libraries(pulp-tool-gpu-health-probes PRIVATE pulp::scene)

        add_executable(pulp-gpu-health-scene3d-acceptance EXCLUDE_FROM_ALL
            ${CMAKE_SOURCE_DIR}/test/gpu_health_scene3d_acceptance.cpp)
        target_link_libraries(pulp-gpu-health-scene3d-acceptance PRIVATE
            pulp::tool-gpu-health-probes)
    endif()
endif()

if(PULP_BUILD_TESTS AND NOT ANDROID AND NOT IOS AND PROJECT_IS_TOP_LEVEL)
    add_executable(pulp-test-gpu-health-result
        ${CMAKE_SOURCE_DIR}/test/test_gpu_health_result.cpp)
    target_link_libraries(pulp-test-gpu-health-result PRIVATE
        pulp::tool-gpu-health-model Catch2::Catch2WithMain)

    add_executable(pulp-test-gpu-health-read-result
        ${CMAKE_SOURCE_DIR}/test/test_gpu_health_read_result.cpp)
    target_link_libraries(pulp-test-gpu-health-read-result PRIVATE
        pulp::tool-gpu-health-model Catch2::Catch2WithMain)

    if(PULP_ENABLE_GPU)
        add_executable(pulp-test-gpu-health-provider
            ${CMAKE_SOURCE_DIR}/test/test_gpu_health_provider.cpp)
        target_link_libraries(pulp-test-gpu-health-provider PRIVATE
            pulp::tool-gpu-health-probes Catch2::Catch2WithMain)
        if(PULP_ENABLE_SCENE3D)
            target_compile_definitions(pulp-test-gpu-health-provider PRIVATE
                PULP_GPU_HEALTH_REQUIRE_RENDERER3D=1)
        endif()
    endif()
endif()
