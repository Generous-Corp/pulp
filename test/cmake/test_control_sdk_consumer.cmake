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

if(APPLE)
    set(_installed_control_libexec "${_prefix}/libexec/pulp")
    foreach(_installed_control_file IN ITEMS
            pulp-control-broker
            pulp-control-standalone-host
            pulp-control-standalone-host.inspector-capabilities.json
            pulp-control-standalone-host.Standalone.control-shipping.json)
        if(NOT EXISTS "${_installed_control_libexec}/${_installed_control_file}")
            message(FATAL_ERROR
                "Installed broker/Standalone composition is missing: ${_installed_control_file}")
        endif()
    endforeach()
    execute_process(
        COMMAND /usr/bin/stat -f %Lp
                "${_installed_control_libexec}/pulp-control-standalone-host"
        OUTPUT_VARIABLE _installed_host_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND /usr/bin/stat -f %Lp
                "${_installed_control_libexec}/pulp-control-standalone-host.inspector-capabilities.json"
        OUTPUT_VARIABLE _installed_host_manifest_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _installed_host_mode STREQUAL "700" OR
       NOT _installed_host_manifest_mode STREQUAL "600")
        message(FATAL_ERROR
            "Installed Standalone authority is not owner-private: executable=${_installed_host_mode}, manifest=${_installed_host_manifest_mode}")
    endif()
endif()

set(_installed_examples "${_prefix}/share/pulp/capability-control")
foreach(_example IN ITEMS control-examples.json README.md cli-walkthrough.sh mcp-tools.jsonl)
    if(NOT EXISTS "${_installed_examples}/${_example}")
        message(FATAL_ERROR "Installed capability-control example is missing: ${_example}")
    endif()
endforeach()
file(READ "${_installed_examples}/cli-walkthrough.sh" _installed_cli_walkthrough)
if(_installed_cli_walkthrough MATCHES "(\\./build|tools/|planning/|--host|--port)")
    message(FATAL_ERROR
        "Installed capability-control walkthrough contains a source-tree/raw-selector command")
endif()

file(WRITE "${_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpControlSdkConsumer LANGUAGES CXX)

# The installed consumer exercises C++20 library facilities in every target,
# including the Standalone processor object library created in a subdirectory.
# Set the language contract at directory scope before importing Pulp so no
# provider or toolchain default can leave a generated target in C++17 mode.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Pulp REQUIRED COMPONENTS
    inspect-protocol
    inspect-control
    inspect-client
    inspect-runtime
    inspect-standalone-runtime
    inspect-ui-runtime)

if(NOT TARGET Pulp::inspect-standalone-runtime)
    message(FATAL_ERROR "Installed canonical Standalone adapter target is missing")
endif()

set(_control_targets
    Pulp::inspect-protocol
    Pulp::inspect-control
    Pulp::inspect-client
    Pulp::inspect-runtime)
if(NOT TARGET Pulp::inspect-ui-runtime)
    message(FATAL_ERROR "Installed control SDK target is missing: Pulp::inspect-ui-runtime")
endif()
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
add_subdirectory(standalone-consumer)
]=])

file(WRITE "${_consumer_source}/main.cpp" [=[
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/control_admission.hpp>
#include <pulp/inspect/control_artifacts.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_client.hpp>
#include <pulp/inspect/control_inspector_client.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_executor_slot.hpp>
#include <pulp/inspect/control_host_enrollment.hpp>
#include <pulp/inspect/control_host_connection.hpp>
#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_host_router.hpp>
#include <pulp/inspect/control_host_ui_executor.hpp>
#include <pulp/inspect/control_standalone_ui_adapter.hpp>
#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_trace_session_executor.hpp>
#include <pulp/inspect/control_operations.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/inspect/control_service.hpp>
#include <pulp/inspect/control_trusted_host_inventory.hpp>
#include <pulp/inspect/control_trusted_host_launcher.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
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

class InstalledControlSessionOpener final
    : public pulp::inspect::InspectorControlSessionOpener {
 public:
  std::optional<pulp::inspect::InspectorControlSession> open(
      std::chrono::milliseconds) override {
    return std::nullopt;
  }
};

int main() {
  pulp::inspect::ControlBroker broker;
  pulp::inspect::ControlHostEnrollmentStore enrollments;
  InstalledControlTransport transport;
  InstalledControlSessionOpener session_opener;
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
  auto* ui_executor_type = static_cast<pulp::inspect::ControlHostUiExecutor*>(nullptr);
  auto* standalone_ui_adapter_type =
      static_cast<pulp::inspect::ControlStandaloneUiAdapter*>(nullptr);
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
  auto* launcher_type = static_cast<pulp::inspect::ControlTrustedHostLauncher*>(nullptr);
  pulp::platform::ProcessOptions process_options;
  process_options.max_standard_input_provider_bytes = 4096;
  pulp::platform::StandardInputByteProvider input_provider =
      [](int child_process_id) -> std::optional<std::vector<std::uint8_t>> {
    return std::vector<std::uint8_t>{
        static_cast<std::uint8_t>(child_process_id & 0xff)};
  };
  pulp::platform::StandardInputChannelSession input_session =
      [](int, pulp::platform::ChildProcessInputChannel,
         std::chrono::steady_clock::time_point) { return false; };
  pulp::inspect::ControlHostPreflightDiagnostics preflight_diagnostics;
  const auto artifact = control_client.read_artifact("artifact-installed", 0, 16);
  const pulp::inspect::ControlLegacyInspectorError legacy_error{
      .error_code = "installed_error",
      .error_message = "installed compatibility error",
      .error_data_json = R"({"installed":true})",
  };
  const auto encoded_legacy_error =
      pulp::inspect::encode_control_legacy_inspector_error(legacy_error);
  const auto decoded_legacy_error = encoded_legacy_error
      ? pulp::inspect::decode_control_legacy_inspector_error(*encoded_legacy_error)
      : std::nullopt;
  const auto unsupported = pulp::inspect::request_control_inspector(
      session_opener, "DOM.getDocument");
  (void)main_thread_executor.executor();
  (void)input_session;
  (void)preflight_diagnostics;

  request.operation_version = 1;
  admission.operation_version = request.operation_version;
  return !broker.is_listening() && !service.is_listening()
             && !host_connection.is_connected()
             && enrollment_open.error_code == "invalid-host-open"
             && slot_installed
             && trace_executor
             && decoded_legacy_error == legacy_error
             && unsupported.response.error_code == "method_not_found"
             && std::holds_alternative<pulp::inspect::ControlHostConnectionPrincipal>(principal)
             && operations.max_receipts > 0
             && artifacts.maximum_blob_bytes > 0
             && inventory.maximum_entries > 0
             && launcher_type == nullptr
             && ui_executor_type == nullptr
             && standalone_ui_adapter_type == nullptr
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

file(MAKE_DIRECTORY "${_consumer_source}/standalone-consumer")
file(WRITE "${_consumer_source}/standalone-consumer/ui.js" [=[
createLabel('author-label', 'Installed parity', '');
setInterval(function () { console.log('installed-parity-live-log'); }, 50);
]=])
file(WRITE "${_consumer_source}/standalone-consumer/CMakeLists.txt" [=[
pulp_add_plugin(InstalledControlStandalone
    PLUGIN_NAME "Installed Control Standalone"
    BUNDLE_ID dev.pulp.installed-control-standalone
    FORMATS Standalone
    SOURCES standalone.cpp
    PROCESSOR_FACTORY create_processor
    CONTROL_PROFILE research-unsafe
    ACKNOWLEDGE_UNSAFE_RUNTIME_EVAL
    CONTROL_CAPABILITIES
        dev.pulp.instance/read@1
        dev.pulp.session/control@1
        dev.pulp.state/read@1
        dev.pulp.ui/observe@1
        dev.pulp.diagnostics/read@1
        dev.pulp.logs/read@1
        dev.pulp.ui/capture@1
        dev.pulp.ui/input@1
        dev.pulp.trace/control@1
        dev.pulp.trace/session-control@1
        dev.pulp.state/parameter-gesture@1
        dev.pulp.test/input@1
        dev.pulp.authoring/tweaks@1
        dev.pulp.telemetry/subscribe@1
        dev.pulp.runtime/evaluate@1)
target_compile_definitions(InstalledControlStandalone_Core PRIVATE
    INSTALLED_PARITY_UI_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/ui.js")
set_target_properties(InstalledControlStandalone_Core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_features(InstalledControlStandalone_Core PRIVATE cxx_std_20)
# Keep the language mode visible in the generated child command. Directory and
# target standard metadata have both regressed independently in hosted SDK
# consumer builds, so this fixture verifies the compiler invocation below.
target_compile_options(InstalledControlStandalone_Core PRIVATE -std=c++20)
]=])

file(WRITE "${_consumer_source}/standalone-consumer/standalone.cpp" [=[
#include <pulp/format/processor.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/inspect/control_telemetry_tap.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/motion_cost.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

static_assert(__cplusplus >= 202002L,
              "installed Standalone consumer must compile as C++20");

class InstalledParityInputNode final : public pulp::view::View {
 public:
  void paint(pulp::canvas::Canvas& canvas) override {
    canvas.set_fill_color(pulp::canvas::Color::rgba8(32, 96, 180));
    canvas.fill_rect(0, 0, bounds().width, bounds().height);
    canvas.set_fill_color(pulp::canvas::Color::rgba8(240, 180, 40));
    canvas.fill_rect(8, 8, bounds().width - 16, bounds().height - 16);
  }
  bool wants_mouse_input() const override { return true; }
  bool accepts_text_input() const override { return true; }
  void on_mouse_down(pulp::view::Point) override {
    ++pointer_down_count;
    const auto current = bounds();
    set_bounds({current.x, current.y,
                current.width == 120.0f ? 124.0f : 120.0f, current.height});
  }
  void on_mouse_up(pulp::view::Point) override { ++pointer_up_count; }
  bool on_key_event(const pulp::view::KeyEvent&) override {
    ++key_count;
    return true;
  }
  void on_text_input(const pulp::view::TextInputEvent& event) override {
    text += event.text;
  }

  unsigned pointer_down_count = 0;
  unsigned pointer_up_count = 0;
  unsigned key_count = 0;
  std::string text;
};

class InstalledParityRoot final : public pulp::view::View {
 public:
  void layout_children() override {}
};

class InstalledStandaloneProcessor final : public pulp::format::Processor {
 public:
  InstalledStandaloneProcessor() {
    telemetry_level_ = channels_.declare_scalar("author-level", "normalized", 0.25f);
    telemetry_thread_ = std::jthread([this](std::stop_token stop) {
      float value = 0.0f;
      while (!stop.stop_requested()) {
        if (telemetry_level_)
          telemetry_level_->publish(value);
        value = value >= 1.0f ? 0.0f : value + 0.01f;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  ~InstalledStandaloneProcessor() override = default;

  pulp::format::PluginDescriptor descriptor() const override {
    return {.name = "Installed Control Standalone",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.installed-control-standalone",
            .version = "1.0.0",
            .input_buses = {{"Input", 2}},
            .output_buses = {{"Output", 2}}};
  }
  void define_parameters(pulp::state::StateStore& store) override {
    store.add_parameter({.id = 1,
                         .name = "Author Level",
                         .range = {.min = 0.0f, .max = 1.0f,
                                   .default_value = 0.25f}});
  }
  void prepare(const pulp::format::PrepareContext&) override {}
  pulp::view::ScriptedUiSession* active_scripted_ui() override {
    return scripted_session_.get();
  }
  const pulp::view::ScriptedUiSession* active_scripted_ui() const override {
    return scripted_session_.get();
  }
  std::unique_ptr<pulp::view::View> create_view() override {
    auto root = std::make_unique<InstalledParityRoot>();
    root->set_id("author-window");
    root->set_bounds({0, 0, 320, 180});
    scripted_session_ = std::make_unique<pulp::view::ScriptedUiSession>(
        *root, state(), pulp::view::ScriptedUiOptions{
            .script_path = INSTALLED_PARITY_UI_SCRIPT});
    std::string script_error;
    if (!scripted_session_->load(&script_error))
      return nullptr;
    auto input = std::make_unique<InstalledParityInputNode>();
    input->set_id("author-input");
    input->set_bounds({20, 20, 120, 48});
    input->set_focusable(true);
    root->add_child(std::move(input));
    auto scroll = std::make_unique<pulp::view::ScrollView>();
    scroll->set_id("author-scroll");
    scroll->set_bounds({20, 84, 280, 76});
    root->add_child(std::move(scroll));
    return root;
  }
  void on_view_closed(pulp::view::View&) override { scripted_session_.reset(); }
  pulp::view::ValueChannelSet* value_channels() override { return &channels_; }
  void process(pulp::audio::BufferView<float>& output,
               const pulp::audio::BufferView<const float>& input,
               pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
               const pulp::format::ProcessContext& context) override {
    for (const auto& event : midi_in) {
      if (event.is_note_on()) {
        observed_note_.store(event.note(), std::memory_order_relaxed);
        observed_channel_.store(event.channel(), std::memory_order_relaxed);
        observed_velocity_.store(event.velocity(), std::memory_order_relaxed);
      }
    }
    observed_playing_.store(context.is_playing, std::memory_order_relaxed);
    observed_position_.store(context.position_samples, std::memory_order_relaxed);
    const auto channels = std::min(output.num_channels(), input.num_channels());
    const auto samples = std::min(output.num_samples(), input.num_samples());
    for (std::size_t channel = 0; channel < channels; ++channel)
      std::copy_n(input.channel(channel).data(), samples,
                  output.channel(channel).data());
    if (telemetry_level_)
      telemetry_level_->publish(0.25f);
  }

  std::string test_input_diagnostics() const {
    std::ostringstream message;
    message << "note_on=" << observed_note_.load(std::memory_order_relaxed)
            << ", channel=" << observed_channel_.load(std::memory_order_relaxed)
            << ", velocity=" << observed_velocity_.load(std::memory_order_relaxed)
            << ", transport_playing="
            << (observed_playing_.load(std::memory_order_relaxed) ? "true" : "false")
            << ", transport_at_or_after_eight_beats="
            << (observed_position_.load(std::memory_order_relaxed) >= 192000 ? "true" : "false");
    return message.str();
  }

 private:
  pulp::view::ValueChannelSet channels_;
  pulp::view::ScalarSource* telemetry_level_ = nullptr;
  std::unique_ptr<pulp::view::ScriptedUiSession> scripted_session_;
  std::jthread telemetry_thread_;
  std::atomic<int> observed_note_{-1};
  std::atomic<int> observed_channel_{-1};
  std::atomic<int> observed_velocity_{-1};
  std::atomic<bool> observed_playing_{false};
  std::atomic<std::int64_t> observed_position_{-1};
};

pulp::inspect::detail::StandaloneControlAuthorHooks installed_parity_control_hooks(
    pulp::format::Processor& processor) {
  struct AuthoringState {
    std::uint64_t generation = 0;
    double gain = 0.0;
    std::string highlight;
    bool repaint_flash = false;
  };
  static AuthoringState authoring;
  auto* installed = dynamic_cast<InstalledStandaloneProcessor*>(&processor);
  const auto fixture = std::filesystem::temp_directory_path() /
                       "pulp-installed-author-motion-fixture.jsonl";
  const auto sink = pulp::view::motion::make_fixture_sink(fixture);
  sink({.kind = pulp::view::motion::SampleEvent::Kind::Baseline,
        .view_name = "author-input", .metric_name = "geometry", .frame = 0});
  sink({.kind = pulp::view::motion::SampleEvent::Kind::Sample,
        .view_name = "author-input", .metric_name = "geometry", .frame = 1});
  sink({.kind = pulp::view::motion::SampleEvent::Kind::End,
        .view_name = "author-input", .metric_name = "geometry", .frame = 2});
  return {
      .telemetry_classifier = [](std::string_view channel) {
        return channel == "author-level"
                   ? pulp::inspect::ControlTelemetrySensitivity::Observable
                   : pulp::inspect::ControlTelemetrySensitivity::Sensitive;
      },
      .motion_cost_probe = [] {
        return pulp::view::motion::RenderCostSnapshot{0.125, 1.0, 1};
      },
      .motion_fixture_path = fixture,
      .diagnostics = [installed] {
        std::ostringstream message;
        message << "generation=" << authoring.generation
                << ", gain=" << authoring.gain
                << ", highlight=" << authoring.highlight
                << ", repaint_flash=" << (authoring.repaint_flash ? "true" : "false");
        std::vector<pulp::inspect::ControlDiagnosticItem> items{{
            .id = "author.parity-state",
            .severity = pulp::inspect::ControlDiagnosticSeverity::Info,
            .message = message.str()}};
        if (installed) {
          items.push_back({
              .id = "author.test-input-consumed",
              .severity = pulp::inspect::ControlDiagnosticSeverity::Info,
              .message = installed->test_input_diagnostics()});
        }
        return items;
      },
      .apply_authoring = [](const pulp::inspect::ControlAuthoringChanges& changes) {
        const auto gain = std::ranges::find_if(changes.constants, [](const auto& value) {
          return value.first == "gain";
        });
        if (gain == changes.constants.end() ||
            changes.highlight_node_id != std::optional<std::string>{"author-input"} ||
            changes.repaint_flash != std::optional<bool>{true}) {
          return pulp::inspect::ControlAuthoringApplyResult{
              .explanation = "fixture expected gain, author-input highlight, and repaint flash"};
        }
        authoring.gain = gain->second;
        authoring.highlight = *changes.highlight_node_id;
        authoring.repaint_flash = *changes.repaint_flash;
        return pulp::inspect::ControlAuthoringApplyResult{
            .applied = true, .generation = ++authoring.generation};
      },
  };
}

std::unique_ptr<pulp::format::Processor> create_processor() {
  static const bool hooks_registered =
      pulp::inspect::detail::install_standalone_control_author_hooks_factory(
          &installed_parity_control_hooks);
  if (!hooks_registered)
    return {};
  return std::make_unique<InstalledStandaloneProcessor>();
}
]=])

file(WRITE "${_consumer_source}/standalone-consumer/main.cpp" [=[
#include <pulp/format/standalone.hpp>

#include <memory>

std::unique_ptr<pulp::format::Processor> create_processor();

int main() {
  pulp::format::StandaloneApp app(&create_processor);
  return app.start() ? 0 : 1;
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

set(_consumer_compile_database "${_consumer_build}/compile_commands.json")
if(NOT EXISTS "${_consumer_compile_database}")
    message(FATAL_ERROR
        "Control SDK consumer did not export compile_commands.json")
endif()
file(READ "${_consumer_compile_database}" _consumer_compile_commands)
string(JSON _consumer_compile_count LENGTH "${_consumer_compile_commands}")
if(_consumer_compile_count EQUAL 0)
    message(FATAL_ERROR "Control SDK consumer compilation database is empty")
endif()
math(EXPR _consumer_compile_last "${_consumer_compile_count} - 1")
set(_standalone_compile_entry_found OFF)
foreach(_consumer_compile_index RANGE 0 ${_consumer_compile_last})
    string(JSON _consumer_compile_file GET
        "${_consumer_compile_commands}" ${_consumer_compile_index} file)
    if(_consumer_compile_file STREQUAL
       "${_consumer_source}/standalone-consumer/standalone.cpp")
        set(_standalone_compile_entry_found ON)
        string(JSON _standalone_compile_command GET
            "${_consumer_compile_commands}" ${_consumer_compile_index} command)
        string(REGEX MATCHALL "(^|[ \t])-std=[^ \t]+"
            _standalone_language_modes "${_standalone_compile_command}")
        if(NOT _standalone_language_modes)
            message(FATAL_ERROR
                "InstalledControlStandalone_Core lacks explicit C++20 mode:\n"
                "${_standalone_compile_command}")
        endif()
        list(GET _standalone_language_modes -1 _standalone_effective_language_mode)
        string(STRIP "${_standalone_effective_language_mode}"
            _standalone_effective_language_mode)
        if(NOT _standalone_effective_language_mode MATCHES
           "^-std=(c\\+\\+|gnu\\+\\+)20$")
            message(FATAL_ERROR
                "InstalledControlStandalone_Core ends in ${_standalone_effective_language_mode}, not C++20:\n"
                "${_standalone_compile_command}")
        endif()
    endif()
endforeach()
if(NOT _standalone_compile_entry_found)
    message(FATAL_ERROR
        "Control SDK consumer compilation database lacks standalone.cpp")
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

set(_consumer_install_prefix "${_fixture_root}/author-product-prefix")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${_consumer_build}"
            --prefix "${_consumer_install_prefix}" --config "${_producer_config}"
    RESULT_VARIABLE _consumer_install_result
    OUTPUT_VARIABLE _consumer_install_output
    ERROR_VARIABLE _consumer_install_error)
if(NOT _consumer_install_result EQUAL 0)
    message(FATAL_ERROR
        "Control SDK author-host install failed (${_consumer_install_result})\n"
        "${_consumer_install_output}\n${_consumer_install_error}")
endif()
if(APPLE)
    set(_author_host_directory
        "${_prefix}/libexec/pulp/control-hosts/dev-pulp-installed-control-standalone-18f9d0d67fc6aec8")
    file(READ "${_author_host_directory}/active" _author_host_version)
    string(STRIP "${_author_host_version}" _author_host_version)
    set(_author_host_directory "${_author_host_directory}/${_author_host_version}")
    foreach(_author_file IN ITEMS host host.inspector-capabilities.json
                                  libwgpu_native.dylib runtime-closure.sha256)
        if(NOT EXISTS "${_author_host_directory}/${_author_file}")
            message(FATAL_ERROR
                "Installed author Standalone catalog entry is missing ${_author_file}")
        endif()
    endforeach()
    execute_process(COMMAND /usr/bin/stat -f %Lp "${_author_host_directory}"
        OUTPUT_VARIABLE _author_dir_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND /usr/bin/stat -f %Lp "${_author_host_directory}/host"
        OUTPUT_VARIABLE _author_host_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND /usr/bin/stat -f %Lp
        "${_author_host_directory}/host.inspector-capabilities.json"
        OUTPUT_VARIABLE _author_manifest_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND /usr/bin/stat -f %Lp
        "${_author_host_directory}/libwgpu_native.dylib"
        OUTPUT_VARIABLE _author_runtime_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _author_dir_mode STREQUAL "700" OR
       NOT _author_host_mode STREQUAL "700" OR
       NOT _author_manifest_mode STREQUAL "600" OR
       NOT _author_runtime_mode STREQUAL "700")
        message(FATAL_ERROR
            "Installed author catalog entry is not owner-private: ${_author_dir_mode}/${_author_host_mode}/${_author_manifest_mode}/${_author_runtime_mode}")
    endif()
    set(_destdir "${_fixture_root}/package-stage")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${_destdir}"
            "${CMAKE_COMMAND}" --install "${_consumer_build}"
            --prefix "${_consumer_install_prefix}" --config "${_producer_config}"
        RESULT_VARIABLE _destdir_install_result
        OUTPUT_VARIABLE _destdir_install_output
        ERROR_VARIABLE _destdir_install_error)
    if(NOT _destdir_install_result EQUAL 0 OR
       NOT EXISTS "${_destdir}${_author_host_directory}/host" OR
       NOT EXISTS "${_destdir}${_author_host_directory}/host.inspector-capabilities.json" OR
       NOT EXISTS "${_destdir}${_author_host_directory}/libwgpu_native.dylib" OR
       NOT EXISTS "${_destdir}${_author_host_directory}/runtime-closure.sha256")
        message(FATAL_ERROR
            "DESTDIR author catalog staging failed (${_destdir_install_result})\n${_destdir_install_output}\n${_destdir_install_error}")
    endif()
    set(_daemon_test "${PULP_BUILD_DIR}/test/pulp-test-control-broker-daemon")
    if(NOT EXISTS "${_daemon_test}")
        message(FATAL_ERROR "Author catalog E2E test binary is missing: ${_daemon_test}")
    endif()
    # Production enrollment trusts only broker-owned installed CLI/MCP
    # identities. Install this signed test client at the canonical CLI sibling
    # path so the E2E exercises that policy instead of adding a test bypass.
    file(COPY_FILE "${_daemon_test}" "${_prefix}/libexec/pulp/pulp")
    file(CHMOD "${_prefix}/libexec/pulp/pulp"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "PULP_CONTROL_AUTHOR_BROKER=${_prefix}/libexec/pulp/pulp-control-broker"
            "PULP_CONTROL_AUTHOR_HOST=${_author_host_directory}/host"
            "${_daemon_test}" "[author-catalog-process]"
        RESULT_VARIABLE _author_process_result
        OUTPUT_VARIABLE _author_process_output
        ERROR_VARIABLE _author_process_error)
    if(NOT _author_process_result EQUAL 0)
        message(FATAL_ERROR
            "Installed production broker did not launch/register the author host (${_author_process_result})\n${_author_process_output}\n${_author_process_error}")
    endif()
    # The aggregate embeds the production daemon implementation so it can
    # inject deterministic explicit consent. Keep its trusted client identities
    # in this test's private fixture instead of overwriting canonical build-tree
    # outputs that parallel CTest workers may be executing or rebuilding.
    set(_author_parity_directory "${_fixture_root}/installed-author-parity")
    file(MAKE_DIRECTORY "${_author_parity_directory}")
    file(CHMOD "${_author_parity_directory}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    set(_author_parity_broker
        "${_author_parity_directory}/pulp-control-broker")
    file(COPY_FILE "${_daemon_test}" "${_author_parity_broker}")
    file(CHMOD "${_author_parity_broker}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    foreach(_installed_client IN ITEMS pulp-cpp pulp-mcp)
        file(COPY_FILE "${_prefix}/bin/${_installed_client}"
            "${_author_parity_directory}/${_installed_client}")
        file(CHMOD "${_author_parity_directory}/${_installed_client}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endforeach()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "PULP_CONTROL_AUTHOR_HOST=${_author_host_directory}/host"
            "PULP_CONTROL_AUTHOR_PARITY_ROOT=${_author_parity_directory}"
            "PULP_CONTROL_AUTHOR_SHARED_TEST_ROOT=${PULP_BUILD_DIR}/test"
            "PULP_CONTROL_AUTHOR_CLI=${_author_parity_directory}/pulp-cpp"
            "PULP_CONTROL_AUTHOR_MCP=${_author_parity_directory}/pulp-mcp"
            "${_author_parity_broker}" "[installed-author-full-parity]"
        RESULT_VARIABLE _author_state_result
        OUTPUT_VARIABLE _author_state_output
        ERROR_VARIABLE _author_state_error)
    if(NOT _author_state_result EQUAL 0)
        message(FATAL_ERROR
            "Installed author host typed state-read E2E failed (${_author_state_result})\n${_author_state_output}\n${_author_state_error}")
    endif()
endif()

file(GLOB_RECURSE _standalone_control_manifests
    "${_consumer_build}/*InstalledControlStandalone*.inspector-capabilities.json")
list(LENGTH _standalone_control_manifests _standalone_manifest_count)
if(NOT _standalone_manifest_count EQUAL 2)
    message(FATAL_ERROR
        "Installed ordinary Standalone and its companion did not emit two control manifests: ${_standalone_control_manifests}")
endif()
foreach(_standalone_control_manifest IN LISTS _standalone_control_manifests)
    file(READ "${_standalone_control_manifest}" _standalone_control_json)
    if(NOT _standalone_control_json MATCHES "\"endpoint_included\"[ \\t]*:[ \\t]*true" OR
       NOT _standalone_control_json MATCHES "\"profile\"[ \\t]*:[ \\t]*\"research-unsafe\"" OR
       NOT _standalone_control_json MATCHES "\"unsafe_runtime_eval_acknowledged\"[ \\t]*:[ \\t]*true" OR
       NOT _standalone_control_json MATCHES "dev.pulp.instance/read@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.session/control@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.state/read@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.ui/observe@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.diagnostics/read@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.logs/read@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.ui/capture@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.ui/input@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.trace/control@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.trace/session-control@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.state/parameter-gesture@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.test/input@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.authoring/tweaks@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.telemetry/subscribe@1" OR
       NOT _standalone_control_json MATCHES "dev.pulp.runtime/evaluate@1")
        message(FATAL_ERROR
            "Installed ordinary Standalone control manifest is not truthful: ${_standalone_control_json}")
    endif()
endforeach()

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
