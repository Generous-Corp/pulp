#include <pulp/inspect/control_read_operations.hpp>

#include <choc/text/choc_JSON.h>

#include <limits>
#include <utility>

namespace pulp::inspect {
namespace {

ControlExecutionOutcome
failure(ControlResultCode code, std::string explanation,
        ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {
        .terminal_state = ControlReceiptState::Failed,
        .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)},
    };
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
        checkpoint == ControlExecutionCheckpoint::AuthorityRevoked) {
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = checkpoint == ControlExecutionCheckpoint::Cancelled
                                          ? "read operation cancelled"
                                          : "read operation authority revoked",
                       .cancellation_reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                                  ? "client-cancelled"
                                                  : "authority-revoked"},
        };
    }
    return failure(ControlResultCode::DeadlineExceeded, "read operation deadline exceeded");
}

std::string_view profile_id_or_unknown(ControlBuildProfile profile) {
    const auto id = control_profile_id(profile);
    return id.empty() ? std::string_view{"unknown"} : id;
}

bool exact_registration(const ControlRegistration& registration, const ControlAdmissionPlan& plan) {
    return registration.registration_id == plan.registration_id &&
           registration.broker_id == plan.broker_id && registration.session_id == plan.session_id &&
           registration.instance_id == plan.instance_id &&
           registration.publication_id == plan.publication_id &&
           registration.manifest_digest == plan.manifest_digest &&
           registration.artifact_digest == plan.producer_artifact_digest;
}

} // namespace

ControlExecutionOutcome execute_control_instance_read(ControlBroker& broker,
                                                      const ControlAdmissionPlan& plan,
                                                      const ControlRequestEnvelope& request,
                                                      const ControlExecutionContext& context) {
    if (request.operation_id != "dev.pulp.instance/read@1" || request.operation_version != 1 ||
        !context.checkpoint) {
        return failure(ControlResultCode::InvalidRequest,
                       "instance read executor is unavailable for this operation");
    }
    const auto checkpoint = context.checkpoint();
    if (checkpoint != ControlExecutionCheckpoint::Continue)
        return checkpoint_failure(checkpoint);
    const auto registration = broker.registration(plan.registration_id);
    if (!registration || !exact_registration(*registration, plan)) {
        return failure(ControlResultCode::HostUnavailable,
                       "exact instance registration is no longer live",
                       ControlRetryClassification::AfterRefresh);
    }
    if (registration->host_tier == ControlHostTier::SharedPluginHost) {
        return failure(ControlResultCode::HostUnavailable,
                       "instance read is unavailable for this host tier");
    }
    if (registration->liveness_generation > std::numeric_limits<std::int64_t>::max()) {
        return failure(ControlResultCode::ResourceExhausted,
                       "liveness generation exceeded the wire domain");
    }

    auto detail = choc::value::createObject("ControlInstanceReadResult");
    detail.setMember("broker_id", registration->broker_id.value);
    detail.setMember("registration_id", registration->registration_id.value);
    detail.setMember("session_id", registration->session_id);
    detail.setMember("instance_id", registration->instance_id);
    detail.setMember("publication_id", registration->publication_id);
    detail.setMember("instance_kind", std::string(control_host_tier_id(registration->host_tier)));
    detail.setMember("lifecycle_status", "active");
    detail.setMember("liveness_generation",
                     static_cast<std::int64_t>(registration->liveness_generation));
    detail.setMember("plugin_id", registration->plugin_id);
    detail.setMember("publisher_id", registration->publisher_id);
    detail.setMember("build_id", registration->build_id);
    detail.setMember("profile", std::string(profile_id_or_unknown(registration->profile)));
    detail.setMember("manifest_digest", registration->manifest_digest);
    detail.setMember("artifact_digest", registration->artifact_digest);
    detail.setMember("peer_identity_sha256", registration->peer_fingerprint);
    auto capabilities = choc::value::createEmptyArray();
    for (const auto capability : registration->capabilities)
        capabilities.addArrayElement(
            choc::value::createString(std::string(capability_contract_id(capability))));
    detail.setMember("capabilities", std::move(capabilities));
    return {.terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = choc::json::toString(detail, true)}};
}

} // namespace pulp::inspect
