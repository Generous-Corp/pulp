add_library(pulp-audio)
add_library(pulp::audio ALIAS pulp-audio)

target_include_directories(pulp-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-audio PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-audio: placeholder — replaced in Phase 2+\nnamespace pulp::audio {}\n"
)

target_link_libraries(pulp-audio PUBLIC pulp::runtime)
