#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace pulp::view {

class ScriptEngine;

/// The engine-thread half of a runtime inspector, as ScriptedUiSession drives
/// it. The session attaches the port to its live engine across reloads, pumps
/// it once per poll(), and discharges the realm reset a completed evaluation
/// owes. What the port does with the engine (off-thread Runtime.evaluate,
/// interrupt) is the implementation's business; Pulp's implementation is
/// ScriptInspectorBridge.
///
/// Every method is called on the session's engine thread.
class ScriptInspectorPort {
  public:
    using EvaluationDeadline = std::chrono::steady_clock::time_point;

    virtual ~ScriptInspectorPort();

    /// Point the port at `engine`, or at none with nullptr.
    virtual void attach(ScriptEngine* engine) = 0;

    /// Release the engine; a blocked off-thread request wakes as detached.
    virtual void detach() = 0;

    /// Run at most one queued request. True when it changed realm state.
    virtual bool pump() = 0;

    /// Install the realm reset a completed evaluation owes. The session
    /// installs it once, before the first attach().
    virtual void
    set_post_evaluation_reset(std::function<std::string(EvaluationDeadline)> reset) = 0;

    /// Run the owed reset if one is pending. Returns its error text, empty on
    /// success or when nothing was owed.
    virtual std::string run_pending_post_evaluation_reset() = 0;
};

/// Builds the port every ScriptedUiSession constructed afterwards installs.
using ScriptInspectorPortFactory = std::unique_ptr<ScriptInspectorPort> (*)();

/// Process-wide: set before the sessions that should carry an inspector are
/// constructed (sessions built earlier keep what they had). nullptr, the
/// default, builds sessions with no inspector port.
void set_script_inspector_port_factory(ScriptInspectorPortFactory factory) noexcept;
ScriptInspectorPortFactory script_inspector_port_factory() noexcept;

} // namespace pulp::view
