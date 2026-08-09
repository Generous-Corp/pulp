#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>
#include <pulp/inspect/test_input.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

inline constexpr std::size_t kControlDevelopmentMaximumTextBytes = 4096;

struct ControlUiObservationRequest {
    std::string selector;
    bool include_geometry = true;
};

struct ControlUiObservation {
    std::string tree_json;
    std::uint64_t generation = 0;
    std::size_t node_count = 0;
};

enum class ControlDiagnosticSeverity : std::uint8_t { Info, Warning, Error };

struct ControlDiagnosticItem {
    std::string id;
    ControlDiagnosticSeverity severity = ControlDiagnosticSeverity::Info;
    std::string message;
};

struct ControlLogEntry {
    std::uint64_t sequence = 0;
    std::string level;
    std::string message;
};

struct ControlLogPage {
    std::vector<ControlLogEntry> entries;
    std::uint64_t next_sequence = 0;
};

struct ControlTestNoteInput {
    std::uint64_t sequence = 0;
    bool note_on = true;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    double velocity = 0.0;
};

struct ControlTestTransportInput {
    std::uint64_t sequence = 0;
    bool playing = false;
    double position_beats = 0.0;
    double tempo_bpm = 120.0;
};

struct ControlAuthoringChanges {
    std::optional<std::string> anchor_id;
    std::optional<bool> bypass;
    std::optional<bool> locked;
    std::vector<std::pair<std::string, double>> constants;
    std::optional<std::string> highlight_node_id;
    std::optional<bool> repaint_flash;
};

struct ControlAuthoringApplyResult {
    bool applied = false;
    std::uint64_t generation = 0;
    std::string explanation;
};

struct ControlHostDevelopmentBinding {
    ControlRegistration registration;
    ControlManifest manifest;
};

struct ControlHostDevelopmentExecutorConfig {
    ControlHostDevelopmentBinding binding;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::function<std::optional<ControlUiObservation>(const ControlUiObservationRequest&)>
        observe_ui;
    std::function<std::vector<ControlDiagnosticItem>()> read_diagnostics;
    std::function<ControlLogPage(std::uint64_t after_sequence, std::size_t limit)> read_logs;
    std::function<TestInputApplyResult(const ControlTestNoteInput&)> apply_test_note;
    std::function<TestInputApplyResult(const ControlTestTransportInput&)> apply_test_transport;
    std::function<void(TestInputReleaseReason)> release_test_input;
    std::function<ControlAuthoringApplyResult(const ControlAuthoringChanges&)> apply_authoring;
    std::chrono::milliseconds artifact_lifetime = std::chrono::minutes(15);
};

/// Canonical host-main composition for bounded development outcomes that do
/// not belong to state, capture, Motion, telemetry, or runtime evaluation.
/// Providers are typed and exact-instance-bound; no method name, path, socket,
/// or command is accepted from a client.
class ControlHostDevelopmentExecutor {
  public:
    static std::unique_ptr<ControlHostDevelopmentExecutor>
    create(ControlHostDevelopmentExecutorConfig config);
    ~ControlHostDevelopmentExecutor();

    ControlHostDevelopmentExecutor(const ControlHostDevelopmentExecutor&) = delete;
    ControlHostDevelopmentExecutor& operator=(const ControlHostDevelopmentExecutor&) = delete;

    ControlOperationExecutor executor() const;
    void end_authority(std::string_view opaque_authority_id,
                       TestInputReleaseReason reason) noexcept;
    void disconnect() noexcept;
    bool ready() const;

  private:
    struct State;
    explicit ControlHostDevelopmentExecutor(std::shared_ptr<State> state,
                                            ControlOperationExecutor executor);
    std::shared_ptr<State> state_;
    ControlOperationExecutor executor_;
};

} // namespace pulp::inspect
