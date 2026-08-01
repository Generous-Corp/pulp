#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulp::format {
class StandaloneApp;
class Processor;
class ViewBridge;
} // namespace pulp::format
namespace pulp::view {
class View;
class WindowHost;
} // namespace pulp::view
namespace pulp::inspect {
class InspectorOverlay;
}

namespace pulp::format::detail {

struct StandaloneInspectorLifecycleState {
    bool rpc_accepting = false;
    bool dispatch_accepting = false;
    bool borrowed_sources_attached = false;
};

#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
enum class StandaloneInspectorAuditOutcome : std::uint8_t {
    Denied,
    Applied,
    Rejected,
};

/// Test-only metadata projection. It intentionally cannot expose request
/// parameters even if the production audit representation later grows.
struct StandaloneInspectorAuditEntry {
    std::string session_id;
    std::string instance_id;
    std::string client_id;
    std::string method;
    StandaloneInspectorAuditOutcome outcome = StandaloneInspectorAuditOutcome::Rejected;
    std::string error_code;
};

struct StandaloneInspectorTelemetryState {
    std::size_t pending_disconnects = 0;
    std::size_t active_subscriptions = 0;
    std::uint64_t source_generation = 0;
};

/// Process-local fixture seam that may take ownership of the next inspector
/// RPC closure and report whether it accepted that closure.
using StandaloneInspectorRpcPostOverride =
    std::function<std::optional<bool>(std::function<void()>&)>;
void set_standalone_inspector_rpc_post_override_for_testing(
    StandaloneInspectorRpcPostOverride post_override);
#endif

/// Host-owned composition root for one explicitly activated standalone
/// Development Inspector session. Protocol/session code remains platform-free;
/// this owner binds live standalone state and tears transport down before UI.
class StandaloneInspectorRuntime {
  public:
    static std::unique_ptr<StandaloneInspectorRuntime>
    create(StandaloneApp& app, Processor& processor, ViewBridge& bridge, view::View& root,
           view::WindowHost& window, std::string profile,
           std::vector<std::string> custom_capabilities);

    ~StandaloneInspectorRuntime();
    StandaloneInspectorRuntime(const StandaloneInspectorRuntime&) = delete;
    StandaloneInspectorRuntime& operator=(const StandaloneInspectorRuntime&) = delete;

    /// Called from the window idle loop, after its main-thread dispatcher is live.
    void pump();
    void stop();
    /// Make one non-blocking attempt to complete fence-gated source retirement.
    /// Used after a platform event loop exits and by deterministic lifecycle tests.
    bool try_finish_retirement();
    bool retirement_pending() const;
    StandaloneInspectorLifecycleState lifecycle_state() const;
#if defined(PULP_STANDALONE_INSPECTOR_TEST_HOOKS)
    std::vector<StandaloneInspectorAuditEntry> audit_snapshot_for_testing() const;
    StandaloneInspectorTelemetryState telemetry_state_for_testing() const;
#endif
    void set_overlay_active(bool active);
    bool startup_failed() const { return startup_failed_; }
    std::function<void()> wrap_close(std::function<void()> close_editor);

  private:
    class Impl;
    class RetirementCoordinator;
    StandaloneInspectorRuntime(std::shared_ptr<inspect::InspectorOverlay> overlay,
                               std::shared_ptr<Impl> impl, std::vector<std::uint8_t> token,
                               view::View& root, view::WindowHost& window);
    std::shared_ptr<inspect::InspectorOverlay> overlay_;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<RetirementCoordinator> retirement_;
    std::vector<std::uint8_t> token_;
    view::View& root_;
    view::WindowHost& window_;
    bool startup_attempted_ = false;
    bool startup_failed_ = false;
    bool stopped_ = false;
};

} // namespace pulp::format::detail
