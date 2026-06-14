# Ink & Signal Showcase — live GPU window of every design-system primitive.
# Desktop only. Uses the GPU window host (MacGpuWindowHost) when built with
# Skia (-DSKIA_DIR=<skia-build>); falls back to the CPU host otherwise.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-ink-signal-showcase main.cpp)
    target_link_libraries(pulp-ink-signal-showcase PRIVATE pulp::view)
endif()
