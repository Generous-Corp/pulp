#pragma once

#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_observability_bundle.hpp>
#include <pulp/inspect/control_motion_executor.hpp>
#include <pulp/inspect/control_trace_session_executor.hpp>

#include <chrono>
#include <memory>

namespace pulp::inspect {

class MotionInspector;
class MotionScrubber;

struct ControlInstalledHostConfig {
    ControlHostBootstrapRecord bootstrap;
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::shared_ptr<TraceInspector> trace_inspector;
    std::shared_ptr<ControlTelemetryTap> telemetry;
    MotionInspector* motion_inspector = nullptr;
    MotionScrubber* motion_scrubber = nullptr;
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
