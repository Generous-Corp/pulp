#include <pulp/inspect/control_vendor_operation_executor.hpp>

#include <exception>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome fail(ControlResultCode code, std::string explanation) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
        return fail(ControlResultCode::DeadlineExceeded, "vendor operation deadline elapsed");
    const std::string reason = checkpoint == ControlExecutionCheckpoint::AuthorityRevoked
                                   ? "vendor operation authority was revoked"
                                   : "vendor operation was cancelled";
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = reason,
                       .cancellation_reason = reason}};
}

} // namespace

bool ControlVendorOperationHandlers::add(std::string operation_id,
                                         ControlVendorOperationHandler handler) {
    if (!handler || !is_control_vendor_operation_id(operation_id))
        return false;
    return handlers_.emplace(std::move(operation_id), std::move(handler)).second;
}

const ControlVendorOperationHandler*
ControlVendorOperationHandlers::find(std::string_view operation_id) const {
    const auto found = handlers_.find(operation_id);
    return found == handlers_.end() ? nullptr : &found->second;
}

std::vector<std::string> ControlVendorOperationHandlers::operation_ids() const {
    std::vector<std::string> ids;
    ids.reserve(handlers_.size());
    for (const auto& entry : handlers_)
        ids.push_back(entry.first);
    return ids;
}

ControlOperationExecutor make_control_vendor_operation_executor(
    std::shared_ptr<const ControlVendorOperationHandlers> handlers) {
    return [handlers = std::move(handlers)](const ControlAdmissionPlan& plan,
                                            const ControlRequestEnvelope& request,
                                            const ControlExecutionContext& context) {
        const auto parts = parse_control_vendor_operation_id(request.operation_id);
        const auto tier = control_vendor_tier_for_capability(plan.capability);
        if (!parts || parts->version != request.operation_version || !tier ||
            plan.operation_id != request.operation_id || !context.checkpoint)
            return fail(ControlResultCode::PolicyDenied,
                        "the admitted plan does not describe this vendor operation");
        const auto* handler = handlers ? handlers->find(request.operation_id) : nullptr;
        if (!handler)
            return fail(ControlResultCode::NotImplemented,
                        "no handler is bound to this vendor operation");
        if (const auto checkpoint = context.checkpoint();
            checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);

        const auto checkpoint_function = context.checkpoint;
        const ControlVendorOperationCall call{
            .operation_id = request.operation_id,
            .operation_version = request.operation_version,
            .tier = *tier,
            .arguments_json = request.params_json,
            .receipt_id = plan.receipt_id.value,
            .still_authorized =
                [checkpoint_function] {
                    return checkpoint_function() == ControlExecutionCheckpoint::Continue;
                },
        };
        ControlVendorOperationResult result;
        try {
            result = (*handler)(call);
        } catch (const std::exception& exception) {
            return fail(ControlResultCode::InternalError,
                        std::string("vendor operation handler failed: ") + exception.what());
        } catch (...) {
            return fail(ControlResultCode::InternalError, "vendor operation handler failed");
        }
        if (!result.ok) {
            if (result.error_code == ControlResultCode::Cancelled)
                return checkpoint_failure(ControlExecutionCheckpoint::Cancelled);
            return fail(result.error_code, result.explanation.empty()
                                               ? std::string("vendor operation failed")
                                               : std::move(result.explanation));
        }
        return ControlExecutionOutcome{
            .terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = std::move(result.result_json)},
        };
    };
}

} // namespace pulp::inspect
