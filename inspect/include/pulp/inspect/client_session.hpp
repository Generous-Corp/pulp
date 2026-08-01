#pragma once

#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/test_input.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pulp::inspect {

/// Exact or narrowing selectors accepted by every live inspector client.
struct InspectorClientSelection {
    std::string host;
    int port = 0;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
};

/// Stable structured failure returned before an inspector request is sent.
struct InspectorClientFailure {
    std::string code;
    std::string message;
    std::string data_json = "{}";
};

template <typename Value> struct InspectorClientResult {
    std::optional<Value> value;
    InspectorClientFailure failure;
    std::int64_t request_id = 0;
    std::string response_json;

    explicit operator bool() const noexcept {
        return value.has_value();
    }
};

struct InspectorCapabilitiesResult {
    std::string session_id;
    std::string instance_id;
    std::string plugin_id;
    std::string protocol_version;
    InspectorProfile profile = InspectorProfile::Off;
    std::vector<InspectorCapability> available;
    std::vector<InspectorCapability> effective;
    std::optional<std::string> controller;
};

struct InspectorAgentContextResult {
    std::string binary_path;
    std::string binary_build_id;
    std::int64_t binary_mtime_unix_ms = 0;
    std::string plugin_id;
    std::string session_id;
    std::string instance_id;
    bool editor_open = false;
    bool window_visible = false;
    bool processing = false;
    std::uint64_t xrun_count = 0;
    bool hot_reload_available = false;
    bool hot_reload_enabled = false;
    bool hot_reload_pending = false;
    std::uint64_t unsaved_tweak_count = 0;
    std::vector<std::string> actionable_issues;
};

struct InspectorParameterSnapshot {
    std::uint32_t id = 0;
    std::string name;
    std::string unit;
    double value = 0.0;
    double normalized = 0.0;
    double modulated = 0.0;
    double default_value = 0.0;
    double min = 0.0;
    double max = 0.0;
    std::optional<std::string> display;
};

struct InspectorSetParameterResult {
    bool applied = false;
};

struct InspectorInjectMidiResult {
    bool accepted = false;
};

struct InspectorSetTransportResult {
    bool applied = false;
};

struct InspectorScreenshotResult {
    std::string mime_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string data_base64;
};

/// Discover owner-private live publications and apply the supplied filters.
/// Invalid selector shapes fail closed and return an empty list.
std::vector<InspectorDiscoveryRecord> discover_inspector_sessions(
    const InspectorClientSelection& selection, InspectorClientFailure* failure = nullptr,
    std::filesystem::path runtime_directory = default_inspector_runtime_directory());

/// Selected authenticated client shared by CLI and MCP frontends.
class InspectorClientSession {
  public:
    using EventHandler = InspectorClient::EventHandler;

    static std::unique_ptr<InspectorClientSession>
    connect(const InspectorClientSelection& selection, InspectorClientFailure* failure = nullptr,
            std::chrono::milliseconds timeout = std::chrono::seconds(3),
            std::filesystem::path runtime_directory = default_inspector_runtime_directory());

    ~InspectorClientSession();
    InspectorClientSession(const InspectorClientSession&) = delete;
    InspectorClientSession& operator=(const InspectorClientSession&) = delete;

    const InspectorDiscoveryRecord& record() const {
        return record_;
    }
    InspectorMessage request(std::string method, std::string params_json = "{}",
                             std::chrono::milliseconds timeout = std::chrono::seconds(3));
    void set_event_handler(EventHandler handler);

    InspectorMessage capabilities(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage agent_context(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage parameters(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage set_parameter(std::int64_t parameter_id, double value, bool normalized = false,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage screenshot(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage inject_midi(const MidiTestInput& input,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorMessage set_transport(const StandaloneTransportTestInput& input,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(3));

    InspectorClientResult<InspectorCapabilitiesResult>
    read_capabilities(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<InspectorAgentContextResult>
    read_agent_context(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<std::vector<InspectorParameterSnapshot>>
    read_parameters(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<InspectorSetParameterResult>
    set_parameter_typed(std::int64_t parameter_id, double value, bool normalized = false,
                        std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<InspectorScreenshotResult>
    capture_screenshot(std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<InspectorInjectMidiResult>
    inject_midi_typed(const MidiTestInput& input,
                      std::chrono::milliseconds timeout = std::chrono::seconds(3));
    InspectorClientResult<InspectorSetTransportResult>
    set_transport_typed(const StandaloneTransportTestInput& input,
                        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    /// Acquire and release the controller lease around one typed mutation.
    InspectorMessage
    request_controlled(std::string method, std::string params_json = "{}",
                       std::chrono::milliseconds timeout = std::chrono::seconds(3));

  private:
    InspectorClientSession(InspectorDiscoveryReader discovery, InspectorDiscoveryRecord record);

    InspectorDiscoveryReader discovery_;
    InspectorDiscoveryRecord record_;
    InspectorClient client_;
    std::timed_mutex control_mutex_;
};

} // namespace pulp::inspect
