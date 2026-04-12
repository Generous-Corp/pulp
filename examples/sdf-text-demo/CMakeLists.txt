# SDF Text Demo — Resolution-Independent Text with Bloom Post-Effect
# Validates the SDF glyph atlas pipeline end-to-end across all GPU platforms.
# Only built when GPU rendering is available.

if(PULP_ENABLE_GPU AND PULP_HAS_SKIA)
    add_executable(pulp-sdf-text-demo main.cpp)
    target_link_libraries(pulp-sdf-text-demo PRIVATE
        pulp::view
        pulp::render
        pulp::canvas
        pulp::state
    )

    if(APPLE)
        target_link_libraries(pulp-sdf-text-demo PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreVideo"
        )
    endif()
endif()
