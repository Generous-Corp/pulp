add_library(pulp-tool-timeline)
add_library(pulp::tool-timeline ALIAS pulp-tool-timeline)

target_sources(pulp-tool-timeline PRIVATE
    src/timeline_agent.cpp
    src/timeline_output.cpp
    src/timeline_playback.cpp
    src/timeline_project_loader.cpp
    src/timeline_render.cpp)

target_include_directories(pulp-tool-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(pulp-tool-timeline
    PRIVATE
        pulp::playback
        pulp::timeline
        pulp::audio
        pulp::runtime
)
