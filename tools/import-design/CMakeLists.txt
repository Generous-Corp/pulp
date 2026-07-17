# pulp-import-design — design file import tool
# Translates Figma/Stitch/v0/Pencil designs to Pulp web-compat JS.
# The versioned detect-only surface is intentionally self-contained so it can
# be linked into the test target without dragging the full pulp::view pipeline
# along.
add_executable(pulp-import-design
    pulp_import_design.cpp
    import_detect.cpp
    fig_lane.cpp
    envelope_merge.cpp
    # miniz lets the CLI auto-unpack .pulp.zip exports from the Figma
    # plugin. Compiled here rather than pulled from
    # pulp::runtime because that target's miniz include dirs are PRIVATE
    # and pulp::view doesn't re-export them.
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tdef.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/external/miniz/miniz_zip.c)
target_include_directories(pulp-import-design PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/external/miniz)
# Lets the CLI locate the offline .fig decoder (a Node subtool) in a dev build.
# Installed builds fall back to $PULP_FIG_DECODE or an upward tools/ search.
target_compile_definitions(pulp-import-design PRIVATE
    PULP_IMPORT_DESIGN_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pulp-import-design PRIVATE pulp::view pulp::state pulp::platform)

add_executable(pulp-design-import-bench
    design_import_benchmark.cpp)
target_include_directories(pulp-design-import-bench PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-design-import-bench PRIVATE
    pulp::view
    pulp::state
    pulp::platform)

# Annotated-capture import lane: manifest parse + DesignFrameView codegen.
# Kept as a small static lib so the test target can link the parser/codegen core
# directly (no CLI process spawn) while the CLI below drives it end-to-end.
add_library(pulp-annotated-capture STATIC annotated_capture.cpp)
target_include_directories(pulp-annotated-capture PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-annotated-capture PUBLIC pulp::view)

add_executable(pulp-annotated-capture-import annotated_capture_import.cpp)
target_include_directories(pulp-annotated-capture-import PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-annotated-capture-import PRIVATE
    pulp-annotated-capture pulp::view pulp::state pulp::platform)

# svg_probe — diagnostic: render an SVG via DesignFrameView (SkSVGDOM) to PNG.
add_executable(pulp-svg-probe svg_probe.cpp)
target_include_directories(pulp-svg-probe PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-svg-probe PRIVATE pulp::view pulp::state pulp::platform)

# Link-order fix for libskia.a → fontconfig on Linux+GNU ld: re-mention
# fontconfig AFTER pulp::view's transitive skia::skia so the linker resolves
# SkFontMgr_New_FontConfig's Fc* references. Caused v0.100.0 release-cli
# linux-x64 to fail; was already in place for pulp-cli.
include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake)
pulp_link_fontconfig_after_skia(pulp-import-design)
pulp_link_fontconfig_after_skia(pulp-design-import-bench)
pulp_link_fontconfig_after_skia(pulp-annotated-capture-import)
