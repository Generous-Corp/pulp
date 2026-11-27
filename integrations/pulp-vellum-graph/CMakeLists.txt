# pulp-vellum-graph: the node-graph editor for Pulp's SignalGraph, as a Vellum
# view, plus the hosted plug-in editor attachment to a Vellum window host. The graph model (pulp::host::SignalGraph) stays in Pulp core; this
# package is the one place the plug-in host stack and Vellum's view layer meet,
# so an ordinary Vellum editor never pulls in plug-in hosting. Core never links
# it. Not built on iOS, where pulp::host does not exist.
find_package(Vellum CONFIG REQUIRED)

if(NOT TARGET pulp::host)
    message(FATAL_ERROR
        "PULP_ENABLE_PULP_VELLUM_GRAPH=ON needs pulp::host, which is not built on this platform")
endif()

add_library(pulp-vellum-graph STATIC src/graph_editor_view.cpp)
add_library(pulp::vellum-graph ALIAS pulp-vellum-graph)
target_include_directories(pulp-vellum-graph PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-graph PUBLIC cxx_std_20)
target_link_libraries(pulp-vellum-graph PUBLIC pulp::host Vellum::MirrorView_core)
