cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/inspector-sdk-consumer-smoke")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_source "${_fixture_root}/src")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")
file(MAKE_DIRECTORY "${_consumer_source}")

set(_producer_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_producer_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_producer_config}" _producer_config_lower)

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_producer_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Inspector SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(WRITE "${_consumer_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(PulpInspectorSdkConsumer LANGUAGES CXX)\n"
    "find_package(Pulp REQUIRED COMPONENTS inspect-protocol inspect-discovery inspect-control inspect-client inspect-runtime inspect-telemetry inspect-authoring)\n"
    "get_target_property(_publication_links Pulp::inspect-publication INTERFACE_LINK_LIBRARIES)\n"
    "get_target_property(_runtime_links Pulp::inspect-runtime INTERFACE_LINK_LIBRARIES)\n"
    "get_target_property(_telemetry_links Pulp::inspect-telemetry INTERFACE_LINK_LIBRARIES)\n"
    "if(_publication_links MATCHES \"Pulp::inspect-discovery(;|$)\" OR _runtime_links MATCHES \"Pulp::inspect-discovery(;|$)\")\n"
    "  message(FATAL_ERROR \"publisher/runtime SDK targets expose discovery reader authority\")\n"
    "endif()\n"
    "if(_telemetry_links MATCHES \"Pulp::inspect-runtime(;|$)\")\n"
    "  message(FATAL_ERROR \"telemetry SDK target exposes inspector runtime authority\")\n"
    "endif()\n"
    "add_executable(pulp-inspector-sdk-consumer main.cpp)\n"
    "target_link_libraries(pulp-inspector-sdk-consumer PRIVATE\n"
    "  Pulp::inspect-protocol Pulp::inspect-discovery Pulp::inspect-control Pulp::inspect-client\n"
    "  Pulp::inspect-runtime Pulp::inspect-telemetry Pulp::inspect-authoring)\n")
file(WRITE "${_consumer_source}/main.cpp"
    "#include <pulp/inspect/capabilities.hpp>\n"
    "#include <pulp/inspect/client.hpp>\n"
    "#include <pulp/inspect/control_broker.hpp>\n"
    "#include <pulp/inspect/discovery.hpp>\n"
    "#include <pulp/inspect/inspector_server.hpp>\n"
    "#include <pulp/inspect/inspector_delivery.hpp>\n"
    "#include <pulp/inspect/main_thread_rpc.hpp>\n"
    "#include <pulp/inspect/session.hpp>\n"
    "#include <pulp/inspect/trace_inspector.hpp>\n"
    "#include <pulp/inspect/tweak_store.hpp>\n"
    "#include <pulp/inspect/value_channel_telemetry_broker.hpp>\n"
    "int main() {\n"
    "  const auto capability = pulp::inspect::capability_from_id(\"session.describe\");\n"
    "  pulp::inspect::InspectorDiscoveryReader discovery;\n"
    "  pulp::inspect::InspectorClient client;\n"
    "  pulp::inspect::ControlBroker broker;\n"
    "  pulp::inspect::InspectorServer server;\n"
    "  pulp::inspect::TraceInspector trace;\n"
    "  pulp::inspect::TweakStore tweaks;\n"
    "  pulp::inspect::ValueChannelTelemetryBroker telemetry;\n"
    "  (void)discovery.list();\n"
    "  return capability && !client.is_connected() && !broker.is_listening()\n"
    "             && server.port() == 0 && tweaks.count() == 0\n"
    "             && telemetry.subscription_count() == 0\n"
    "             && trace.owns_method(\"Trace.snapshot\") ? 0 : 1;\n"
    "}\n")

set(_consumer_configure_args
    -S "${_consumer_source}"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")
if(_producer_config_lower STREQUAL "debug")
    list(APPEND _consumer_configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()

set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags
        " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags
        " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
string(STRIP "${_consumer_linker_flags}" _consumer_linker_flags)
if(PULP_PARENT_SANITIZER)
    list(APPEND _consumer_configure_args
        "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _consumer_configure_args
        "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(_consumer_linker_flags)
    list(APPEND _consumer_configure_args
        "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Inspector SDK consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
            --config "${_producer_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Inspector SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable_suffix "")
if(WIN32)
    set(_executable_suffix ".exe")
endif()
set(_executable
    "${_consumer_build}/pulp-inspector-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_executable}")
    set(_executable
        "${_consumer_build}/${_producer_config}/pulp-inspector-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_executable}" RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "Inspector SDK consumer exited ${_run_result}")
endif()
