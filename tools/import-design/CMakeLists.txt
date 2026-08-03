# pulp-import-design — design file import tool
# Translates Figma/Stitch/v0/Pencil designs to Pulp web-compat JS.
# The versioned detect-only surface is intentionally self-contained so it can
# be linked into the test target without dragging the full pulp::view pipeline
# along.
add_library(pulp-browser-capture-backend STATIC
    browser_capture_backend.cpp
    browser_discovery.cpp
    browser_capture_diagnostics.cpp
    node_runtime.cpp)
add_library(pulp::browser-capture-backend ALIAS
    pulp-browser-capture-backend)
target_include_directories(pulp-browser-capture-backend PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(pulp-browser-capture-backend PRIVATE
    PULP_BROWSER_CAPTURE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/browser_capture/capture.mjs")
target_link_libraries(pulp-browser-capture-backend PUBLIC
    pulp::platform)

add_executable(pulp-import-design
    pulp_import_design.cpp
    import_design_cli_help.cpp
    browser_import_cli.cpp
    browser_import_session.cpp
    browser_html_import.cpp
    browser_capture_workspace.cpp
    browser_capture_ir.cpp
    browser_capture_styles.cpp
    browser_capture_tree.cpp
    browser_capture_validation.cpp
    claude_html_dependencies.cpp
    html_project_stager.cpp
    html_intake.cpp
    import_detect.cpp
    fig_lane.cpp
    envelope_merge.cpp
    sprite_skins.cpp
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
target_link_libraries(pulp-import-design PRIVATE
    pulp::browser-capture-backend
    pulp::view
    pulp::state
    pulp::platform)

# Match the SDK's bin/ + lib/ install layout so the installed helper can
# resolve shared dependencies such as wgpu-native from the same prefix.
if(APPLE)
    set_target_properties(pulp-import-design PROPERTIES
        INSTALL_RPATH "@executable_path/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE
        MACOSX_RPATH TRUE)
elseif(UNIX)
    set_target_properties(pulp-import-design PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()

file(STRINGS
    "${CMAKE_CURRENT_SOURCE_DIR}/browser_capture/runtime_manifest.txt"
    _pulp_browser_capture_runtime_names)
set(_pulp_browser_capture_runtime_files)
foreach(_runtime_name IN LISTS _pulp_browser_capture_runtime_names)
    list(APPEND _pulp_browser_capture_runtime_files
        "browser_capture/${_runtime_name}")
endforeach()
add_custom_target(pulp-browser-capture-runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:pulp-import-design>/browser_capture-v1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${_pulp_browser_capture_runtime_files}
        "$<TARGET_FILE_DIR:pulp-import-design>/browser_capture-v1"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS ${_pulp_browser_capture_runtime_files}
    COMMENT "Staging browser-capture runtime beside pulp-import-design")
add_dependencies(pulp-import-design pulp-browser-capture-runtime)

# Keep source-prefix/SDK installs equivalent to release archives: the Rust
# `pulp` frontend delegates design import to this sibling executable, which in
# turn requires the JavaScript capture runtime beside it.
install(TARGETS pulp-import-design
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin)
install(FILES ${_pulp_browser_capture_runtime_files}
    DESTINATION bin/browser_capture-v1)

add_executable(pulp-design-import-bench
    design_import_benchmark.cpp)
target_include_directories(pulp-design-import-bench PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-design-import-bench PRIVATE
    pulp::view
    pulp::state
    pulp::platform)

# Development-only observer used by the Importer Differential Lab. It reads a
# serialized DesignIR directly, avoiding codegen/QuickJS as a confounding layer
# when measuring the candidate importer's native tree.
add_executable(pulp-design-ir-observe
    design_ir_observe.cpp)
target_include_directories(pulp-design-ir-observe PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(pulp-design-ir-observe PRIVATE
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
pulp_link_fontconfig_after_skia(pulp-design-ir-observe)
pulp_link_fontconfig_after_skia(pulp-annotated-capture-import)
