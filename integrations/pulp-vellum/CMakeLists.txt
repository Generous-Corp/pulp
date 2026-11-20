# pulp-vellum: the parameter bridge between Pulp's editor-host contract and
# Vellum's value interfaces (ParamModel, ValueFeed). It depends on Pulp's
# format-core and on an installed Vellum SDK found through CMAKE_PREFIX_PATH
# (or Vellum_DIR); it brings no UI and fetches nothing. Core never links it.

find_package(Vellum CONFIG REQUIRED)

add_library(pulp-vellum STATIC
    src/mapping.cpp
    src/param_port_model.cpp
    src/visual_port_feed.cpp)
add_library(pulp::vellum ALIAS pulp-vellum)
target_include_directories(pulp-vellum PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum PUBLIC cxx_std_20)
target_link_libraries(pulp-vellum PUBLIC pulp::format-core Vellum::Value)
include("${PULP_ROOT_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake")

# The About view and Swift motion bridge require a Vellum SDK with its view layer.
if(TARGET Vellum::MirrorView_core)
    target_sources(pulp-vellum PRIVATE src/about_notices.cpp src/swift_motion_bridge.cpp)
    set_property(TARGET pulp-vellum PROPERTY PULP_VELLUM_HAS_SWIFT_MOTION TRUE)
    target_include_directories(pulp-vellum PRIVATE ${PULP_ROOT_DIR}/apple/Sources/PulpSwift)
    target_link_libraries(pulp-vellum PUBLIC Vellum::MirrorView_core)
endif()
