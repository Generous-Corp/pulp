add_library(pulp-format)
add_library(pulp::format ALIAS pulp-format)

target_include_directories(pulp-format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-format PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-format: placeholder — replaced in Phase 2+\nnamespace pulp::format {}\n"
)

target_link_libraries(pulp-format PUBLIC pulp::audio pulp::midi pulp::state)
