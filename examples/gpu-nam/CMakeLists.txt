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

        # Fused GPU NAM forward validated against the exact CPU oracle on the real
        # .nam capture. Needs both pulp::render (GPU primitive) and pulp::runtime
        # (choc::json, used by the .nam loader / oracle).
        if(TARGET pulp::runtime)
            add_executable(gpu-nam-gpu-test test_gpu_nam_real.cpp)
            target_link_libraries(gpu-nam-gpu-test
                PRIVATE pulp::render pulp::runtime Catch2::Catch2WithMain)
            target_include_directories(gpu-nam-gpu-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            add_test(NAME gpu-nam-gpu-test COMMAND gpu-nam-gpu-test)
        endif()
    endif()
endif()

# Exact, reference-faithful NAM WaveNet CPU inference + .nam loader. This is the
# oracle the GPU primitive is validated against; it only needs choc::json (via
# pulp::runtime), not the GPU/render stack, so it builds independently of pulp::render.
if(TARGET pulp::runtime)
    add_executable(gpu-nam-model-check nam_model_check.cpp)
    target_link_libraries(gpu-nam-model-check PRIVATE pulp::runtime)
    target_include_directories(gpu-nam-model-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        add_executable(gpu-nam-model-test test_nam_model.cpp)
        target_link_libraries(gpu-nam-model-test PRIVATE pulp::runtime Catch2::Catch2WithMain)
        target_include_directories(gpu-nam-model-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME gpu-nam-model-test COMMAND gpu-nam-model-test)
    endif()
endif()
