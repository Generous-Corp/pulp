add_library(pulp-render)
add_library(pulp::render ALIAS pulp-render)

target_include_directories(pulp-render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-render PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-render: placeholder — replaced in Phase 2+\nnamespace pulp::render {}\n"
)

target_link_libraries(pulp-render PUBLIC pulp::runtime)
