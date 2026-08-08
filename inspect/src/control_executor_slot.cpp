#include <pulp/inspect/control_executor_slot.hpp>

#include <mutex>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome unavailable() {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = ControlResultCode::HostUnavailable,
                   .retry = ControlRetryClassification::AfterBackoff,
                   .explanation = "host operation executor is not installed"},
    };
}

ControlExecutionOutcome cancelled() {
    return {
        .terminal_state = ControlReceiptState::Cancelled,
        .result = {.result_code = ControlResultCode::Cancelled,
                   .retry = ControlRetryClassification::Never,
                   .explanation = "host operation executor slot is closed",
                   .cancellation_reason = "executor-slot-closed"},
    };
}

} // namespace

struct ControlOperationExecutorSlot::State {
    enum class Phase { Pending, Installed, Closed };

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) const {
        std::shared_ptr<const ControlOperationExecutor> acquired;
        Phase observed = Phase::Pending;
        {
            std::lock_guard lock(mutex);
            observed = phase;
            acquired = installed;
        }
        if (observed == Phase::Closed)
            return cancelled();
        if (!acquired)
            return unavailable();
        return (*acquired)(plan, request, context);
    }

    bool install(ControlOperationExecutor executor) {
        if (!executor)
            return false;
        auto candidate = std::make_shared<const ControlOperationExecutor>(std::move(executor));
        std::lock_guard lock(mutex);
        if (phase != Phase::Pending)
            return false;
        installed = std::move(candidate);
        phase = Phase::Installed;
        return true;
    }

    void close() noexcept {
        std::shared_ptr<const ControlOperationExecutor> released;
        {
            std::lock_guard lock(mutex);
            phase = Phase::Closed;
            released = std::move(installed);
        }
    }

    mutable std::mutex mutex;
    std::shared_ptr<const ControlOperationExecutor> installed;
    Phase phase = Phase::Pending;
};

ControlOperationExecutorSlot::ControlOperationExecutorSlot() : state_(std::make_shared<State>()) {}

ControlOperationExecutorSlot::~ControlOperationExecutorSlot() {
    close();
}

ControlOperationExecutorSlot::ControlOperationExecutorSlot(
    ControlOperationExecutorSlot&& other) noexcept
    : state_(std::move(other.state_)) {}

ControlOperationExecutorSlot&
ControlOperationExecutorSlot::operator=(ControlOperationExecutorSlot&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    state_ = std::move(other.state_);
    return *this;
}

ControlOperationExecutor ControlOperationExecutorSlot::executor() const {
    const auto state = state_;
    return [state](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlExecutionContext& context) {
        return state ? state->execute(plan, request, context) : cancelled();
    };
}

bool ControlOperationExecutorSlot::install(ControlOperationExecutor executor) {
    return state_ && state_->install(std::move(executor));
}

void ControlOperationExecutorSlot::close() noexcept {
    if (state_)
        state_->close();
}

} // namespace pulp::inspect
