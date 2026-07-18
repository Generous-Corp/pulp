add_library(pulp-playback
    src/transport.cpp
)
add_library(pulp::playback ALIAS pulp-playback)

target_compile_features(pulp-playback PUBLIC cxx_std_20)

target_include_directories(pulp-playback
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-playback
    PUBLIC
        pulp::runtime
        pulp::timebase
        pulp::audio
)
