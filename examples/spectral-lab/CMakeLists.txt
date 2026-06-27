# Spectral Lab — N-layer GPU spectral "cloud" (freeze + morph) example.
# The example proper (processor / UI / formats) is added below; the bench and
# tests build only when the GPU compute lib is present.

if(TARGET pulp::gpu-audio AND TARGET pulp::render)
    add_executable(spectral-lab-bench bench.cpp)
    target_link_libraries(spectral-lab-bench
        PRIVATE pulp::gpu-audio pulp::render pulp::signal)
    target_include_directories(spectral-lab-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET Catch2::Catch2WithMain)
        add_executable(spectral-lab-test test_spectral_lab.cpp)
        target_link_libraries(spectral-lab-test
            PRIVATE pulp::gpu-audio pulp::render pulp::signal Catch2::Catch2WithMain)
        target_include_directories(spectral-lab-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME spectral-lab-test COMMAND spectral-lab-test)
    endif()
endif()
