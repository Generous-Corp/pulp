add_library(pulp-runtime)
add_library(pulp::runtime ALIAS pulp-runtime)

target_include_directories(pulp-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-runtime PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-runtime: placeholder — replaced in Phase 2+\nnamespace pulp::runtime {}\n"
)

target_link_libraries(pulp-runtime PUBLIC pulp::platform)
