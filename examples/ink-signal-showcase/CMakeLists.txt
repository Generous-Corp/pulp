# Ink & Signal Showcase — live GPU window of every design-system primitive.
# Desktop only. Uses the GPU window host (MacGpuWindowHost) when built with
# Skia (-DSKIA_DIR=<skia-build>); falls back to the CPU host otherwise.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-ink-signal-showcase main.cpp)
    target_link_libraries(pulp-ink-signal-showcase PRIVATE pulp::view)
    # Faithful-Overview SVGs (overview-{dark,light}.svg) are loaded at runtime
    # from the source assets dir so the showcase mirrors the Figma Overview 1:1.
    target_compile_definitions(pulp-ink-signal-showcase PRIVATE
        SHOWCASE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()
