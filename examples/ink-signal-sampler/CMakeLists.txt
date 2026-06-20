# Ink & Signal Sampler — headless renderer for the starter sampler UI built
# from the pulp::design component catalog. Desktop only; uses the
# built-in CoreGraphics screenshot provider on Apple (no GPU/Skia required — the
# widgets paint via the 2D canvas). On platforms without a screenshot provider
# the binary still builds and exits with a clear "no provider" message.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-ink-signal-sampler main.cpp)
    target_link_libraries(pulp-ink-signal-sampler PRIVATE pulp::view)
endif()
