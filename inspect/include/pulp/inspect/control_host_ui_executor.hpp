#pragma once

#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace pulp::inspect {

class InspectorMainThreadRpc;
class RuntimeEvaluator;

inline constexpr std::size_t kControlCaptureMaximumBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kControlRuntimeEvalMaximumResultBytes = 256u * 1024u;
inline constexpr std::size_t kControlUiMaximumTargetBytes = 256;
inline constexpr std::size_t kControlUiMaximumViewGenerationBytes = 128;
inline constexpr std::size_t kControlUiMaximumKeyBytes = 64;
inline constexpr std::size_t kControlUiMaximumTextBytes = 4096;
inline constexpr double kControlUiMaximumCoordinate = 1'000'000.0;

/// A node name is never authority by itself. The host adapter receives the
/// complete immutable identity rooted in the broker registration and the
/// currently attached view generation on every call.
struct ControlUiExactTarget {
    std::string instance_id;
    std::string instance_generation;
    std::string view_generation;
    std::string node_id;
};

/// Exact broker authority owner for any UI state retained between receipts.
/// The canonical control plane will replace this projection with its opaque
/// controller-lease binding when that authority-end subscription lands.
struct ControlUiAuthorityOwner {
    std::string client_id;
    std::string grant_id;
    std::string client_principal;

    friend bool operator==(const ControlUiAuthorityOwner&,
                           const ControlUiAuthorityOwner&) = default;
};

struct ControlUiPointerInput {
    enum class Phase : std::uint8_t { Down, Move, Up };
    Phase phase = Phase::Down;
    double x = 0.0;
    double y = 0.0;
    std::uint8_t button = 0;
};

struct ControlUiKeyboardInput {
    enum class Phase : std::uint8_t { Down, Up };
    Phase phase = Phase::Down;
    std::string key;
    bool repeat = false;
};

struct ControlUiFocusInput {
    bool focused = false;
};

struct ControlUiTextInput {
    std::string text;
};

using ControlUiInput = std::variant<ControlUiPointerInput, ControlUiKeyboardInput,
                                    ControlUiFocusInput, ControlUiTextInput>;

enum class ControlUiApplyStatus : std::uint8_t {
    Applied,
    TargetUnavailable,
    StaleGeneration,
    InvalidEvent,
};

/// Pulp-owned composition seam for an ordinary Standalone view tree. A host
/// implementation consumes the existing capture, pointer, focus, key, and text
/// dispatch APIs; protocol code never receives a raw View/WindowHost pointer or
/// an arbitrary method name.
class ControlHostUiTargetAdapter {
  public:
    virtual ~ControlHostUiTargetAdapter() = default;
    virtual InspectorCapture capture_node_png(const ControlUiExactTarget& target) = 0;
    virtual ControlUiApplyStatus dispatch_input(const ControlUiExactTarget& target,
                                                const ControlUiAuthorityOwner& owner,
                                                const ControlUiInput& input) = 0;
    /// Close any pointer/focus bracket retained by the current controller.
    /// An owner value releases only state acquired by that exact authority;
    /// nullopt is reserved for fenced host teardown.
    virtual void release_controller(
        const std::optional<ControlUiAuthorityOwner>& owner) noexcept = 0;
};

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
    std::shared_ptr<ControlHostUiTargetAdapter> target_adapter;
    /// Opaque generation minted whenever the Standalone attaches a replacement
    /// root view. Required with target_adapter and included in every node call.
    std::string view_generation;
    std::shared_ptr<RuntimeEvaluator> runtime_evaluator;
    /// Required with runtime_evaluator. Returning nullopt fails closed.
    ControlRuntimeEvalRedactor redact_runtime_eval_result;
    std::size_t maximum_capture_bytes = kControlCaptureMaximumBytes;
    std::size_t maximum_eval_result_bytes = kControlRuntimeEvalMaximumResultBytes;
    std::chrono::milliseconds capture_lifetime = std::chrono::minutes(15);
};

/// Canonical exact-instance adapter for screenshot capture, bounded UI input,
/// and explicitly unsafe research evaluation. All operations run through the
/// existing deadline-fenced Inspector main-thread RPC.
class ControlHostUiExecutor {
  public:
    static std::unique_ptr<ControlHostUiExecutor> create(ControlHostUiExecutorConfig config);
    ~ControlHostUiExecutor();

    ControlHostUiExecutor(const ControlHostUiExecutor&) = delete;
    ControlHostUiExecutor& operator=(const ControlHostUiExecutor&) = delete;

    ControlOperationExecutor executor() const;
    bool ready() const;
    /// Fence controller cleanup on the registered main thread. False means the
    /// host must keep the borrowed UI alive and retry before teardown.
    bool disconnect() noexcept;

  private:
    struct State;
    explicit ControlHostUiExecutor(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

/// Stable registry/documentation disposition for the UI-input contract.
std::string_view control_ui_input_disposition() noexcept;

} // namespace pulp::inspect
