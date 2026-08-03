add_library(pulp-inspect-protocol
    src/authentication.cpp
    src/capabilities.cpp
    src/protocol.cpp
    src/session.cpp
    src/test_input.cpp)
add_library(pulp::inspect-protocol ALIAS pulp-inspect-protocol)

target_include_directories(pulp-inspect-protocol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-inspect-protocol PUBLIC pulp::runtime)

add_library(pulp-inspect-discovery-support
    src/discovery_common.cpp
    src/discovery_paths.cpp
    src/discovery_security.cpp)
add_library(pulp::inspect-discovery-support
    ALIAS pulp-inspect-discovery-support)
target_include_directories(pulp-inspect-discovery-support
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-discovery-support
    PRIVATE pulp::runtime)
if(WIN32)
    target_link_libraries(
        pulp-inspect-discovery-support PRIVATE Advapi32)
endif()

add_library(pulp-inspect-discovery src/discovery_reader.cpp)
add_library(pulp::inspect-discovery ALIAS pulp-inspect-discovery)
target_include_directories(pulp-inspect-discovery
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-discovery
    PUBLIC
        pulp::inspect-protocol
        pulp::inspect-discovery-support)
add_library(pulp-inspect-publication
    src/discovery_publisher.cpp
    src/discovery_security_write.cpp)
add_library(pulp::inspect-publication ALIAS pulp-inspect-publication)
target_include_directories(pulp-inspect-publication
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-publication
    PUBLIC
        pulp::inspect-protocol
        pulp::inspect-discovery-support)
if(WIN32)
    target_link_libraries(pulp-inspect-publication PRIVATE Advapi32)
endif()

add_library(pulp-inspect-client src/client.cpp)
add_library(pulp::inspect-client ALIAS pulp-inspect-client)
target_include_directories(pulp-inspect-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-client
    PUBLIC pulp::inspect-discovery
    PRIVATE pulp::events)

add_library(pulp-inspect-runtime
    src/inspector_connected_client.hpp
    src/inspector_publication.hpp
    src/inspector_server.cpp
    src/main_thread_rpc.cpp
    src/trace_inspector.cpp)
add_library(pulp::inspect-runtime ALIAS pulp-inspect-runtime)
target_include_directories(pulp-inspect-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-runtime
    PUBLIC
        pulp::inspect-protocol
        pulp::inspect-publication
        pulp::events)

add_library(pulp-inspect-telemetry
    src/value_channel_telemetry_broker.cpp)
add_library(pulp::inspect-telemetry ALIAS pulp-inspect-telemetry)
target_include_directories(pulp-inspect-telemetry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-telemetry
    PUBLIC
        pulp::inspect-runtime
        pulp::view-core)

add_library(pulp-inspect-authoring src/tweak_store.cpp)
add_library(pulp::inspect-authoring ALIAS pulp-inspect-authoring)
target_include_directories(pulp-inspect-authoring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-authoring PUBLIC pulp::runtime)

if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
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
    src/domain_handler_capture.cpp
    src/domain_handler_inspector.cpp
    src/domain_runtime.cpp
    src/editor_url.cpp
    src/source_jump.cpp
    src/motion_inspector.cpp
    src/motion_scrubber.cpp
    src/agent_request_queue.cpp
)

target_link_libraries(pulp-inspect
    PUBLIC pulp::view pulp::render pulp::state pulp::events pulp::audio
    PRIVATE
        pulp::inspect-runtime
        pulp::inspect-telemetry
        pulp::inspect-authoring)
endif()
