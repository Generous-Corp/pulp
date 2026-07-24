add_library(pulp-sequence
    src/host_transport_projector.cpp
    src/sequence_processor.cpp
)
add_library(pulp::sequence ALIAS pulp-sequence)

target_compile_features(pulp-sequence PUBLIC cxx_std_20)

target_include_directories(pulp-sequence
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-sequence
    PUBLIC
        pulp::audio
        pulp::format
        pulp::graph
        pulp::midi
        pulp::playback
        pulp::state
        pulp::timebase
)
