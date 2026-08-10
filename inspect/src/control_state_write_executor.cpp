#include <pulp/inspect/control_state_write_executor.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>

#include <choc/text/choc_JSON.h>

#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome fail(ControlResultCode code, std::string explanation,
                             ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome cancelled(std::string explanation) {
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = explanation,
                       .cancellation_reason = std::move(explanation)}};
}

ControlExecutionOutcome unknown_after_apply(std::string explanation) {
    return {.terminal_state = ControlReceiptState::UnknownNeedsRefresh,
            .result = {.result_code = ControlResultCode::UnknownNeedsRefresh,
                       .retry = ControlRetryClassification::AfterRefresh,
                       .explanation = std::move(explanation)}};
}

} // namespace

ControlOperationExecutor
make_control_state_write_executor(ControlStateWriteTargetResolver resolve_target) {
    return [resolve_target = std::move(resolve_target)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.state/parameter-gesture@1" ||
            request.operation_version != 1 ||
            request.registration_id != plan.registration_id.value ||
            request.expected_state_generation != plan.expected_state_generation ||
            !resolve_target || !context.checkpoint) {
            return fail(ControlResultCode::InvalidRequest,
                        "parameter gesture executor is unavailable for this operation");
        }
        state::ParamID parameter_id = 0;
        float normalized = 0.0f;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject() || !params.hasObjectMember("parameter_id") ||
                !params["parameter_id"].isInt() || !params.hasObjectMember("normalized_value") ||
                !(params["normalized_value"].isFloat() || params["normalized_value"].isInt())) {
                return fail(ControlResultCode::InvalidRequest,
                            "parameter gesture request was not canonical");
            }
            const auto raw_id = params["parameter_id"].getInt64();
            const auto raw_value = params["normalized_value"].getWithDefault<double>(-1.0);
            if (raw_id < 0 || raw_id > std::numeric_limits<state::ParamID>::max() ||
                !std::isfinite(raw_value) || raw_value < 0.0 || raw_value > 1.0) {
                return fail(ControlResultCode::InvalidRequest,
                            "parameter gesture values exceeded schema bounds");
            }
            parameter_id = static_cast<state::ParamID>(raw_id);
            normalized = static_cast<float>(raw_value);
        } catch (...) {
            return fail(ControlResultCode::InvalidRequest,
                        "parameter gesture request could not be decoded");
        }

        const auto checkpoint = context.checkpoint();
        if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
            checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
            return cancelled("parameter gesture authority unavailable before apply");
        if (checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
            return fail(ControlResultCode::DeadlineExceeded,
                        "parameter gesture deadline elapsed before apply");
        if (events::MainThreadDispatcher::has_backend() &&
            !events::MainThreadDispatcher::is_main_thread()) {
            return fail(ControlResultCode::HostUnavailable,
                        "parameter gesture requires the host main thread",
                        ControlRetryClassification::AfterBackoff);
        }

        auto target = resolve_target(plan);
        if (!target || !target->store || target->registration_id != plan.registration_id ||
            (target->host_tier != ControlHostTier::Standalone &&
             target->host_tier != ControlHostTier::SharedPluginHost)) {
            return fail(ControlResultCode::HostUnavailable, "exact parameter target is unavailable",
                        ControlRetryClassification::AfterRefresh);
        }
        if (!target->store->info(parameter_id))
            return fail(ControlResultCode::InvalidRequest, "parameter id is not registered");
        const auto observed_generation = target->state_generation;
        if (!target->store->state_snapshot_is_current(observed_generation) ||
            (plan.expected_state_generation != 0 &&
             plan.expected_state_generation != observed_generation)) {
            return fail(ControlResultCode::StateConflict,
                        "state generation changed before parameter apply",
                        ControlRetryClassification::AfterRefresh);
        }
        if (observed_generation >=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return fail(ControlResultCode::ResourceExhausted,
                        "state generation exceeds the canonical wire range");

        const auto apply_checkpoint = context.checkpoint();
        if (apply_checkpoint == ControlExecutionCheckpoint::Cancelled ||
            apply_checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
            return cancelled("parameter gesture authority unavailable at apply");
        if (apply_checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
            return fail(ControlResultCode::DeadlineExceeded,
                        "parameter gesture deadline elapsed at apply");

        const auto applied = target->store->apply_normalized_gesture_if_generation(
            observed_generation, parameter_id, normalized);
        if (applied.status == state::ParameterGestureApplyStatus::GenerationConflict)
            return fail(ControlResultCode::StateConflict,
                        "state generation changed at parameter apply",
                        ControlRetryClassification::AfterRefresh);
        if (applied.status != state::ParameterGestureApplyStatus::Applied)
            return unknown_after_apply(
                "parameter gesture did not commit without concurrent state changes");
        auto detail = choc::value::createObject("ControlStateGestureResult");
        detail.setMember("receipt_id", plan.receipt_id.value);
        detail.setMember("applied", true);
        detail.setMember("state_generation", static_cast<std::int64_t>(applied.generation));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

} // namespace pulp::inspect
