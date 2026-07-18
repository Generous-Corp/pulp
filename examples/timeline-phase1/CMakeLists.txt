add_library(pulp-timeline-phase1-examples STATIC
    timeline_example_engine.cpp
    timeline_audio_player.cpp
    timeline_step_sequencer.cpp
)
target_include_directories(pulp-timeline-phase1-examples PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pulp-timeline-phase1-examples PUBLIC cxx_std_20)
target_link_libraries(pulp-timeline-phase1-examples PUBLIC
    pulp::format
    pulp::host
    pulp::playback
    pulp::timeline
    pulp::timebase
)

# Native compile gate for the same example composition under the production
# web/no-exceptions constraint. It intentionally compiles the real sources,
# rather than a reduced header smoke test.
add_library(pulp-timeline-phase1-examples-noexcept OBJECT
    timeline_example_engine.cpp
    timeline_audio_player.cpp
    timeline_step_sequencer.cpp
)
target_include_directories(pulp-timeline-phase1-examples-noexcept PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pulp-timeline-phase1-examples-noexcept PRIVATE cxx_std_20)
target_link_libraries(pulp-timeline-phase1-examples-noexcept PRIVATE
    pulp::format pulp::host pulp::playback pulp::timeline pulp::timebase)
if(MSVC)
    target_compile_options(pulp-timeline-phase1-examples-noexcept PRIVATE /EHs-c-)
else()
    target_compile_options(pulp-timeline-phase1-examples-noexcept PRIVATE -fno-exceptions)
endif()

add_executable(pulp-timeline-audio-player main_audio_player.cpp)
target_link_libraries(pulp-timeline-audio-player PRIVATE
    pulp-timeline-phase1-examples pulp::standalone)

add_executable(pulp-timeline-step-sequencer main_step_sequencer.cpp)
target_link_libraries(pulp-timeline-step-sequencer PRIVATE
    pulp-timeline-phase1-examples pulp::standalone)

if(PULP_BUILD_TESTS)
    add_test(NAME timeline-audio-player-headless-run
        COMMAND pulp-timeline-audio-player --headless)
    set_tests_properties(timeline-audio-player-headless-run PROPERTIES
        LABELS "examples;timeline;headless" TIMEOUT 10)
    add_test(NAME timeline-step-sequencer-headless-run
        COMMAND pulp-timeline-step-sequencer --headless)
    set_tests_properties(timeline-step-sequencer-headless-run PROPERTIES
        LABELS "examples;timeline;headless" TIMEOUT 10)
endif()
