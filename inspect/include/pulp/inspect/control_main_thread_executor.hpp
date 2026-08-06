#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/inspect/main_thread_rpc.hpp>

#include <memory>

namespace pulp::inspect {

/// Adapts typed control execution onto the completion-carrying Inspector main
/// thread RPC without creating another queue or transport.
class ControlMainThreadExecutor {
  public:
    using Handler = ControlOperationExecutor;

    ControlMainThreadExecutor(std::shared_ptr<InspectorMainThreadRpc> rpc, Handler handler);

    /// The returned executor owns the adapter state. A main-thread operation
    /// that outlives a fenced started timeout therefore cannot reference a
    /// destroyed adapter or borrowed request/progress objects.
    ControlOperationExecutor executor() const;

  private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::inspect
