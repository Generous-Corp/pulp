add_library(pulp-sequence
    src/host_transport_projector.cpp
    src/midi_latency_queue.cpp
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
        # Only the authoring vocabulary is used here: process_context.hpp,
        # graph_runtime_executor.hpp and processor.hpp, none of which reaches
        # pulp/view. The umbrella would add the editor surface and its
        # Skia/Dawn closure behind two public headers.
        pulp::format-core
        pulp::graph
        pulp::midi
        pulp::playback
        pulp::state
        pulp::timebase
)
