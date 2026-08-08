add_library(pulp-inspect-protocol
    src/authentication.cpp
    src/capabilities.cpp
    src/control_json_safety.cpp
    src/control_json_schema.cpp
    src/control_manifest.cpp
    src/control_host_protocol_codec.cpp
    src/control_protocol.cpp
    src/control_protocol_codec.cpp
    src/control_protocol_fields.cpp
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

if(NOT PULP_ENABLE_INSPECTOR)
    return()
endif()

add_library(pulp-inspect-control
    src/control_admission.cpp
    src/control_artifacts.cpp
    src/control_broker.cpp
    src/control_carrier.cpp
    src/control_connection_admission.cpp
    src/control_endpoint.cpp
    src/control_executor_slot.cpp
    src/control_grants.cpp
    src/control_host_enrollment.cpp
    src/control_host_bootstrap.cpp
    src/control_host_preflight.cpp
    src/control_host_connection.cpp
    src/control_host_router.cpp
    src/control_identity.cpp
    src/control_operation_store.cpp
    src/control_operations.cpp
    src/control_peer.cpp
    src/control_private_store.cpp
    src/control_read_operations.cpp
    src/control_service.cpp)
target_sources(pulp-inspect-control PRIVATE
    src/control_trusted_host_inventory.cpp
    src/control_trusted_host_launcher.cpp)
if(APPLE AND NOT IOS AND NOT PULP_IOS)
    target_sources(pulp-inspect-control PRIVATE
        src/control_peer_mac.cpp
        src/control_static_code_identity_mac.cpp)
else()
    target_sources(pulp-inspect-control PRIVATE
        src/control_peer_stub.cpp
        src/control_static_code_identity_stub.cpp)
endif()
add_library(pulp::inspect-control ALIAS pulp-inspect-control)
target_include_directories(pulp-inspect-control
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-control
    PUBLIC pulp::inspect-protocol pulp::events pulp::platform)
if(APPLE AND NOT IOS AND NOT PULP_IOS)
    target_link_libraries(pulp-inspect-control PRIVATE
        "-framework Security" "-framework CoreFoundation")
elseif(WIN32)
    target_link_libraries(pulp-inspect-control PRIVATE Advapi32)
endif()

if(APPLE AND NOT IOS AND NOT PULP_IOS)
    add_executable(pulp-control-broker
        src/control_broker_daemon.cpp
        src/control_broker_daemon_main.cpp)
    target_include_directories(pulp-control-broker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(pulp-control-broker PRIVATE
        PULP_CONTROL_SDK_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pulp-control-broker PRIVATE pulp::inspect-control)
    install(TARGETS pulp-control-broker
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/pulp)
endif()

add_library(pulp-inspect-client
    src/control_client.cpp
    src/control_client_connection.cpp
    src/control_inspector_client.cpp
    src/control_health.cpp)
add_library(pulp::inspect-client ALIAS pulp-inspect-client)
target_include_directories(pulp-inspect-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-client
    PUBLIC pulp::inspect-control)

add_library(pulp-inspect-runtime
    src/control_state_read_executor.cpp
    src/control_state_write_executor.cpp
    src/control_main_thread_executor.cpp
    src/control_trace_session_executor.cpp
    src/main_thread_rpc.cpp
    src/trace_inspector.cpp)
add_library(pulp::inspect-runtime ALIAS pulp-inspect-runtime)
target_include_directories(pulp-inspect-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-runtime
    PUBLIC
        pulp::inspect-control
        pulp::inspect-protocol
        pulp::events
        pulp::state)

# T0 offline integration is a separate headless adapter archive so the shared
# control runtime does not acquire format/view dependencies merely by existing.
add_library(pulp-inspect-offline-runtime
    src/control_offline_render_executor.cpp)
add_library(pulp::inspect-offline-runtime ALIAS pulp-inspect-offline-runtime)
target_include_directories(pulp-inspect-offline-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-offline-runtime
    PUBLIC pulp::inspect-control pulp::format)

add_library(pulp-inspect-telemetry
    src/control_telemetry_tap.cpp
    src/value_channel_telemetry_broker.cpp)
add_library(pulp::inspect-telemetry ALIAS pulp-inspect-telemetry)
target_include_directories(pulp-inspect-telemetry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-telemetry
    PUBLIC
        pulp::inspect-protocol
        pulp::view-core)

add_library(pulp-inspect-authoring src/tweak_store.cpp)
add_library(pulp::inspect-authoring ALIAS pulp-inspect-authoring)
target_include_directories(pulp-inspect-authoring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-authoring PUBLIC pulp::runtime)

if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
# Arbitrary live-realm execution is a separate authoring archive. Safe
# protocol, transport, client, runtime, and visual inspector targets never
# link it; an explicitly opted-in host injects RuntimeEvaluator.
add_library(pulp-inspect-runtime-eval src/runtime_eval_component.cpp)
add_library(pulp::inspect-runtime-eval ALIAS pulp-inspect-runtime-eval)
target_include_directories(pulp-inspect-runtime-eval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pulp-inspect-runtime-eval PRIVATE pulp::view)

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

# Configure-time dependency proof: these base targets must remain usable
# without pulling the high-risk evaluator archive into their link closure.
foreach(_pulp_eval_free_target IN ITEMS
        pulp-inspect-protocol
        pulp-inspect-control
        pulp-inspect-client
        pulp-inspect-runtime
        pulp-inspect-offline-runtime
        pulp-inspect-telemetry
        pulp-inspect-authoring
        pulp-inspect
        pulp-format)
    if(TARGET ${_pulp_eval_free_target})
        get_target_property(_pulp_private_links ${_pulp_eval_free_target} LINK_LIBRARIES)
        get_target_property(_pulp_public_links ${_pulp_eval_free_target} INTERFACE_LINK_LIBRARIES)
        if("${_pulp_private_links};${_pulp_public_links}" MATCHES
           "pulp::inspect-runtime-eval|pulp-inspect-runtime-eval")
            message(FATAL_ERROR
                "${_pulp_eval_free_target} must not depend on pulp-inspect-runtime-eval")
        endif()
    endif()
endforeach()
unset(_pulp_eval_free_target)
unset(_pulp_private_links)
unset(_pulp_public_links)
