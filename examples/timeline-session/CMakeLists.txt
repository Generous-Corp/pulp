add_library(pulp-timeline-session-examples STATIC
    timeline_launch_session.cpp
    timeline_daw_project.cpp
)
target_include_directories(pulp-timeline-session-examples PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pulp-timeline-session-examples PUBLIC cxx_std_20)
target_link_libraries(pulp-timeline-session-examples PUBLIC
    pulp::playback
    pulp::timeline
    pulp::timebase
)
add_executable(pulp-timeline-launch-session main_launch_session.cpp)
target_link_libraries(pulp-timeline-launch-session PRIVATE pulp-timeline-session-examples)

add_executable(pulp-timeline-daw-project main_daw_project.cpp)
target_link_libraries(pulp-timeline-daw-project PRIVATE pulp-timeline-session-examples)

if(PULP_ENABLE_PROJECT_PACKAGE)
    add_library(pulp-timeline-project-session-shell STATIC project_session_shell.cpp)
    target_link_libraries(pulp-timeline-project-session-shell PUBLIC
        pulp::project-package
        pulp::timeline
    )

    add_executable(pulp-timeline-project-session main_project_session.cpp)
    target_link_libraries(pulp-timeline-project-session PRIVATE
        pulp-timeline-project-session-shell
    )
endif()

if(PULP_BUILD_TESTS)
    add_test(NAME timeline-launch-session-headless-run
        COMMAND pulp-timeline-launch-session)
    set_tests_properties(timeline-launch-session-headless-run PROPERTIES
        LABELS "examples;timeline;headless;launcher" TIMEOUT 30)
    add_test(NAME timeline-daw-project-headless-run
        COMMAND pulp-timeline-daw-project)
    set_tests_properties(timeline-daw-project-headless-run PROPERTIES
        LABELS "examples;timeline;headless;nesting" TIMEOUT 30)
    if(PULP_ENABLE_PROJECT_PACKAGE)
        add_test(NAME timeline-project-session-headless-run
            COMMAND pulp-timeline-project-session)
        set_tests_properties(timeline-project-session-headless-run PROPERTIES
            LABELS "examples;timeline;headless;project-package" TIMEOUT 30)
    endif()
endif()
