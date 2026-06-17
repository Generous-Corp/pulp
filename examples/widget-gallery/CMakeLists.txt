# Widget Gallery — headless renderer for the Ink & Signal component gallery.
# Desktop only; uses the built-in CoreGraphics screenshot provider on Apple
# (no GPU/Skia required — the gallery widgets paint via the 2D canvas, no
# file-backed images). On platforms without a screenshot provider the binary
# still builds and exits with a clear "no provider" message.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-widget-gallery main.cpp)
    target_link_libraries(pulp-widget-gallery PRIVATE pulp::view)
endif()
