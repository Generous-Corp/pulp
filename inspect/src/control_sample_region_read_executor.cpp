#include <pulp/inspect/control_sample_region_read_executor.hpp>
#include <pulp/events/main_thread_dispatcher.hpp>
#include <utility>

namespace pulp::inspect {
ControlOperationExecutor make_control_sample_region_read_executor(ControlSampleRegionTargetResolver resolve) {
    return [resolve = std::move(resolve)](const ControlAdmissionPlan& plan,
                                         const ControlRequestEnvelope& request,
                                         const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (!resolve || request.operation_id != "dev.pulp.graph/sample-region.read@1" ||
            request.operation_version != 1 || request.registration_id != plan.registration_id.value ||
            !context.checkpoint)
            return {.terminal_state = ControlReceiptState::Failed,
                    .result = {.result_code = ControlResultCode::InvalidRequest,
                               .explanation = "region executor request binding is invalid"}};
        if (events::MainThreadDispatcher::has_backend() &&
            !events::MainThreadDispatcher::is_main_thread())
            return {.terminal_state = ControlReceiptState::Failed,
                    .result = {.result_code = ControlResultCode::HostUnavailable,
                               .explanation = "region control requires the host main thread"}};
        const auto target = resolve(plan);
        if (!target || !target->can_read())
            return {.terminal_state = ControlReceiptState::Failed,
                    .result = {.result_code = ControlResultCode::HostUnavailable,
                               .explanation = "exact region provider is unavailable"}};
        return target->read(plan, request, context);
    };
}
} // namespace pulp::inspect
