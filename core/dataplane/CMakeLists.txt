# The editor data plane: parameter host surfaces, named value channels and the
# audio-to-UI bridges. Nothing here draws, lays out or owns a window, so a
# format adapter, a headless host or an editor built on any UI library can use
# it without linking Pulp's view, canvas or render layers.
#
#   pulp-dataplane        host surfaces, value sources, value channels (runtime + state)
#   pulp-dataplane-audio  AudioBridge and VisualizationBridge (adds audio + signal)
#
# The split keeps pulp-dataplane's link closure free of audio and signal, so
# the view core can depend on it and stay audio-free. Both targets share one
# include root; audio_bridge.hpp and visualization_bridge.hpp need the
# audio-side target linked.

add_library(pulp-dataplane)
add_library(pulp::dataplane ALIAS pulp-dataplane)

target_compile_features(pulp-dataplane PUBLIC cxx_std_20)
target_include_directories(pulp-dataplane
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_sources(pulp-dataplane PRIVATE
    src/host_surfaces.cpp
    src/host_param_surface.cpp
    src/value_source.cpp
    src/value_channel_telemetry.cpp
    src/value_channel_set.cpp
    src/value_channel_json.cpp
)
target_link_libraries(pulp-dataplane PUBLIC pulp::runtime pulp::state)

add_library(pulp-dataplane-audio)
add_library(pulp::dataplane-audio ALIAS pulp-dataplane-audio)

target_sources(pulp-dataplane-audio PRIVATE
    src/visualization_bridge.cpp
)
target_link_libraries(pulp-dataplane-audio
    PUBLIC
        pulp::dataplane
        pulp::runtime
        pulp::audio
        pulp::signal
)
