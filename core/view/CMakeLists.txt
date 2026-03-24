add_library(pulp-view)
add_library(pulp::view ALIAS pulp-view)

target_include_directories(pulp-view
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-view PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-view: placeholder — replaced in Phase 2+\nnamespace pulp::view {}\n"
)

target_link_libraries(pulp-view PUBLIC pulp::canvas pulp::events)
