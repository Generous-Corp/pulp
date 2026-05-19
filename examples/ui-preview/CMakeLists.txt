# UI Preview — standalone app for validating the rendering pipeline.
# Desktop-only: depends on pulp::inspect which needs the GPU stack.
# Skia is required because main.cpp's --font-probe codepath calls
# pulp::canvas::probe_font_glyph which is only declared when
# PULP_HAS_SKIA is defined. Without this gate, configurations with
# PULP_ENABLE_GPU=ON but Skia binaries missing (e.g. the SDK release
# build on a GitHub-hosted macOS image that hasn't bootstrapped Skia)
# would attempt to build ui-preview and fail at compile time. The
# #ifndef PULP_HAS_SKIA guard in main.cpp (pulp #2281) is a belt; this
# is the suspenders.
if(APPLE AND NOT PULP_IOS AND PULP_ENABLE_GPU AND PULP_HAS_SKIA AND TARGET pulp::inspect)
    add_executable(pulp-ui-preview main.cpp)
    target_link_libraries(pulp-ui-preview PRIVATE
        pulp::view
        pulp::state
        pulp::inspect
    )

    # Zero-copy benchmark mode (Slice 0 of #516 / #517). The root
    # CMakeLists.txt adds PULP_BENCHMARK as a compile definition
    # globally when the option is ON — ui-preview just needs an
    # explicit link against pulp::render so its perf-counter header
    # is on the include path.
    if(PULP_BENCHMARK)
        if(TARGET pulp-render)
            target_link_libraries(pulp-ui-preview PRIVATE pulp::render)
        endif()
    endif()
endif()
