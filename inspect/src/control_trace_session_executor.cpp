#include <pulp/inspect/control_trace_session_executor.hpp>

#include <pulp/inspect/control_manifest.hpp>

#include <choc/text/choc_JSON.h>

#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kOperationId = "dev.pulp.trace/session-control@1";

ControlExecutionOutcome failure(ControlResultCode code, ControlRetryClassification retry,
                                std::string explanation) {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result =
            {
                .result_code = code,
                .retry = retry,
                .explanation = std::move(explanation),
            },
    };
}

ControlExecutionOutcome legacy_failure(const InspectorMessage& response) {
    auto code = ControlResultCode::InternalError;
    auto retry = ControlRetryClassification::Never;
    if (response.error_code == "invalid_params") {
        code = ControlResultCode::InvalidRequest;
    } else if (response.error_code == "tracing_unavailable") {
        code = ControlResultCode::NotBuilt;
    } else if (response.error_code == "trace_owner_unbound") {
        code = ControlResultCode::SessionStale;
        retry = ControlRetryClassification::AfterRefresh;
    } else if (response.error_code == "trace_owned_by_another_controller" ||
               response.error_code == "trace_already_active") {
        code = ControlResultCode::LeaseConflict;
        retry = ControlRetryClassification::AfterRefresh;
    } else if (response.error_code == "no_active_trace") {
        code = ControlResultCode::Inactive;
        retry = ControlRetryClassification::AfterRefresh;
    }
    return failure(code, retry, response.params_json);
}

} // namespace

struct ControlTraceSessionExecutor::State {
    std::shared_ptr<TraceInspector> trace;
    std::unique_ptr<TraceOwnerLease> owner;
    ControlRegistrationId registration_id;

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request) const {
        if (plan.registration_id != registration_id ||
            request.registration_id != registration_id.value)
            return failure(ControlResultCode::SessionStale,
                           ControlRetryClassification::AfterRefresh,
                           "trace execution registration does not match the bound host");
        if (plan.operation_id != request.operation_id ||
            plan.operation_version != request.operation_version)
            return failure(ControlResultCode::InvalidRequest, ControlRetryClassification::Never,
                           "trace execution plan does not match the request");
        if (request.operation_id != kOperationId || request.operation_version != 1)
            return failure(ControlResultCode::NotImplemented, ControlRetryClassification::Never,
                           "host does not implement the requested control operation");

        const auto* descriptor =
            resolve_control_operation(request.operation_id, request.operation_version);
        ControlJsonSchemaDiagnostics diagnostics;
        if (!descriptor || !validate_control_json_schema(
                               request.params_json, descriptor->input_schema_json, &diagnostics)) {
            return failure(ControlResultCode::InvalidRequest, ControlRetryClassification::Never,
                           diagnostics.explanation.empty() ? "invalid trace-session request"
                                                           : std::move(diagnostics.explanation));
        }

        choc::value::Value params;
        try {
            params = choc::json::parse(request.params_json);
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest, ControlRetryClassification::Never,
                           "trace-session request is not valid JSON");
        }

        const auto action = params["action"].getString();
        InspectorMessage response;
        if (action == "start") {
            auto legacy = choc::value::createObject("");
            if (params.hasObjectMember("categories"))
                legacy.addMember("categories", params["categories"]);
            if (params.hasObjectMember("ring_mb"))
                legacy.addMember("ring_mb", params["ring_mb"]);
            response = owner->handle(make_request(1, std::string(methods::kTraceStartSession),
                                                  choc::json::toString(legacy, false)));
        } else {
            response = owner->handle(
                make_request(1, std::string(methods::kTraceStopSession), "{}"));
        }
        if (response.is_error)
            return legacy_failure(response);

        if (!validate_control_output_json_schema(response.params_json,
                                                 descriptor->output_schema_json, &diagnostics)) {
            return failure(ControlResultCode::InternalError, ControlRetryClassification::Never,
                           diagnostics.explanation.empty()
                               ? "trace-session result violated its contract"
                               : std::move(diagnostics.explanation));
        }
        return {
            .terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = std::move(response.params_json)},
        };
    }
};

ControlTraceSessionExecutor::ControlTraceSessionExecutor(std::shared_ptr<State> state,
                                                         ControlMainThreadExecutor main_thread)
    : state_(std::move(state)), main_thread_(std::move(main_thread)) {}

std::unique_ptr<ControlTraceSessionExecutor>
ControlTraceSessionExecutor::create(ControlTraceSessionExecutorConfig config) {
    if (!config.main_thread_rpc || !config.trace_inspector || !config.registration_id)
        return nullptr;
    auto owner = config.trace_inspector->bind_control_registration(config.registration_id.value);
    if (!owner)
        return nullptr;
    auto state = std::make_shared<State>(State{
        .trace = std::move(config.trace_inspector),
        .owner = std::move(owner),
        .registration_id = std::move(config.registration_id),
    });
    ControlMainThreadExecutor main_thread{
        std::move(config.main_thread_rpc),
        [state](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                const ControlExecutionContext&) { return state->execute(plan, request); },
    };
    return std::unique_ptr<ControlTraceSessionExecutor>(
        new ControlTraceSessionExecutor(std::move(state), std::move(main_thread)));
}

ControlOperationExecutor ControlTraceSessionExecutor::executor() const {
    return main_thread_.executor();
}

} // namespace pulp::inspect
