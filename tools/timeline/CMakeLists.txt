add_library(pulp-tool-timeline)
add_library(pulp::tool-timeline ALIAS pulp-tool-timeline)

target_sources(pulp-tool-timeline PRIVATE
    src/timeline_agent.cpp)

target_include_directories(pulp-tool-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pulp-tool-timeline
    PUBLIC
        pulp::playback
        pulp::timeline
        pulp::audio
)
