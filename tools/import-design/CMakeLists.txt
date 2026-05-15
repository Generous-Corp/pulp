# pulp-import-design — design file import tool
# Translates Figma/Stitch/v0/Pencil designs to Pulp web-compat JS.
# pulp #1031 adds the versioned detect-only surface (import_detect.cpp);
# the detector is intentionally self-contained so it can be linked into
# the test target without dragging the full pulp::view pipeline along.
add_executable(pulp-import-design
    pulp_import_design.cpp
    import_detect.cpp
    widget_promotion.cpp)
target_include_directories(pulp-import-design PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-import-design PRIVATE pulp::view pulp::state)

# Link-order fix for libskia.a → fontconfig on Linux+GNU ld (#1986
# follow-up): re-mention fontconfig AFTER pulp::view's transitive
# skia::skia so the linker resolves SkFontMgr_New_FontConfig's Fc*
# references. Caused v0.100.0 release-cli linux-x64 to fail; was
# already in place for pulp-cli.
include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake)
pulp_link_fontconfig_after_skia(pulp-import-design)
