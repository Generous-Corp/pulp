add_library(pulp-signal)
add_library(pulp::signal ALIAS pulp-signal)

target_include_directories(pulp-signal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-signal PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-signal: placeholder — replaced in Phase 2+\nnamespace pulp::signal {}\n"
)

target_link_libraries(pulp-signal PUBLIC pulp::runtime)
