add_library(pulp-timebase
    src/compiled_tempo_map.cpp
)
add_library(pulp::timebase ALIAS pulp-timebase)

target_include_directories(pulp-timebase
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
