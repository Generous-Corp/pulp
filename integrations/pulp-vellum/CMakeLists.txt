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
include("${PULP_ROOT_DIR}/integrations/pulp-vellum/cmake/PulpRmlUiEditorPackage.cmake")

# Vellum owns PluginViewHost::create(), its factory registry, and the GPU
# editor host. Pulp must not provide a competing static definition: the linker
# could silently select the raster implementation. Fail closed for old SDKs.
if(NOT TARGET Vellum::MirrorView_core)
    message(FATAL_ERROR
        "pulp-vellum requires a Vellum SDK exporting Vellum::MirrorView_core "
        "and vellum::view::PluginViewHost::create(); the SDK is too old")
endif()
