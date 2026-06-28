# GPU NAM — fused block-parallel WaveNet neural-amp inference on the GPU.
# Correctness + value check (GPU conv-stack vs the CPU WaveNet reference).
if(TARGET pulp::render)
    add_executable(gpu-nam-check wavenet_check.cpp)
    target_link_libraries(gpu-nam-check PRIVATE pulp::render)
    target_include_directories(gpu-nam-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        add_executable(gpu-nam-test test_gpu_nam.cpp)
        target_link_libraries(gpu-nam-test PRIVATE pulp::render Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME gpu-nam-test COMMAND gpu-nam-test)
    endif()
endif()
