add_library(pulp-state)
add_library(pulp::state ALIAS pulp-state)

target_include_directories(pulp-state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-state PRIVATE
    src/store.cpp
    src/preset_manager.cpp
)

target_link_libraries(pulp-state PUBLIC pulp::runtime)

# CHOC headers (used by store.cpp for endianness utilities)
target_include_directories(pulp-state PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)
