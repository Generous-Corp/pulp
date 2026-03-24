add_library(pulp-platform)
add_library(pulp::platform ALIAS pulp-platform)

target_include_directories(pulp-platform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-platform PRIVATE
    src/placeholder.cpp
)

# Placeholder source to make the library valid
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp
    "// pulp-platform: placeholder — replaced in Phase 2+\nnamespace pulp::platform {}\n"
)
