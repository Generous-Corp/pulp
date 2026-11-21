# Widget Gallery — headless renderer for the Ink & Signal component gallery.
# Desktop only; uses the built-in CoreGraphics screenshot provider on Apple
# (no GPU/Skia required — the gallery widgets paint via the 2D canvas, no
# file-backed images). On platforms without a screenshot provider the binary
# still builds and exits with a clear "no provider" message.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-widget-gallery main.cpp)
    if(TARGET pulp::vellum-widget-gallery)
        target_compile_definitions(pulp-widget-gallery PRIVATE PULP_WIDGET_GALLERY_VELLUM)
        target_link_libraries(pulp-widget-gallery PRIVATE pulp::vellum-widget-gallery)
    else()
        target_link_libraries(pulp-widget-gallery PRIVATE pulp::view)
    endif()
endif()
