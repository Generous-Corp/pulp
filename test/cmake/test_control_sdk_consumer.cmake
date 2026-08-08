cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/control-sdk-consumer-smoke")
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
        "Control SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(WRITE "${_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpControlSdkConsumer LANGUAGES CXX)

find_package(Pulp REQUIRED COMPONENTS
    inspect-protocol
    inspect-control
    inspect-client
    inspect-runtime)

set(_control_targets
    Pulp::inspect-protocol
    Pulp::inspect-control
    Pulp::inspect-client
    Pulp::inspect-runtime)
set(_forbidden_direct_dependency
    "pulp::(render|view|format|host|canvas|gpu-audio)|pulp-(render|view|format|host|canvas|gpu-audio|cli|mcp)")
foreach(_target IN LISTS _control_targets)
    if(NOT TARGET ${_target})
        message(FATAL_ERROR "Installed control SDK target is missing: ${_target}")
    endif()
    get_target_property(_links ${_target} INTERFACE_LINK_LIBRARIES)
    if(_links)
        string(TOLOWER "${_links}" _links_lower)
        if(_links_lower MATCHES "${_forbidden_direct_dependency}")
            message(FATAL_ERROR
                "${_target} exposes a forbidden direct dependency: ${_links}")
        endif()
    endif()
endforeach()

add_executable(pulp-control-sdk-consumer main.cpp)
target_compile_features(pulp-control-sdk-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-control-sdk-consumer PRIVATE ${_control_targets})
]=])

file(WRITE "${_consumer_source}/main.cpp" [=[
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_admission.hpp>
#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_executor_slot.hpp>
#include <pulp/inspect/control_host_enrollment.hpp>
#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_trace_session_executor.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class InstalledControlTransport final
    : public pulp::inspect::ControlClientTransport {
 public:
  pulp::inspect::ControlTransportDispatchResult dispatch(
      std::string_view, std::chrono::milliseconds) override {
    return {.error_code = "not-connected"};
  }

  pulp::inspect::ControlArtifactReadResult read_artifact(
      std::string_view artifact_id,
      std::uint64_t,
      std::size_t,
      std::chrono::milliseconds) override {
    return {
        .status = pulp::inspect::ControlArtifactStatus::Read,
        .metadata = pulp::inspect::ControlArtifactMetadata{
            .artifact_id = std::string(artifact_id),
            .lineage = {.producer_client_id = "installed-client"},
        },
        .bytes = {1},
        .eof = true,
    };
  }
};

int main() {
  pulp::inspect::ControlBroker broker;
  pulp::inspect::ControlHostEnrollmentStore enrollments;
  pulp::inspect::InspectorClient client;
  InstalledControlTransport transport;
  pulp::inspect::ControlClient control_client{transport};
  pulp::inspect::ControlService service{broker};
  pulp::inspect::ControlHostRouter host_router;
  pulp::inspect::ControlOperationExecutorSlot executor_slot;
  pulp::inspect::ControlHostConnection host_connection{
      {.endpoint_path = "/tmp/not-connected-control.sock"}, executor_slot.executor()};
  const auto enrollment_open =
      host_connection.open_host_enrollment("installed-enrollment", std::chrono::milliseconds(1));
  pulp::inspect::ControlHostBootstrapRecord bootstrap;
  bootstrap.endpoint_path = "/tmp/not-connected-control.sock";
  bootstrap.expected_broker.evidence = {
      .role = pulp::inspect::ControlPeerRole::TrustedHostBridge,
      .user_id = "installed-user",
      .process_id = 1,
      .process_start_id = "installed-start",
      .executable_identity = "installed-executable",
      .publisher_id = "installed-publisher"};
  bootstrap.admission_id = "installed-admission";
  bootstrap.registration_id = pulp::inspect::ControlRegistrationId{"installed-registration"};
  bootstrap.expires_at_unix_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          (std::chrono::system_clock::now() + std::chrono::minutes(1)).time_since_epoch()).count();
  const auto bootstrap_bytes = pulp::inspect::encode_control_host_bootstrap(bootstrap);
  pulp::inspect::ControlHostBootstrapRecord enrollment_bootstrap;
  enrollment_bootstrap.endpoint_path = bootstrap.endpoint_path;
  enrollment_bootstrap.expected_broker = bootstrap.expected_broker;
  enrollment_bootstrap.enrollment_id = "installed-enrollment";
  enrollment_bootstrap.expires_at_unix_ms = bootstrap.expires_at_unix_ms;
  const auto enrollment_bootstrap_bytes =
      pulp::inspect::encode_control_host_bootstrap(enrollment_bootstrap);
  pulp::inspect::ControlConnectionPrincipal principal =
      pulp::inspect::ControlHostConnectionPrincipal{
          pulp::inspect::ControlRegistrationId{"installed-registration"}};
  auto rpc = std::make_shared<pulp::inspect::InspectorMainThreadRpc>();
  pulp::inspect::ControlMainThreadExecutor main_thread_executor{rpc, {}};
  auto trace = std::make_shared<pulp::inspect::TraceInspector>();
  auto trace_executor = pulp::inspect::ControlTraceSessionExecutor::create({
      .main_thread_rpc = rpc,
      .trace_inspector = trace,
      .registration_id = pulp::inspect::ControlRegistrationId{"installed-registration"},
  });
  const auto slot_installed =
      trace_executor && executor_slot.install(trace_executor->executor());
  pulp::inspect::ControlRequestEnvelope request;
  pulp::inspect::ControlAdmissionRequest admission;
  pulp::inspect::ControlOperationStoreConfig operations;
  pulp::inspect::ControlArtifactStoreConfig artifacts;
  pulp::inspect::ControlTrustedHostInventoryConfig inventory;
  pulp::platform::ProcessOptions process_options;
  process_options.max_standard_input_provider_bytes = 4096;
  pulp::platform::StandardInputByteProvider input_provider =
      [](int child_process_id) -> std::optional<std::vector<std::uint8_t>> {
    return std::vector<std::uint8_t>{
        static_cast<std::uint8_t>(child_process_id & 0xff)};
  };
  pulp::platform::StandardInputChannelSession input_session =
      [](int, pulp::platform::ChildProcessInputChannel) { return false; };
  pulp::inspect::ControlHostPreflightDiagnostics preflight_diagnostics;
  const auto artifact = control_client.read_artifact("artifact-installed", 0, 16);
  (void)main_thread_executor.executor();
  (void)input_session;
  (void)preflight_diagnostics;

  request.operation_version = 1;
  admission.operation_version = request.operation_version;
  return !broker.is_listening() && !service.is_listening()
             && !client.is_connected()
             && !host_connection.is_connected()
             && enrollment_open.error_code == "invalid-host-open"
             && slot_installed
             && trace_executor
             && std::holds_alternative<pulp::inspect::ControlHostConnectionPrincipal>(principal)
             && operations.max_receipts > 0
             && artifacts.maximum_blob_bytes > 0
             && inventory.maximum_entries > 0
             && !bootstrap_bytes.empty()
             && !enrollment_bootstrap_bytes.empty()
             && process_options.max_standard_input_provider_bytes == 4096
             && input_provider(7).has_value()
             && admission.operation_version == 1
             && artifact.status == pulp::inspect::ControlArtifactStatus::Read
             && artifact.metadata
             && artifact.metadata->lineage.producer_client_id == "installed-client"
         ? 0
         : 1;
}
]=])

set(_consumer_configure_args
    -S "${_consumer_source}"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _consumer_configure_args
        "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()
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
        "Control SDK consumer configure failed (${_configure_result})\n"
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
        "Control SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable_suffix "")
if(WIN32)
    set(_executable_suffix ".exe")
endif()
set(_executable
    "${_consumer_build}/pulp-control-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_executable}")
    set(_executable
        "${_consumer_build}/${_producer_config}/pulp-control-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_executable}" RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "Control SDK consumer exited ${_run_result}")
endif()
