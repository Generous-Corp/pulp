#pragma once

#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/trace_inspector.hpp>

#include <memory>

namespace pulp::inspect {

struct ControlTraceSessionExecutorConfig {
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    std::shared_ptr<TraceInspector> trace_inspector;
    ControlRegistrationId registration_id;
};

/// Executes the canonical trace-session operation through the existing
/// process-global tracing owner on the registered host main thread.
class ControlTraceSessionExecutor {
  public:
    static std::unique_ptr<ControlTraceSessionExecutor>
    create(ControlTraceSessionExecutorConfig config);

    ControlTraceSessionExecutor(const ControlTraceSessionExecutor&) = delete;
    ControlTraceSessionExecutor& operator=(const ControlTraceSessionExecutor&) = delete;

    ControlOperationExecutor executor() const;

  private:
    struct State;
    ControlTraceSessionExecutor(std::shared_ptr<State> state,
                                ControlMainThreadExecutor main_thread);

    std::shared_ptr<State> state_;
    ControlMainThreadExecutor main_thread_;
};

} // namespace pulp::inspect
