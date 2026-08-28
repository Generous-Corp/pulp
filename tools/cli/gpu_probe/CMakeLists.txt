add_library(pulp-tool-gpu-probe-model STATIC
    src/probe_result.cpp)
add_library(pulp::tool-gpu-probe-model ALIAS pulp-tool-gpu-probe-model)
target_compile_features(pulp-tool-gpu-probe-model PUBLIC cxx_std_20)
target_include_directories(pulp-tool-gpu-probe-model
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(PULP_BUILD_TESTS)
    add_executable(pulp-test-gpu-probe-result
        ${CMAKE_SOURCE_DIR}/test/test_gpu_probe_result.cpp)
    target_link_libraries(pulp-test-gpu-probe-result PRIVATE
        pulp::tool-gpu-probe-model Catch2::Catch2WithMain)
    add_test(NAME gpu-probe-result COMMAND pulp-test-gpu-probe-result)
endif()
