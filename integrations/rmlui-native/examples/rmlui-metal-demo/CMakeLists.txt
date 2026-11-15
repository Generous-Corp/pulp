add_library(pulp-rmlui-metal-demo-ui STATIC rmlui_metal_demo_ui.mm)
set_source_files_properties(rmlui_metal_demo_ui.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_include_directories(pulp-rmlui-metal-demo-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pulp-rmlui-metal-demo-ui PUBLIC pulp::rmlui-native-metal pulp::format-core)
set_target_properties(pulp-rmlui-metal-demo-ui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PULP_EDITOR_FACTORY_FUNCTION "pulp_rmlui_metal_demo::editor_factory"
    PULP_EDITOR_FACTORY_HEADER "rmlui_metal_demo_ui.hpp")

set(_formats "")
if(PULP_HAS_CLAP)
    list(APPEND _formats CLAP)
endif()
if(PULP_HAS_AUSDK)
    list(APPEND _formats AU)
endif()
if(NOT _formats)
    return()
endif()

pulp_add_plugin(PulpRmlUiMetalDemo
    UI pulp-rmlui-metal-demo-ui
    FORMATS ${_formats}
    PLUGIN_NAME "Pulp + RmlUi + Metal"
    BUNDLE_ID "dev.pulp.rmlui.metal-demo"
    MANUFACTURER "Pulp"
    VERSION "1.0.0"
    CATEGORY Effect
    PLUGIN_CODE "PRmM"
    MANUFACTURER_CODE "PlRm")
