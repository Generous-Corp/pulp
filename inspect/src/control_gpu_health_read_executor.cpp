#include <pulp/inspect/control_gpu_health_read_executor.hpp>

#include <pulp/inspect/control_manifest.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome
failure(ControlResultCode code, std::string explanation,
        ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
        checkpoint == ControlExecutionCheckpoint::AuthorityRevoked) {
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = checkpoint == ControlExecutionCheckpoint::Cancelled
                                          ? "GPU health read cancelled"
                                          : "GPU health read authority revoked",
                       .cancellation_reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                                  ? "client-cancelled"
                                                  : "authority-revoked"}};
    }
    return failure(ControlResultCode::DeadlineExceeded, "GPU health read deadline exceeded");
}

void append_correlation_id(const std::optional<std::string>& correlation,
                           std::vector<std::string>& output) {
    if (!correlation)
        return;
    if (std::ranges::find(output, *correlation) == output.end())
        output.push_back(*correlation);
}

} // namespace

ControlOperationExecutor
make_control_gpu_health_read_executor(ControlGpuHealthReadSourceResolver resolve_source) {
    return [resolve_source = std::move(resolve_source)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.gpu/health.read@1" ||
            request.operation_version != 1 || !resolve_source || !context.checkpoint) {
            return failure(ControlResultCode::InvalidRequest,
                           "GPU health read executor is unavailable for this operation");
        }
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject() || params.size() != 0)
                return failure(ControlResultCode::InvalidRequest,
                               "GPU health read request was not the canonical empty object");
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "GPU health read request could not be decoded");
        }

        auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);
        const auto source = resolve_source(plan);
        if (!source || !source->read_result ||
            source->registration_id != plan.registration_id ||
            source->instance_id != plan.instance_id ||
            source->publication_id != plan.publication_id) {
            return failure(ControlResultCode::HostUnavailable,
                           "exact GPU health source is unavailable",
                           ControlRetryClassification::AfterRefresh);
        }
        const auto result = source->read_result();
        if (!result) {
            return failure(ControlResultCode::HostUnavailable,
                           "GPU health snapshot is not available",
                           ControlRetryClassification::AfterRefresh);
        }

        checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);
        std::string semantic_error;
        if (!tooling::gpu_health::validate(*result, &semantic_error))
            return failure(ControlResultCode::InternalError,
                           "GPU health producer returned a semantically invalid v1 snapshot");
        auto result_json = tooling::gpu_health::to_json(*result);
        const auto* operation = resolve_control_operation("dev.pulp.gpu/health.read@1", 1);
        ControlJsonSchemaDiagnostics diagnostics;
        if (operation == nullptr ||
            !validate_control_output_json_schema(result_json, operation->output_schema_json,
                                                 &diagnostics)) {
            return failure(ControlResultCode::InternalError,
                           "GPU health producer returned an invalid v1 snapshot");
        }

        std::vector<std::string> evidence_ids;
        append_correlation_id(result->startup.correlation.gpu_evidence_id, evidence_ids);
        append_correlation_id(result->startup.correlation.trace_evidence_id, evidence_ids);
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = std::move(result_json),
                           .evidence_ids = std::move(evidence_ids)}};
    };
}

} // namespace pulp::inspect
