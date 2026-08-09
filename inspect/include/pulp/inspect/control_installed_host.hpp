#pragma once

#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_development_executor.hpp>
#include <pulp/inspect/control_host_observability_bundle.hpp>
#include <pulp/inspect/control_host_ui_executor.hpp>
#include <pulp/inspect/control_motion_executor.hpp>
#include <pulp/inspect/control_trace_session_executor.hpp>

#include <chrono>
#include <memory>

namespace pulp::inspect {

class MotionInspector;
class MotionScrubber;

struct ControlInstalledHostUiTargets {
    std::shared_ptr<InspectorCaptureSource> capture_source;
    std::shared_ptr<ControlHostUiTargetAdapter> target_adapter;
    std::string view_generation;
};

struct ControlInstalledHostUiConfig {
    ControlManifest manifest;
    /// Runs after the broker returns the exact binding and before executor
    /// installation/ready publication, so adapters never self-bind or race the
    /// first dispatch.
    std::function<std::optional<ControlInstalledHostUiTargets>(
        const ControlHostOpenResult&)>
        make_targets;
    std::shared_ptr<RuntimeEvaluator> runtime_evaluator;
    ControlRuntimeEvalRedactor redact_runtime_eval_result;
};

struct ControlInstalledHostDevelopmentConfig {
    ControlManifest manifest;
    std::function<std::optional<ControlUiObservation>(const ControlUiObservationRequest&)>
        observe_ui;
    std::function<std::vector<ControlDiagnosticItem>()> read_diagnostics;
    std::function<ControlLogPage(std::uint64_t after_sequence, std::size_t limit)> read_logs;
    std::function<TestInputApplyResult(const ControlTestNoteInput&)> apply_test_note;
    std::function<TestInputApplyResult(const ControlTestTransportInput&)> apply_test_transport;
    std::function<void(TestInputReleaseReason)> release_test_input;
    std::function<ControlAuthoringApplyResult(const ControlAuthoringChanges&)> apply_authoring;
};

struct ControlInstalledHostConfig {
    ControlHostBootstrapRecord bootstrap;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::shared_ptr<TraceInspector> trace_inspector;
    std::shared_ptr<ControlTelemetryTap> telemetry;
    MotionInspector* motion_inspector = nullptr;
    MotionScrubber* motion_scrubber = nullptr;
    std::optional<ControlInstalledHostUiConfig> ui;
    std::optional<ControlInstalledHostDevelopmentConfig> development;
    /// Additional typed host executor (for example canonical StateStore
    /// read/write composition). It is reached only after exact binding and
    /// opaque projected-authority validation, and is installed before ready.
    ControlOperationExecutor host_executor;
    std::chrono::milliseconds heartbeat_interval = std::chrono::seconds(5);
    std::chrono::milliseconds heartbeat_ttl = std::chrono::seconds(30);
    std::chrono::milliseconds handshake_timeout = std::chrono::seconds(3);
};

/// Production ownership seam for one broker-launched installed T1 host.
///
/// It consumes only inherited enrollment authority, installs the integrated
/// observability+Motion executor before acknowledging readiness, and then owns
/// the authenticated carrier, heartbeat lease, opaque authority lifecycle, and
/// restart/disconnect cleanup. It creates no listener or discovery surface.
class ControlInstalledHost {
  public:
    static std::unique_ptr<ControlInstalledHost> start(ControlInstalledHostConfig config);
    ~ControlInstalledHost();

    ControlInstalledHost(const ControlInstalledHost&) = delete;
    ControlInstalledHost& operator=(const ControlInstalledHost&) = delete;

    bool ready() const;
    const ControlHostOpenResult& binding() const;
    void stop() noexcept;

  private:
    struct State;
    explicit ControlInstalledHost(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace pulp::inspect
