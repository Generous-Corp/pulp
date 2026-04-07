add_library(pulp-events)
add_library(pulp::events ALIAS pulp-events)

target_include_directories(pulp-events
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-events PRIVATE
    src/event_loop.cpp
    src/timer.cpp
    src/async_updater.cpp
    src/interprocess_connection.cpp
    src/child_process_manager.cpp
    src/volume_detector.cpp
)

target_link_libraries(pulp-events PUBLIC pulp::runtime)
