# pulp-vellum: the parameter bridge between Pulp's editor-host contract and
# Vellum's value interfaces (ParamModel, ValueFeed). It depends on Pulp's
# format-core and on an installed Vellum SDK found through CMAKE_PREFIX_PATH
# (or Vellum_DIR); it brings no UI and fetches nothing. Core never links it.

# GLOBAL: the imported Vellum targets must resolve outside this directory too,
# where the plug-in link-floor checks walk pulp-vellum's link closure.
find_package(Vellum CONFIG REQUIRED GLOBAL)

add_library(pulp-vellum STATIC
    src/mapping.cpp
    src/param_port_model.cpp
    src/visual_port_feed.cpp)
add_library(pulp::vellum ALIAS pulp-vellum)
target_include_directories(pulp-vellum PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(pulp-vellum PUBLIC cxx_std_20)
target_link_libraries(pulp-vellum PUBLIC pulp::format-core Vellum::Value)
include("${PULP_ROOT_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake")

# pulp-vellum-host: the editor host behind vellum::view::PluginViewHost::create().
# Vellum declares create() and its factory registry, and no Vellum library
# defines them, because attaching an editor to a DAW's native parent is
# plug-in embedding glue that Pulp keeps. This target defines them, with a
# raster NSView host on macOS. Every Vellum editor package links it.
if(TARGET Vellum::MirrorView_core)
    add_library(pulp-vellum-host STATIC src/plugin_view_host.cpp)
    add_library(pulp::vellum-host ALIAS pulp-vellum-host)
    target_include_directories(pulp-vellum-host PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(pulp-vellum-host PUBLIC cxx_std_20)
    target_link_libraries(pulp-vellum-host PUBLIC Vellum::MirrorView_core)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OBJCXX_COMPILER)
        target_sources(pulp-vellum-host PRIVATE src/plugin_view_host_mac.mm)
        set_source_files_properties(src/plugin_view_host_mac.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(pulp-vellum-host PRIVATE PULP_VELLUM_HAS_MAC_HOST=1)
        target_link_libraries(pulp-vellum-host PRIVATE
            "-framework Cocoa" "-framework QuartzCore")
    endif()
endif()
