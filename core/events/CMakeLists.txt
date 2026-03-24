add_library(pulp-events)
add_library(pulp::events ALIAS pulp-events)

target_include_directories(pulp-events
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-events PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-events: placeholder — replaced in Phase 2+\nnamespace pulp::events {}\n"
)

target_link_libraries(pulp-events PUBLIC pulp::runtime)
