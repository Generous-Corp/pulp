add_library(pulp-tool-timeline)
add_library(pulp::tool-timeline ALIAS pulp-tool-timeline)

target_sources(pulp-tool-timeline PRIVATE
    src/atomic_publisher.cpp
    src/bounded_zip_archive.cpp
    src/dawproject_media_packager.cpp
    src/timeline_agent.cpp
    src/timeline_interchange.cpp
    src/timeline_output.cpp
    src/timeline_playback.cpp
    src/timeline_project_loader.cpp
    src/timeline_render.cpp)

target_include_directories(pulp-tool-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/core/playback/src
        ${PROJECT_SOURCE_DIR}/external/miniz
)

target_link_libraries(pulp-tool-timeline
    PRIVATE
        pulp::playback
        pulp::timeline
        pulp::audio
        pulp::runtime
        pulp::interchange
        pulp::smf-interchange
        pulp::dawproject-import
        pulp::dawproject-export
)

# Reference-clock sync soak capture (TL-18). Generates a
# `pulp.timeline-sync-soak-trace.v1` trace against a SYNTHETIC reference whose
# drift is injected at a known rate, so the engine-side half of the soak needs
# no hardware and reproduces byte-for-byte. It covers the engine's chase math;
# a physical cable, real interface jitter, and driver buffering still need a rig.
if(PROJECT_IS_TOP_LEVEL)
    add_executable(pulp-sync-soak-capture sync_soak_capture.cpp)
    target_link_libraries(pulp-sync-soak-capture PRIVATE pulp::playback pulp::timebase pulp::midi)
    target_include_directories(pulp-sync-soak-capture PRIVATE ${choc_SOURCE_DIR})
endif()
