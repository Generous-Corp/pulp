add_library(pulp-content
    src/content_registry.cpp
)
add_library(pulp::content ALIAS pulp-content)

target_include_directories(pulp-content
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../external/miniz
        ${choc_SOURCE_DIR}
)

# PUBLIC pulp::state: content_registry.hpp includes PresetManager and the
# capability-manifest header, whose parser lives in pulp-state, so every
# consumer of this header needs the state archive on its link line after this one.
target_link_libraries(pulp-content
    PUBLIC pulp::runtime pulp::state
    PRIVATE pulp::sample-bank-manifest
)
