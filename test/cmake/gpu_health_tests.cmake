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
