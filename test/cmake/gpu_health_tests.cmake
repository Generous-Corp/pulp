if(TARGET pulp-test-gpu-health-result
        AND TARGET pulp-test-gpu-health-read-result)
    add_test(NAME gpu-health-result COMMAND pulp-test-gpu-health-result)
    set_tests_properties(gpu-health-result PROPERTIES
        LABELS "gpu;gpu-health;contract")

    add_test(NAME gpu-health-read-result COMMAND pulp-test-gpu-health-read-result)
    set_tests_properties(gpu-health-read-result PROPERTIES
        LABELS "gpu;gpu-health;read;contract")
endif()

if(TARGET pulp-test-gpu-health-provider)
    add_test(NAME gpu-health-provider COMMAND pulp-test-gpu-health-provider)
    set_tests_properties(gpu-health-provider PROPERTIES
        LABELS "gpu;gpu-health")
    if(PULP_ENABLE_SCENE3D)
        set_tests_properties(gpu-health-provider PROPERTIES
            LABELS "gpu;gpu-health;scene3d-required")
    endif()
endif()

add_executable(pulp-test-control-gpu-health-read-executor
    test_control_gpu_health_read_executor.cpp)
target_link_libraries(pulp-test-control-gpu-health-read-executor PRIVATE
    pulp::inspect-runtime pulp::tool-gpu-health-model Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-gpu-health-read-executor
    PROPERTIES LABELS "inspect;control;gpu;read")

add_executable(pulp-test-control-gpu-health-provider
    test_control_gpu_health_provider.cpp)
target_link_libraries(pulp-test-control-gpu-health-provider PRIVATE
    pulp::inspect-ui-runtime pulp::tool-gpu-health-model Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-gpu-health-provider
    PROPERTIES LABELS "inspect;control;gpu;provider")
