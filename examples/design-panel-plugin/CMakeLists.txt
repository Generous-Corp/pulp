# A plugin whose editor is an imported design, built for every format.
#
# Point PULP_DESIGN_PANEL_IR at any DesignIR JSON to build that panel into the
# plugin. The IR is EMBEDDED rather than read from disk at runtime, because a
# takeaway plugin has no importer around it and a path that resolves during the
# build is not a path that resolves on another machine.

set(PULP_DESIGN_PANEL_IR "${CMAKE_CURRENT_SOURCE_DIR}/panel.design.pulp.json"
    CACHE FILEPATH "DesignIR JSON to embed in the design-panel plugin")

if(NOT PULP_HAS_SKIA OR NOT EXISTS "${PULP_DESIGN_PANEL_IR}")
    # No Skia means no native design render; no IR means nothing to draw. Both
    # are ordinary configurations, so this stays a skip rather than an error.
    return()
endif()

# Embed the IR as a raw string literal. The delimiter matches Forge's exporter
# so a JSON body containing `)"` cannot terminate it early.
file(READ "${PULP_DESIGN_PANEL_IR}" _design_panel_ir)
set(_design_panel_header
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_design_ir.hpp")
file(WRITE "${_design_panel_header}"
"#pragma once\n"
"// GENERATED from ${PULP_DESIGN_PANEL_IR} — do not edit.\n"
"namespace pulp::examples {\n"
"inline constexpr const char* kEmbeddedDesignIr = R\"forgeir(\n"
"${_design_panel_ir}\n"
")forgeir\";\n"
"}  // namespace pulp::examples\n")

set(PULP_DESIGN_PANEL_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND PULP_DESIGN_PANEL_FORMATS AU AUv3)
endif()

pulp_add_plugin(PulpDesignPanel
    FORMATS         ${PULP_DESIGN_PANEL_FORMATS}
    PLUGIN_NAME     "PulpDesignPanel"
    BUNDLE_ID       "com.pulp.design-panel"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PDpl"
    MANUFACTURER_CODE "Pulp"
)

# AUv3 fans out into several targets; some are utility targets that cannot
# carry include dirs, so the type is checked rather than assumed.
foreach(_fmt IN ITEMS VST3 CLAP Standalone AU AUv3 AUv3Framework AUv3Host)
    if(TARGET PulpDesignPanel_${_fmt})
        get_target_property(_kind PulpDesignPanel_${_fmt} TYPE)
    endif()
    if(TARGET PulpDesignPanel_${_fmt} AND NOT _kind STREQUAL "UTILITY")
        target_include_directories(PulpDesignPanel_${_fmt} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(PulpDesignPanel_${_fmt} PRIVATE pulp::view)
    endif()
endforeach()

# The standalone renders its editor with --screenshot and exits before any
# audio device is opened, so a headless check is never audible.
if(TARGET PulpDesignPanel_Standalone)
    target_sources(PulpDesignPanel_Standalone PRIVATE main.cpp)
endif()

# ── The editor, rendered headlessly ───────────────────────────────────────
# Subject is Processor::create_view() — the same call every format adapter
# makes — so the PNG is what the plugin's editor draws, not what the importer
# would have drawn.
add_executable(pulp-design-panel-render-editor render_editor.cpp)
target_include_directories(pulp-design-panel-render-editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pulp-design-panel-render-editor PRIVATE
    pulp::format pulp::view)
target_compile_features(pulp-design-panel-render-editor PRIVATE cxx_std_20)

# The same editor, obtained the way the adapters obtain it. A regression
# BETWEEN the adapter and create_view() fails here and passes above.
add_executable(pulp-design-panel-render-adapter-editor render_adapter_editor.cpp)
target_include_directories(pulp-design-panel-render-adapter-editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pulp-design-panel-render-adapter-editor PRIVATE
    pulp::format pulp::view pulp::state)
target_compile_features(pulp-design-panel-render-adapter-editor PRIVATE cxx_std_20)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpDesignPanel
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpDesignPanel.clap/Contents/MacOS/PulpDesignPanel'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpDesignPanel PROPERTIES
        LABELS "validation;clap" TIMEOUT 20)
endif()
