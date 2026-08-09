#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::inspect {

class InspectorCaptureSource;
class InspectorMainThreadRpc;
class RuntimeEvaluator;

inline constexpr std::size_t kControlCaptureMaximumBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kControlRuntimeEvalMaximumResultBytes = 256u * 1024u;

struct ControlHostUiBinding {
    /// Exact broker-validated registration plus the manifest whose digest was
    /// retained in it. Construction fails if any immutable evidence disagrees.
    ControlRegistration registration;
    ControlManifest manifest;
};

using ControlRuntimeEvalRedactor =
    std::function<std::optional<std::string>(std::string_view result_json)>;

struct ControlHostUiExecutorConfig {
    ControlHostUiBinding binding;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::shared_ptr<InspectorCaptureSource> capture_source;
    std::shared_ptr<RuntimeEvaluator> runtime_evaluator;
    /// Required with runtime_evaluator. Returning nullopt fails closed.
    ControlRuntimeEvalRedactor redact_runtime_eval_result;
    std::size_t maximum_capture_bytes = kControlCaptureMaximumBytes;
    std::size_t maximum_eval_result_bytes = kControlRuntimeEvalMaximumResultBytes;
    std::chrono::milliseconds capture_lifetime = std::chrono::minutes(15);
};

/// Canonical exact-instance adapter for screenshot capture and explicitly
/// unsafe research evaluation. Both operations run through the existing
/// deadline-fenced Inspector main-thread RPC. UI input remains deliberately
/// unavailable until a host-owned exact-target input seam exists.
class ControlHostUiExecutor {
  public:
    static std::unique_ptr<ControlHostUiExecutor> create(ControlHostUiExecutorConfig config);
    ~ControlHostUiExecutor();

    ControlHostUiExecutor(const ControlHostUiExecutor&) = delete;
    ControlHostUiExecutor& operator=(const ControlHostUiExecutor&) = delete;

    ControlOperationExecutor executor() const;
    bool ready() const;
    void disconnect() noexcept;

  private:
    struct State;
    explicit ControlHostUiExecutor(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

/// Stable registry/documentation disposition for the frozen UI-input contract.
std::string_view control_ui_input_disposition() noexcept;

} // namespace pulp::inspect
