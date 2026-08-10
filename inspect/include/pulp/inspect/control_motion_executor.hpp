#pragma once

#include <pulp/inspect/control_main_thread_executor.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pulp::inspect {

class MotionInspector;
class MotionScrubber;

/// Exact T1 host state resolved after canonical admission. Every identity field
/// is checked against the immutable admission plan immediately before apply.
struct ControlMotionTarget {
    ControlRegistrationId registration_id;
    ControlHostTier host_tier = ControlHostTier::Standalone;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::string instance_generation;
    std::function<bool()> authority_live;
    std::function<std::shared_ptr<void>(std::function<void()>)> subscribe_authority_end;
    MotionInspector* inspector = nullptr;
    MotionScrubber* scrubber = nullptr;
};

using ControlMotionTargetResolver =
    std::function<std::optional<ControlMotionTarget>(const ControlAdmissionPlan&)>;

struct ControlMotionExecutorConfig {
    std::shared_ptr<InspectorMainThreadRpc> main_thread_rpc;
    ControlMotionTargetResolver resolve_target;
};

/// Executes dev.pulp.trace/control@1 without invoking the deleted Motion RPC
/// transport. Geometry/scroll traces, fixture replay, and cost observations all
/// run on the registered host main thread under one deadline and authority.
class ControlMotionExecutor {
  public:
    static std::unique_ptr<ControlMotionExecutor> create(ControlMotionExecutorConfig config);

    ControlMotionExecutor(const ControlMotionExecutor&) = delete;
    ControlMotionExecutor& operator=(const ControlMotionExecutor&) = delete;

    ControlOperationExecutor executor() const;

  private:
    explicit ControlMotionExecutor(ControlMainThreadExecutor main_thread);
    ControlMainThreadExecutor main_thread_;
};

} // namespace pulp::inspect
