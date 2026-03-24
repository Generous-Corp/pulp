add_library(pulp-canvas)
add_library(pulp::canvas ALIAS pulp-canvas)

target_include_directories(pulp-canvas
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-canvas PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-canvas: placeholder — replaced in Phase 2+\nnamespace pulp::canvas {}\n"
)

target_link_libraries(pulp-canvas PUBLIC pulp::runtime)
