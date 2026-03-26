# GPU Validation Demo — Modulation Matrix with Animated Cables
# Exercises the full GPU render pipeline: acquire → wrap → render → submit → present
# Only built when GPU rendering is available (PULP_HAS_SKIA)

if(PULP_ENABLE_GPU AND PULP_HAS_SKIA)
    add_executable(pulp-gpu-demo main.cpp)
    target_link_libraries(pulp-gpu-demo PRIVATE
        pulp::view
        pulp::render
        pulp::canvas
        pulp::state
    )

    if(APPLE)
        target_link_libraries(pulp-gpu-demo PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreVideo"
        )
    endif()
endif()
