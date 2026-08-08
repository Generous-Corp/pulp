# View widgets, inspector, WidgetBridge, text editor, layout, and visual harness tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Platform maturity tests (cursor, focus, IME, context menu, accessibility)
pulp_add_test_suite(pulp-test-platform-maturity LIBRARIES pulp::view)

# Audio bridge tests
pulp_add_test_suite(pulp-test-audio-bridge LIBRARIES pulp::view)
# Widget tests
pulp_add_test_suite(pulp-test-widgets LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-parameter-edit LIBRARIES pulp::view pulp::state)
pulp_add_test_suite(pulp-test-param-host-sync LIBRARIES pulp::state INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/core/format/include)
pulp_add_test_suite(pulp-test-midi-binding LIBRARIES pulp::view pulp::state pulp::midi)
# Widget tests — Label cluster extracted from test_widgets.cpp. Label
# intrinsic_width / intrinsic_height /
# line-height multiplier / line_clamp / measured_height under
# bounded width / baseline_y from text metrics / vertical text
# direction / letter_spacing glyph counting.
pulp_add_test_suite(pulp-test-widgets-label LIBRARIES pulp::view)
# Hot-reload tests
add_executable(pulp-test-hot-reload test_hot_reload.cpp)
target_link_libraries(pulp-test-hot-reload PRIVATE pulp::view Catch2::Catch2WithMain)
# `slow`: each HotReloader scenario sleeps on file-watcher
# debounce + filesystem mtime resolution (~1-1.5 sec each).
catch_discover_tests(pulp-test-hot-reload
    PROPERTIES
        RESOURCE_LOCK hot-reload-file-watcher
        LABELS slow)

# Inspector component tests exist only when the optional SDK component is
# present. A gate-off build must not compile inspector implementation sources
# back into its test artifacts.
if(PULP_ENABLE_INSPECTOR)

# Non-GPU inspector helpers. The full inspector-domain suite is GPU-gated
# because it exercises View/Render integration, but these domain helpers are
# plain data/StateStore contracts and should stay covered in CPU-only builds.
add_executable(pulp-test-inspector-domain-helpers
    test_inspector_domain_helpers.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/audio_inspector.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/capabilities.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/console_capture.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/editor_url.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/protocol.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/state_inspector.cpp)
target_include_directories(pulp-test-inspector-domain-helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/inspect/include)
target_link_libraries(pulp-test-inspector-domain-helpers PRIVATE
    pulp::audio pulp::canvas pulp::state pulp::runtime pulp::view-core
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-domain-helpers)

# CPU-only inspector session policy, capability enforcement, and lease tests.
add_executable(pulp-test-inspector-session
    test_inspector_session.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/authentication.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/capabilities.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/main_thread_rpc.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/protocol.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/session.cpp)
target_include_directories(pulp-test-inspector-session PRIVATE
    ${CMAKE_SOURCE_DIR}/inspect/include
    ${CMAKE_SOURCE_DIR}/inspect/src)
target_link_libraries(pulp-test-inspector-session PRIVATE
    pulp::canvas pulp::events pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-session)

add_executable(pulp-test-inspector-test-input
    test_inspector_test_input.cpp)
target_link_libraries(pulp-test-inspector-test-input PRIVATE
    pulp::inspect-protocol Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-test-input)

add_executable(pulp-test-control-manifest test_control_manifest.cpp)
target_link_libraries(pulp-test-control-manifest PRIVATE
    pulp::inspect-protocol Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-manifest
    PROPERTIES LABELS "inspect;control;manifest")

add_executable(pulp-test-control-protocol test_control_protocol.cpp)
target_link_libraries(pulp-test-control-protocol PRIVATE
    pulp::inspect-protocol Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-protocol
    PROPERTIES LABELS "inspect;control;protocol")

add_executable(pulp-test-control-protocol-fuzz
    test_control_protocol_fuzz.cpp)
target_include_directories(pulp-test-control-protocol-fuzz PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pulp-test-control-protocol-fuzz PRIVATE
    pulp::inspect-protocol Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-protocol-fuzz
    PROPERTIES LABELS "inspect;control;protocol;fuzz")

add_executable(pulp-generate-control-protocol-fuzz-corpus
    fuzz/control_protocol_seed_corpus_main.cpp)
target_include_directories(pulp-generate-control-protocol-fuzz-corpus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz)
target_link_libraries(pulp-generate-control-protocol-fuzz-corpus PRIVATE
    pulp::inspect-protocol)

if(PULP_ENABLE_FUZZING)
    add_executable(pulp-fuzz-control-protocol
        fuzz/control_protocol_fuzz_target.cpp)
    target_include_directories(pulp-fuzz-control-protocol PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pulp-fuzz-control-protocol PRIVATE
        pulp::inspect-protocol)
    target_compile_options(pulp-fuzz-control-protocol PRIVATE
        -fsanitize=fuzzer,address -fno-omit-frame-pointer)
    target_link_options(pulp-fuzz-control-protocol PRIVATE
        -fsanitize=fuzzer,address)
endif()

add_executable(pulp-test-control-identity test_control_identity.cpp)
target_link_libraries(pulp-test-control-identity PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-identity
    PROPERTIES LABELS "inspect;control;identity")

add_executable(pulp-test-control-peer test_control_peer.cpp)
target_link_libraries(pulp-test-control-peer PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
if(APPLE)
    find_program(_pulp_test_codesign codesign REQUIRED)
    # Apple Silicon's linker emits an ad-hoc signature for native executables,
    # but a cross-built x86_64 test binary may be unsigned. This security test
    # intentionally rejects unsigned peers, so sign the fixture explicitly on
    # every macOS architecture instead of weakening the production verifier.
    add_custom_command(TARGET pulp-test-control-peer POST_BUILD
        COMMAND "${_pulp_test_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-peer>"
        COMMENT "Ad-hoc signing control peer test fixture"
        VERBATIM)
endif()
catch_discover_tests(pulp-test-control-peer
    PROPERTIES LABELS "inspect;control;identity;peer")

add_executable(pulp-test-control-endpoint test_control_endpoint.cpp)
target_link_libraries(pulp-test-control-endpoint PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
if(APPLE)
    find_program(_pulp_endpoint_test_codesign codesign REQUIRED)
    add_custom_command(TARGET pulp-test-control-endpoint POST_BUILD
        COMMAND "${_pulp_endpoint_test_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-endpoint>"
        COMMENT "Ad-hoc signing control endpoint test fixture"
        VERBATIM)
endif()
catch_discover_tests(pulp-test-control-endpoint
    PROPERTIES LABELS "inspect;control;carrier")

add_executable(pulp-test-control-host-router test_control_host_router.cpp)
target_link_libraries(pulp-test-control-host-router PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-host-router
    PROPERTIES LABELS "inspect;control;host;router")

add_executable(pulp-test-control-carrier test_control_carrier.cpp)
target_link_libraries(pulp-test-control-carrier PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-carrier
    PROPERTIES LABELS "inspect;control;carrier;security")

if(APPLE AND NOT IOS AND NOT PULP_IOS)
    add_executable(pulp-test-control-broker-daemon
        test_control_broker_daemon.cpp
        ${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp)
    target_include_directories(pulp-test-control-broker-daemon PRIVATE
        ${CMAKE_SOURCE_DIR}/inspect/src)
    target_link_libraries(pulp-test-control-broker-daemon PRIVATE
        pulp::inspect-control Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-control-broker-daemon
        PROPERTIES LABELS "inspect;control;carrier;daemon")
endif()

add_executable(pulp-test-control-grants test_control_grants.cpp)
target_link_libraries(pulp-test-control-grants PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-grants
    PROPERTIES LABELS "inspect;control;grants")

add_executable(pulp-test-control-broker test_control_broker.cpp)
target_link_libraries(pulp-test-control-broker PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-broker
    PROPERTIES LABELS "inspect;control;broker")

add_executable(pulp-test-control-admission test_control_admission.cpp)
target_link_libraries(pulp-test-control-admission PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-admission
    PROPERTIES LABELS "inspect;control;admission")

add_executable(pulp-test-control-operations test_control_operations.cpp)
target_link_libraries(pulp-test-control-operations PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-operations
    PROPERTIES LABELS "inspect;control;receipt")

add_executable(pulp-test-control-artifacts test_control_artifacts.cpp)
target_link_libraries(pulp-test-control-artifacts PRIVATE
    pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-artifacts
    PROPERTIES LABELS "inspect;control;artifact")

add_executable(pulp-test-control-service test_control_service.cpp)
target_link_libraries(pulp-test-control-service PRIVATE
    pulp::inspect-control pulp::inspect-client Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-service
    PROPERTIES LABELS "inspect;control;service;client")

add_executable(pulp-test-control-client-connection
    test_control_client_connection.cpp)
target_link_libraries(pulp-test-control-client-connection PRIVATE
    pulp::inspect-client Catch2::Catch2WithMain)
if(APPLE)
    find_program(_pulp_client_connection_codesign codesign REQUIRED)
    add_custom_command(TARGET pulp-test-control-client-connection POST_BUILD
        COMMAND "${_pulp_client_connection_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-client-connection>"
        COMMENT "Ad-hoc signing control client connection test fixture"
        VERBATIM)
endif()
catch_discover_tests(pulp-test-control-client-connection
    PROPERTIES LABELS "inspect;control;carrier;client")

add_executable(pulp-test-control-health test_control_health.cpp)
target_link_libraries(pulp-test-control-health PRIVATE
    pulp::inspect-client Catch2::Catch2WithMain)
if(APPLE)
    find_program(_pulp_control_health_codesign codesign REQUIRED)
    add_custom_command(TARGET pulp-test-control-health POST_BUILD
        COMMAND "${_pulp_control_health_codesign}" --force --sign -
                "$<TARGET_FILE:pulp-test-control-health>"
        COMMENT "Ad-hoc signing control health test fixture"
        VERBATIM)
endif()
catch_discover_tests(pulp-test-control-health
    PROPERTIES LABELS "inspect;control;carrier;health")

add_executable(pulp-test-control-main-thread-executor
    test_control_main_thread_executor.cpp)
target_link_libraries(pulp-test-control-main-thread-executor PRIVATE
    pulp::inspect-runtime pulp::inspect-control Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-main-thread-executor
    PROPERTIES LABELS "inspect;control;main-thread;executor")

add_executable(pulp-test-inspector-audit
    test_inspector_audit.cpp
    test_main_thread_rpc_timeout.cpp
    ${CMAKE_SOURCE_DIR}/inspect/src/main_thread_rpc.cpp)
target_link_libraries(pulp-test-inspector-audit PRIVATE
    pulp::inspect-protocol pulp::events Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-audit
    PROPERTIES LABELS "inspect;control;main-thread;timeout")

add_executable(pulp-test-inspector-server
    test_inspector_server.cpp
    unsafe_legacy_inspector_server.cpp)
target_link_libraries(pulp-test-inspector-server PRIVATE
    pulp::inspect-protocol pulp::events Catch2::Catch2WithMain)
if(WIN32)
    catch_discover_tests(pulp-test-inspector-server
        PROPERTIES
            ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1"
            LABELS "windows-pr-quarantine")
else()
    catch_discover_tests(pulp-test-inspector-server
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
endif()

add_executable(pulp-test-inspector-discovery test_inspector_discovery.cpp)
target_link_libraries(pulp-test-inspector-discovery PRIVATE
    pulp::inspect-discovery
    pulp::inspect-publication
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-discovery)

add_executable(
    pulp-test-inspector-client
    test_inspector_client.cpp
    test_inspector_server_async_lifecycle.cpp
    test_inspector_server_lifecycle.cpp
    test_inspector_client_limits.cpp
)
target_link_libraries(pulp-test-inspector-client PRIVATE
    pulp::inspect-client pulp::inspect-runtime Catch2::Catch2WithMain)
target_include_directories(pulp-test-inspector-client PRIVATE
    ${PROJECT_SOURCE_DIR}/inspect/src)
catch_discover_tests(pulp-test-inspector-client)

add_executable(pulp-test-inspector-value-channel-telemetry
    test_value_channel_telemetry_broker.cpp)
target_link_libraries(pulp-test-inspector-value-channel-telemetry PRIVATE
    pulp::inspect-telemetry pulp::inspect-client pulp::inspect-runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-inspector-value-channel-telemetry)

# Inspector tests — only when GPU is enabled (pulp-inspect requires GPU stack).
if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
    add_executable(pulp-test-inspector test_inspector.cpp)
    target_link_libraries(pulp-test-inspector PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    # PULP_INSPECTOR_NO_LAUNCH: the source-jump tests exercise the J
    # hotkey, whose handler resolves with dry_run=false. Without this
    # guard the test would spawn a real `open vscode://file/...`, popping
    # a macOS open-confirmation dialog. The env var makes launch_editor_url()
    # a no-op. The test file also sets it in-process so it stays safe when
    # the binary is run directly, outside CTest.
    catch_discover_tests(pulp-test-inspector
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Inspector sibling TUs: GPU pass attribution, source-jump,
    # drift/reconcile, and atlas viewer.
    # Each mirrors pulp-test-inspector's exact registration (own executable
    # + catch_discover_tests, same libraries, same NO_LAUNCH env guard).
    add_executable(pulp-test-inspector-gpu-passes test_inspector_gpu_passes.cpp)
    target_link_libraries(pulp-test-inspector-gpu-passes PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-gpu-passes
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Wiring tab — lists design-sourced (Figma) overlays + wired/unwired badge.
    add_executable(pulp-test-inspector-wiring test_inspector_wiring.cpp)
    target_link_libraries(pulp-test-inspector-wiring PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-wiring
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-source-jump test_inspector_source_jump.cpp)
    target_link_libraries(pulp-test-inspector-source-jump PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-source-jump
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-drift-reconcile test_inspector_drift_reconcile.cpp)
    target_link_libraries(pulp-test-inspector-drift-reconcile PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-drift-reconcile
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-atlas-viewer test_inspector_atlas_viewer.cpp)
    target_link_libraries(pulp-test-inspector-atlas-viewer PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-atlas-viewer
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Additional inspector sibling TUs. Same registration as
    # pulp-test-inspector.
    add_executable(pulp-test-inspector-domains test_inspector_domains.cpp)
    target_link_libraries(pulp-test-inspector-domains PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-domains
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-runtime-domain
        test_inspector_runtime_domain.cpp)
    target_link_libraries(pulp-test-inspector-runtime-domain PRIVATE
        pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-runtime-domain
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspect-runtime-eval-component
        test_runtime_eval_component.cpp)
    target_link_libraries(pulp-test-inspect-runtime-eval-component PRIVATE
        pulp::inspect-runtime-eval pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspect-runtime-eval-component)

    add_test(NAME pulp-inspect-runtime-eval-archive-boundary
        COMMAND ${CMAKE_COMMAND}
            -DEVAL_ARCHIVE=$<TARGET_FILE:pulp-inspect-runtime-eval>
            -DBASE_INSPECT=$<TARGET_FILE:pulp-inspect>
            -DBASE_RUNTIME=$<TARGET_FILE:pulp-inspect-runtime>
            -DBASE_PROTOCOL=$<TARGET_FILE:pulp-inspect-protocol>
            -DBASE_CLIENT=$<TARGET_FILE:pulp-inspect-client>
            -DBASE_FORMAT=$<TARGET_FILE:pulp-format>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/inspect_runtime_eval_archive_check.cmake)

    add_executable(pulp-test-inspector-hook-lifecycle
        test_inspector_hook_lifecycle.cpp)
    target_link_libraries(pulp-test-inspector-hook-lifecycle PRIVATE
        pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-hook-lifecycle
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-context-capture
        test_inspector_context_capture.cpp)
    target_link_libraries(pulp-test-inspector-context-capture PRIVATE
        pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-context-capture
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Trace.* bridge to the process-global pulp::runtime::Tracing controller.
    # Config-agnostic: verifies the OFF (shipping) build reports tracing is not
    # compiled in, and the ON build round-trips a real .pftrace.
    add_executable(pulp-test-trace-inspector test_trace_inspector.cpp)
    target_link_libraries(pulp-test-trace-inspector PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-trace-inspector
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-field-edit test_inspector_field_edit.cpp)
    target_link_libraries(pulp-test-inspector-field-edit PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-field-edit
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-eyedropper test_inspector_eyedropper.cpp)
    target_link_libraries(pulp-test-inspector-eyedropper PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-eyedropper
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-overlay-knobs test_inspector_overlay_knobs.cpp)
    target_link_libraries(pulp-test-inspector-overlay-knobs PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-overlay-knobs
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-knobs-host-integration test_inspector_knobs_host_integration.cpp)
    target_link_libraries(pulp-test-inspector-knobs-host-integration PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-knobs-host-integration
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    add_executable(pulp-test-inspector-knob-panel-screenshot test_inspector_knob_panel_screenshot.cpp)
    target_link_libraries(pulp-test-inspector-knob-panel-screenshot PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-inspector-knob-panel-screenshot
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # TweakStore + Inspector.applyTweak protocol surface.
    add_executable(pulp-test-tweak-store test_tweak_store.cpp)
    target_link_libraries(pulp-test-tweak-store PRIVATE pulp::view pulp::inspect pulp::state Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-tweak-store)

    # Editor URI plumbing for the source-jump action.
    # Pure config / format-helper / protocol — no overlay / GPU surface,
    # but lives under the same PULP_ENABLE_GPU guard as the rest of
    # pulp-inspect's tests so it links against the same library.
    add_executable(pulp-test-editor-url test_editor_url.cpp)
    target_link_libraries(pulp-test-editor-url PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-editor-url
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")

    # Agent-request queue: pure serialize/parse/append/ack + atomic file I/O.
    # No overlay / GPU surface, but links pulp::inspect so it shares the guard.
    add_executable(pulp-test-agent-request-queue test_agent_request_queue.cpp)
    target_link_libraries(pulp-test-agent-request-queue PRIVATE pulp::inspect Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-agent-request-queue
        PROPERTIES ENVIRONMENT "PULP_INSPECTOR_NO_LAUNCH=1")
endif()
endif()

# An ordinary pulp::standalone consumer is the inspector-stripped artifact fixture.
# The post-build scan is non-vacuous when inspector components are present:
# it first proves their archive contains pulp::inspect symbols, then rejects
# every defined pulp::inspect symbol in this consumer.
add_executable(pulp-test-inspector-stripped-artifact
    fixtures/inspector_stripped_artifact.cpp)
target_link_libraries(pulp-test-inspector-stripped-artifact PRIVATE
    pulp::standalone)
set(PULP_pulp-test-inspector-stripped-artifact_SHIP_INSPECTOR FALSE)
set(PULP_pulp-test-inspector-stripped-artifact_SHIP_INSPECTOR_RUNTIME_EVAL FALSE)
set(PULP_pulp-test-inspector-stripped-artifact_INSPECTOR_CAPABILITIES "")
set(PULP_pulp-test-inspector-stripped-artifact_INSPECTOR_MANIFEST_DIRECTORY
    "${CMAKE_BINARY_DIR}/pulp-inspector-test-manifests")
_pulp_configure_inspector_shipping(
    pulp-test-inspector-stripped-artifact
    "com.pulp.test.inspector-stripped"
    "Inspector Stripped Artifact")
_pulp_attach_inspector_shipping(
    pulp-test-inspector-stripped-artifact pulp-test-inspector-stripped-artifact)
add_test(NAME inspector-stripped-artifact-runs
    COMMAND pulp-test-inspector-stripped-artifact)
unset(_pulp_inspector_symbol_tool)
set(_pulp_inspector_symbol_mode "NM")
if(MSVC)
    get_filename_component(_pulp_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(_pulp_inspector_symbol_tool
        NAMES dumpbin.exe dumpbin llvm-nm.exe llvm-nm
        HINTS "${_pulp_compiler_dir}")
    if(_pulp_inspector_symbol_tool MATCHES "llvm-nm")
        set(_pulp_inspector_symbol_mode "COFF_NM")
    else()
        set(_pulp_inspector_symbol_mode "DUMPBIN")
    endif()
elseif(CMAKE_NM)
    set(_pulp_inspector_symbol_tool "${CMAKE_NM}")
endif()
if(NOT _pulp_inspector_symbol_tool)
    message(FATAL_ERROR
        "The inspector stripped-artifact proof requires nm, llvm-nm, or dumpbin")
else()
    set(_pulp_inspector_archive "")
    if(TARGET pulp-inspect-runtime)
        set(_pulp_inspector_archive "$<TARGET_FILE:pulp-inspect-runtime>")
    endif()
    add_custom_command(TARGET pulp-test-inspector-stripped-artifact POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSYMBOL_TOOL=${_pulp_inspector_symbol_tool}"
            "-DSYMBOL_MODE=${_pulp_inspector_symbol_mode}"
            "-DSTRIPPED_ARTIFACT=$<TARGET_FILE:pulp-test-inspector-stripped-artifact>"
            "-DINSPECTOR_ARCHIVE=${_pulp_inspector_archive}"
            -P "${CMAKE_CURRENT_LIST_DIR}/check_inspector_stripped_artifact.cmake"
        COMMENT "Checking ordinary standalone consumers contain no inspector symbols")
    unset(_pulp_inspector_archive)
endif()
unset(_pulp_compiler_dir)
unset(_pulp_inspector_symbol_mode)
unset(_pulp_inspector_symbol_tool)

# Widget bridge tests
set(_pulp_widget_bridge_test_libs pulp::view)
if(TARGET pulp-render)
    list(APPEND _pulp_widget_bridge_test_libs pulp::render)
endif()
pulp_add_test_suite(pulp-test-widget-bridge LIBRARIES ${_pulp_widget_bridge_test_libs})
pulp_add_test_suite(pulp-test-widget-bridge-capabilities LIBRARIES ${_pulp_widget_bridge_test_libs})
pulp_add_test_suite(pulp-test-widget-bridge-removal-lifetime
    LIBRARIES ${_pulp_widget_bridge_test_libs})

# Widget bridge — source-level API contract. Keeps JS-native registrations
# unique and matched to the reviewed bridge API manifest so future registrar
# splits cannot add unclassified bridge entries.
pulp_add_test_suite(pulp-test-widget-bridge-api-contracts
    COMPILE_DEFINITIONS PULP_REPO_ROOT="${CMAKE_SOURCE_DIR}")

# Widget bridge — no-GPU gate enforcement.
# Pure static scan: walks widget_bridge.cpp line-by-line and asserts every
# `gpu_surface_->` dereference is inside a PULP_WIDGET_BRIDGE_HAS_GPU_SURFACE
# gate (or the #else of an #ifndef PULP_HAS_SKIA block, which implies the
# render module's include path is present). Catches the iOS Simulator
# regression class where GpuSurface is forward-declared but its members are
# called from configures that did not link the render module. Runs in the
# default macOS lane in milliseconds — no Xcode / iOS SDK required, so it
# closes the gap the slow `cmake-ios-auv3-configure` test left behind for
# validation.
pulp_add_test_suite(pulp-test-widget-bridge-no-gpu-gates
    COMPILE_DEFINITIONS PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Widget bridge — runtime-import handlers.
pulp_add_test_suite(pulp-test-widget-bridge-runtime-import LIBRARIES pulp::view)

# Widget bridge — declarative native→widget param/meter bindings
# (bindWidgetToParam / bindMeter / unbindWidget + gesture precedence).
pulp_add_test_suite(pulp-test-widget-bridge-param-binding LIBRARIES pulp::view pulp::state)

# Widget bridge — Canvas2D surface. Covers canvasSetTransform /
# canvasClip / canvasGlobalCompositeOperation, canvasMeasureText /
# canvasSetLineDash / canvasDrawImage, canvasGetImageData /
# canvasPutImageData, and 4-arg canvasFillText prior-state preservation.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d LIBRARIES pulp::view)

# Widget bridge — Yoga layer. Three coherent clusters: (1) dimension
# percent strings (width/height + min/max via Yoga's percent API),
# (2) flexBasis% promotion (basis "NN%" + "flex 1 1 NN%"
# decomposition), and (3) yoga value-aliasing
# (flexDirection / justifyContent / alignItems / alignSelf / order /
# flexWrap value translations).
pulp_add_test_suite(pulp-test-widget-bridge-yoga LIBRARIES pulp::view)

# Widget bridge — recovered Canvas2D/CSS compatibility regressions.
# The canonical Canvas2D bridge surface is in
# pulp-test-widget-bridge-wave2-cheap below; this older split keeps
# recovered CSS cases and later Canvas2D bridge regressions.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-wave2 LIBRARIES pulp::view)

# Widget bridge — yoga logical-edge fan-out + A4 OOS pins. Covers yoga
# logical-edge marginInline/Block + paddingInline/Block + inset
# shorthand plus CSS NOT-IMPL closure catalog hygiene.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-a4-oos LIBRARIES pulp::view)

# Widget bridge — compatibility cheap-wiring. Bundles the Canvas2D
# wiring (DIVERGE → PASS) + CSS value-coverage entries from compat.json.
pulp_add_test_suite(pulp-test-widget-bridge-wave2-cheap LIBRARIES pulp::view)

# Widget bridge — RN style-prop bridge primitives.
# setShadow / setOpacity / setTransform RN-shaped style
# functions flow through bridge into View's slots. Includes RN's
# shadowOpacity-into-color-alpha composition + transform-prop
# aggregation.
pulp_add_test_suite(pulp-test-widget-bridge-rn-style LIBRARIES pulp::view)

# Widget bridge — Tier-4 OOS / perf-hints / interaction misc pins.
# Pins the no-op / fallback contract for properties
# Pulp deliberately doesn't paint: 3D transforms, generated content,
# scroll-snap, will-change, contain, touch-action
# secondary keywords. Catalog hygiene tests.
pulp_add_test_suite(pulp-test-widget-bridge-tier4-oos LIBRARIES pulp::view)

# Widget bridge — RN outline cluster.
# outlineColor / outlineOffset / outlineStyle / outlineWidth longhands
# + `outline` shorthand decomposition through the RN style shim.
pulp_add_test_suite(pulp-test-widget-bridge-rn-outline LIBRARIES pulp::view)

# Widget bridge — clip-path + mask cluster.
# clip-path: inset/circle/polygon/url + shape coordinate parsing;
# mask-image: url + linear-gradient + transforms; mask-size /
# -position / -repeat / -origin / -clip / -composite; mask shorthand.
pulp_add_test_suite(pulp-test-widget-bridge-clip-mask LIBRARIES pulp::view)

# Widget bridge — SVG widgets. Three clusters: SvgPathWidget JS bridge
# integration, SvgRectWidget + SvgLineWidget JS bridge integration,
# and compound-path
# parser regression (Spectr PEAK / AVG / BOTH / OFF analyzer icons).
pulp_add_test_suite(pulp-test-widget-bridge-svg LIBRARIES pulp::view)

# Widget bridge — CSS compatibility audit. Runtime-path coverage for
# 49 entries flipped from partial/DIVERGE to supported:
# backgroundPosition / backgroundSize / textShadow /
# border / border-side / borderRadius / per-corner radius / boxShadow
# / opacity / outline / textOverflow / transformOrigin / zIndex /
# backdropFilter / display / overflow / overflow per-axis / and
# many more. 45 TEST_CASEs each exercising JS shim → bridge → View
# slot round-trip.
pulp_add_test_suite(pulp-test-widget-bridge-wave5-css LIBRARIES pulp::view)

# Widget bridge — HTML ARIA + querySelector. aria-label / role flow
# into View accessibility slots;
# document.querySelector accepts attribute selectors + combinators +
# :hover / :disabled / :checked / :enabled / :not / :first-child /
# :nth-child / :empty pseudo-classes.
pulp_add_test_suite(pulp-test-widget-bridge-html-aria LIBRARIES pulp::view)

# Widget bridge — CSS animations + transitions.
# animation-* longhands + shorthand decomposition; transition-*
# longhands + shorthand round-trip through the CSS shim and bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-animations LIBRARIES pulp::view)

# Widget bridge — CSS Grid extended surface.
# grid-template-columns / -rows / -areas, grid-column / -row / -area
# placement shorthand, gap longhands + shorthand, justify-* / align-* /
# place-* alignment, repeat() + minmax() + fr-unit + auto sizing tokens
# round-trip through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-grid LIBRARIES pulp::view)

# Widget bridge — animation API cluster. Bridge ↔ MotionEngine
# plumbing for setMotionToken, animate(), the Web
# Animations API surface (Element.animate / KeyframeEffect), motion
# provenance, and pulp-motion-bench harness output. Self-contained
# ~800-line cluster from test_widget_bridge.cpp.
pulp_add_test_suite(pulp-test-widget-bridge-animation LIBRARIES pulp::view)

# Widget bridge — per-edge margin / padding. marginTop /
# marginRight / marginBottom / marginLeft (and padding counterparts)
# each route to their own Yoga edge enum without cross-contamination
# through the bridge.
pulp_add_test_suite(pulp-test-widget-bridge-css-per-edge LIBRARIES pulp::view)

# Widget bridge — RN-OOS-fixup catalog audit tail. Material elevation
# shim, includeFontPadding round-trip, borderCurve
# squircle paint dispatch, isolation honest CSS-subset, and other
# RN-side OOS catalog hygiene checks.
pulp_add_test_suite(pulp-test-widget-bridge-rn-oos-fixup LIBRARIES pulp::view)

# Widget bridge — Canvas2D bridge-fn cluster. canvasSetFontFull,
# fillRule, and canvasSetDirection / canvasSetFilter are closely related
# bridge entry-points that thread JS-side Canvas2D semantics through
# WidgetBridge into the native canvas pipeline.
pulp_add_test_suite(pulp-test-widget-bridge-canvas2d-bridge-fns LIBRARIES pulp::view)

# Widget bridge — Yoga borderWidth wiring.
# Pins YGNodeStyleSetBorder integration with Yoga 3.x default box-sizing
# (border-box); the content-box case belongs with setBoxSizing coverage.
pulp_add_test_suite(pulp-test-widget-bridge-yoga-border LIBRARIES pulp::view)

# Widget bridge — CSS-misc cluster. Text-decoration longhands,
# line-clamp, and background-repeat are two coherent
# small CSS clusters that keep test_widget_bridge.cpp under the
# 3,000-line target.
pulp_add_test_suite(pulp-test-widget-bridge-css-misc LIBRARIES pulp::view)

# Autonomous Spectr regression suite. Composes the six
# [contract] invariants from
# test_widget_bridge.cpp into a Spectr-shaped mini-scenario so a future
# change that keeps each unit-level invariant passing but breaks their
# *interaction* still fails first.
pulp_add_test_suite(pulp-test-spectr-regression LIBRARIES pulp::view)

# Typography inheritance for the CSS-style cascade
pulp_add_test_suite(pulp-test-typography-inheritance LIBRARIES pulp::view)

# Text-overflow ellipsis helper
pulp_add_test_suite(pulp-test-text-overflow LIBRARIES pulp::view)

# Editor bridge tests for the renderer-agnostic envelope/dispatcher
pulp_add_test_suite(pulp-test-editor-bridge LIBRARIES pulp::view)

# Input events tests
pulp_add_test_suite(pulp-test-input-events LIBRARIES pulp::view)

# Text editor tests
# Caret shape geometry + the solid-while-moving blink policy shared by every
# editable widget.
pulp_add_test_suite(pulp-test-caret LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-mouse LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)
pulp_add_test_suite(pulp-test-text-editor-paint LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-policy LIBRARIES pulp::view PROPERTIES RESOURCE_LOCK system-clipboard)

# TextEditor multi-line coverage: wrap, click-to-caret in
# wrapped rows, caret_rect pixel positioning, single-vs-multi Enter
# contract). Keeps the original test_text_editor.cpp file unchanged
# so the existing single-line surface stays pinned in isolation.
pulp_add_test_suite(pulp-test-text-editor-multiline LIBRARIES pulp::view)

# TextEditor input pipeline tests (headless — validates focus, typing, Enter, backspace)
pulp_add_test_suite(pulp-test-text-input LIBRARIES pulp::view)

# W3C Layout parity tests (flexbox, box model, visual properties)
pulp_add_test_suite(pulp-test-layout-w3c LIBRARIES pulp::view)

# W3C design-token runtime pair (parse/export) via the self-contained
# w3c_tokens.hpp — stays always-compiled when design-import is gated.
pulp_add_test_suite(pulp-test-w3c-tokens LIBRARIES pulp::view)

# LottieView playback logic (opt-in PULP_LOTTIE). Passes whether or not Lottie
# is compiled in: LottieView::supported() gates the playback assertions.
pulp_add_test_suite(pulp-test-lottie-view LIBRARIES pulp::view)

# Visual semantic snapshot harness. This is a custom-main Catch2 binary:
# --self-test runs its focused tests, while --fixture emits a stable JSON
# snapshot for tools/harness/visual/runner.py.
add_executable(pulp-test-visual visual/pulp-test-visual.cpp)
target_link_libraries(pulp-test-visual PRIVATE pulp::view Catch2::Catch2)
add_test(NAME visual-harness-self-test COMMAND pulp-test-visual --self-test)
set_tests_properties(visual-harness-self-test PROPERTIES
    LABELS "visual;harness;yoga"
    TIMEOUT 30)

# Off-UI-thread query service (R7): worker + marshal-back, and the JS bridge API.
pulp_add_test_suite(pulp-test-query-service LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-widget-bridge-query LIBRARIES pulp::view pulp::state)
