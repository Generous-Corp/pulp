add_library(pulp-state)
add_library(pulp::state ALIAS pulp-state)

target_include_directories(pulp-state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-state PRIVATE
    src/store.cpp
    src/state_migration.cpp
    src/param_json.cpp
    src/preset_manager.cpp
    src/state_tree.cpp
    src/state_tree_sync.cpp
    src/properties_file.cpp
)

target_link_libraries(pulp-state PUBLIC pulp::runtime)
target_link_libraries(pulp-state PRIVATE pulp::events)

# Keep the concrete state archive closure narrow while preserving the historical
# pulp::state consumer surface through the content implementation transitively.
target_link_libraries(pulp-state INTERFACE pulp::content)

# content_registry.hpp keeps its public pulp/state include path but now lives in
# pulp-content; state sources still need that directory while compiling.
target_include_directories(pulp-state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../content/include
)

# CHOC headers (used by store.cpp for endianness utilities)
target_include_directories(pulp-state PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz>
)

# Configure a CTest that checks the concrete archive's direct link closure.
# Compatibility consumers configure this subsystem without the repository test
# tree; only the top-level Pulp test build has the template and registers it.
if(EXISTS "${CMAKE_SOURCE_DIR}/test/cmake/pulp_state_link_closure.cmake.in")
get_target_property(_pulp_state_direct_links pulp-state LINK_LIBRARIES)
if(NOT _pulp_state_direct_links)
    set(_pulp_state_direct_links "")
endif()
string(REPLACE ";" "\\;" _pulp_state_direct_links_escaped "${_pulp_state_direct_links}")
configure_file(
    ${CMAKE_SOURCE_DIR}/test/cmake/pulp_state_link_closure.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pulp_state_link_closure.cmake
    @ONLY
)
endif()
