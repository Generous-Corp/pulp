add_library(pulp-inspect-protocol
    src/authentication.cpp
    src/capabilities.cpp
    src/protocol.cpp
    src/session.cpp)
add_library(pulp::inspect-protocol ALIAS pulp-inspect-protocol)

target_include_directories(pulp-inspect-protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-inspect-protocol PUBLIC pulp::runtime)

add_library(pulp-inspect-runtime
    src/inspector_server.cpp
    src/main_thread_rpc.cpp)
add_library(pulp::inspect-runtime ALIAS pulp-inspect-runtime)
target_include_directories(pulp-inspect-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-runtime
    PUBLIC pulp::inspect-protocol pulp::events)

if(PULP_ENABLE_GPU AND NOT ANDROID)
add_library(pulp-inspect)
add_library(pulp::inspect ALIAS pulp-inspect)

target_include_directories(pulp-inspect
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-inspect PRIVATE
    src/inspector_overlay.cpp
    src/inspector_overlay_field_edit.cpp
    src/inspector_overlay_knobs.cpp
    src/inspector_overlay_zoom.cpp
    src/inspector_overlay_pass_viewer.cpp
    src/inspector_overlay_paint.cpp
    src/inspector_window.cpp
    src/state_inspector.cpp
    src/console_capture.cpp
    src/audio_inspector.cpp
    src/domain_handler.cpp
    src/domain_runtime.cpp
    src/editor_url.cpp
    src/source_jump.cpp
    src/motion_inspector.cpp
    src/motion_scrubber.cpp
    src/trace_inspector.cpp
    src/tweak_store.cpp
    src/agent_request_queue.cpp
)

target_link_libraries(pulp-inspect
    PUBLIC pulp::view pulp::render pulp::state pulp::events pulp::audio
    PRIVATE pulp::inspect-runtime)
endif()
