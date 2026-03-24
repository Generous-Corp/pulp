add_library(pulp-inspect)
add_library(pulp::inspect ALIAS pulp-inspect)

target_include_directories(pulp-inspect
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-inspect PRIVATE
    src/placeholder.cpp
)

file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-inspect: placeholder — replaced in Phase 6\nnamespace pulp::inspect {}\n"
)

target_link_libraries(pulp-inspect PUBLIC pulp::view pulp::render)
