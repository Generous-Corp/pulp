add_library(pulp-midi)
add_library(pulp::midi ALIAS pulp-midi)

target_include_directories(pulp-midi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-midi PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-midi: placeholder — replaced in Phase 2+\nnamespace pulp::midi {}\n"
)

target_link_libraries(pulp-midi PUBLIC pulp::runtime)
