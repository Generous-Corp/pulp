# GPU NAM — fused block-parallel WaveNet neural-amp inference on the GPU.
# Correctness + value check (GPU conv-stack vs the CPU WaveNet reference).
if(TARGET pulp::render)
    add_executable(gpu-nam-check wavenet_check.cpp)
    target_link_libraries(gpu-nam-check PRIVATE pulp::render)
    target_include_directories(gpu-nam-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
