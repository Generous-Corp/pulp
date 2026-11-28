cmake_minimum_required(VERSION 3.24)
project(LinkClosureGraphProject LANGUAGES CXX)

# A small project built with the real graph writer. pulp-canvas is the forbidden
# layer; every root below except clean_root reaches it by a route an earlier
# writer or checker could not see.
set(_empty "${CMAKE_CURRENT_SOURCE_DIR}/empty.cpp")
include("${PULP_TEST_DIR}/cmake/link_closure_graph.cmake")

add_library(pulp-canvas STATIC "${_empty}")
add_library(pulp::canvas ALIAS pulp-canvas)
add_library(pulp-clean STATIC "${_empty}")

# 1. an intermediate whose name does not start with pulp-
add_library(gpu_helper STATIC "${_empty}")
target_link_libraries(gpu_helper PRIVATE pulp::canvas)
add_library(route_name STATIC "${_empty}")
target_link_libraries(route_name PRIVATE gpu_helper)

# 2. a raw archive path
add_library(route_path INTERFACE)
target_link_libraries(route_path INTERFACE "/build/core/canvas/libpulp-canvas.a")

# 3. INTERFACE_SOURCES $<TARGET_OBJECTS:...>
add_library(canvas_objs OBJECT "${_empty}")
target_link_libraries(canvas_objs PRIVATE pulp::canvas)
add_library(route_objects INTERFACE)
target_sources(route_objects INTERFACE $<TARGET_OBJECTS:canvas_objs>)

# 4. an imported target
add_library(Vendor::Device INTERFACE IMPORTED)
set_property(TARGET Vendor::Device PROPERTY INTERFACE_LINK_LIBRARIES pulp::canvas)
add_library(route_imported INTERFACE)
target_link_libraries(route_imported INTERFACE Vendor::Device)

# control: the same shapes with the forbidden layer removed
add_library(clean_helper STATIC "${_empty}")
target_link_libraries(clean_helper PRIVATE pulp-clean)
add_library(clean_root INTERFACE)
target_link_libraries(clean_root INTERFACE clean_helper)
