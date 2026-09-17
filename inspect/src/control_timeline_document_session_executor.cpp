#include <pulp/inspect/control_timeline_document_session_executor.hpp>

#include <pulp/inspect/control_manifest.hpp>

#include <choc/text/choc_JSON.h>

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace pulp::inspect {
namespace {

using Action = ControlTimelineDocumentSessionAction;
using Conflict = pulp::timeline::ConflictCode;

constexpr std::string_view kOperationId = "dev.pulp.timeline/document-session@1";

struct ActionName {
    Action action;
    std::string_view name;
};

constexpr std::array<ActionName, 5> kActionNames{
    ActionName{Action::Open, "open"}, ActionName{Action::Apply, "apply"},
    ActionName{Action::Diff, "diff"}, ActionName{Action::Undo, "undo"},
    ActionName{Action::Redo, "redo"},
};

ControlExecutionOutcome
failure(ControlResultCode code, std::string explanation,
        ControlRetryClassification retry = ControlRetryClassification::Never,
        std::string detail_json = "{}") {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code,
                       .retry = retry,
                       .explanation = std::move(explanation),
                       .detail_json = std::move(detail_json)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::Cancelled ||
        checkpoint == ControlExecutionCheckpoint::AuthorityRevoked) {
        return {
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = checkpoint == ControlExecutionCheckpoint::Cancelled
                                          ? "timeline document session cancelled"
                                          : "timeline document session authority revoked",
                       .cancellation_reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                                  ? "client-cancelled"
                                                  : "authority-revoked"}};
    }
    return failure(ControlResultCode::DeadlineExceeded,
                   "timeline document session deadline exceeded");
}

std::optional<std::string> object_string(const choc::value::ValueView& object,
                                         std::string_view member) {
    if (!object.hasObjectMember(member))
        return std::nullopt;
    const auto value = object[member];
    if (!value.isString())
        return std::nullopt;
    return std::string(value.getString());
}

} // namespace

std::string_view control_timeline_document_session_action_name(Action action) noexcept {
    for (const auto& entry : kActionNames) {
        if (entry.action == action)
            return entry.name;
    }
    return kActionNames.front().name;
}

std::optional<Action>
control_timeline_document_session_action_by_name(std::string_view name) noexcept {
    for (const auto& entry : kActionNames) {
        if (entry.name == name)
            return entry.action;
    }
    return std::nullopt;
}

ControlResultCode control_timeline_conflict_result_code(Conflict conflict) noexcept {
    switch (conflict) {
    // The CommandAuthority axis. The session already decided it against the
    // mask fixed at writer registration; the broker reports that same verdict.
    case Conflict::CapabilityDenied:
        return ControlResultCode::PolicyDenied;
    // Optimistic concurrency: the caller's expected_revision no longer names
    // the published document, so a refreshed read can succeed.
    case Conflict::StaleRevision:
    case Conflict::CheckpointMismatch:
        return ControlResultCode::StateConflict;
    // Replay identity collisions and gesture-lifecycle violations are caller
    // sequencing errors against live session state.
    case Conflict::TransactionIdCollision:
    case Conflict::CommandIdCollision:
    case Conflict::AlreadyAppliedResultExpired:
    case Conflict::GestureState:
        return ControlResultCode::StateConflict;
    // Quota and capacity ceilings.
    case Conflict::WriterQuotaExhausted:
    case Conflict::JournalFull:
    case Conflict::UndoFull:
    case Conflict::WriterLimit:
    case Conflict::SequenceExhausted:
        return ControlResultCode::ResourceExhausted;
    // Malformed or unsatisfiable requests against the current document.
    case Conflict::InvalidIdentifier:
    case Conflict::EmptyTransaction:
    case Conflict::TargetMissing:
    case Conflict::WrongTargetKind:
    case Conflict::InactiveTarget:
    case Conflict::ParentMismatch:
    case Conflict::ExpectedValueMismatch:
    case Conflict::IdentityNotAvailable:
    case Conflict::NothingToUndo:
    case Conflict::NothingToRedo:
        return ControlResultCode::InvalidRequest;
    // Journal or reducer integrity failures are never caller errors.
    case Conflict::ModelInvariant:
    case Conflict::JournalDurability:
    case Conflict::ReplayDivergence:
    case Conflict::Unspecified:
        return ControlResultCode::InternalError;
    }
    return ControlResultCode::InternalError;
}

ControlRetryClassification control_timeline_conflict_retry(Conflict conflict) noexcept {
    switch (conflict) {
    case Conflict::StaleRevision:
    case Conflict::CheckpointMismatch:
    case Conflict::GestureState:
        return ControlRetryClassification::AfterRefresh;
    case Conflict::WriterQuotaExhausted:
    case Conflict::JournalFull:
    case Conflict::UndoFull:
        return ControlRetryClassification::AfterBackoff;
    case Conflict::CapabilityDenied:
        return ControlRetryClassification::AfterGrant;
    default:
        break;
    }
    return ControlRetryClassification::Never;
}

std::optional<std::string>
control_timeline_admissible_writer_profile(std::string_view requested) noexcept {
    // The broker never selects the unquotaed authority on a caller's behalf,
    // and an unspecified profile resolves down to the least authority rather
    // than to whatever the host itself holds.
    if (requested.empty() || requested == "proposal")
        return std::string("proposal");
    if (requested == "editor")
        return std::string("editor");
    return std::nullopt;
}

ControlOperationExecutor make_control_timeline_document_session_executor(
    ControlTimelineDocumentSessionSourceResolver resolve_source) {
    return [resolve_source = std::move(resolve_source)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != kOperationId || request.operation_version != 1 ||
            !resolve_source || !context.checkpoint) {
            return failure(ControlResultCode::InvalidRequest,
                           "timeline document session executor is unavailable for this operation");
        }

        ControlTimelineDocumentSessionRequest decoded;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject())
                return failure(ControlResultCode::InvalidRequest,
                               "timeline document session request was not an object");
            const auto action_name = object_string(params, "action");
            if (!action_name)
                return failure(ControlResultCode::InvalidRequest,
                               "timeline document session request did not name an action");
            const auto action = control_timeline_document_session_action_by_name(*action_name);
            if (!action)
                return failure(ControlResultCode::InvalidRequest,
                               "timeline document session request named an unknown action");
            decoded.action = *action;
            decoded.project = object_string(params, "project").value_or(std::string{});
            decoded.session_id = object_string(params, "session_id").value_or(std::string{});
            decoded.commands_json = object_string(params, "commands").value_or(std::string{});
            decoded.idempotency_key =
                object_string(params, "idempotency_key").value_or(std::string{});
            const auto profile = control_timeline_admissible_writer_profile(
                object_string(params, "writer_profile").value_or(std::string{}));
            if (!profile) {
                return failure(ControlResultCode::PolicyDenied,
                               "the broker does not admit the requested writer profile");
            }
            decoded.writer_profile = *profile;
            if (params.hasObjectMember("expected_revision")) {
                const auto revision = params["expected_revision"];
                if (!revision.isInt())
                    return failure(ControlResultCode::InvalidRequest,
                                   "expected_revision was not an integer");
                const auto value = revision.getInt64();
                if (value < 0)
                    return failure(ControlResultCode::InvalidRequest,
                                   "expected_revision was negative");
                decoded.expected_revision = static_cast<std::uint64_t>(value);
            }
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "timeline document session request could not be decoded");
        }

        if (decoded.action == Action::Open) {
            if (decoded.project.empty() || !decoded.session_id.empty())
                return failure(ControlResultCode::InvalidRequest,
                               "open requires a project and must not name a session");
        } else if (decoded.session_id.empty()) {
            return failure(ControlResultCode::InvalidRequest,
                           "this action requires an open session id");
        }
        if (decoded.action != Action::Apply &&
            (!decoded.commands_json.empty() || !decoded.idempotency_key.empty() ||
             decoded.expected_revision.has_value())) {
            return failure(ControlResultCode::InvalidRequest,
                           "commands, idempotency_key, and expected_revision are apply-only");
        }
        if (decoded.action == Action::Apply && decoded.commands_json.empty())
            return failure(ControlResultCode::InvalidRequest, "apply requires commands");

        auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);

        const auto source = resolve_source(plan);
        if (!source || !source->invoke || source->registration_id != plan.registration_id ||
            source->instance_id != plan.instance_id ||
            source->publication_id != plan.publication_id) {
            return failure(ControlResultCode::HostUnavailable,
                           "exact timeline document session source is unavailable",
                           ControlRetryClassification::AfterRefresh);
        }

        const auto outcome = source->invoke(decoded);

        checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);

        if (!outcome.accepted) {
            if (!outcome.conflict) {
                return failure(ControlResultCode::InternalError,
                               "timeline document session refused without naming a conflict");
            }
            auto detail = outcome.refusal_json.empty() ? std::string("{}") : outcome.refusal_json;
            return failure(control_timeline_conflict_result_code(*outcome.conflict),
                           outcome.explanation.empty()
                               ? std::string("timeline document session refused the request")
                               : outcome.explanation,
                           control_timeline_conflict_retry(*outcome.conflict), std::move(detail));
        }

        if (outcome.session_id.empty()) {
            return failure(ControlResultCode::InternalError,
                           "timeline document session accepted without publishing a session id");
        }
        if (outcome.revision > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return failure(ControlResultCode::ResourceExhausted,
                           "document revision exceeds the canonical wire range");

        auto detail = choc::value::createObject("ControlTimelineDocumentSessionResult");
        detail.setMember("receipt_id", plan.receipt_id.value);
        detail.setMember(
            "action", std::string(control_timeline_document_session_action_name(decoded.action)));
        detail.setMember("session_id", outcome.session_id);
        detail.setMember("revision", static_cast<std::int64_t>(outcome.revision));
        detail.setMember("applied", outcome.applied);
        detail.setMember("replayed", outcome.replayed);
        detail.setMember("writer_profile", decoded.writer_profile);
        if (decoded.action == Action::Diff)
            detail.setMember("diff", outcome.diff_json);

        auto result_json = choc::json::toString(detail, true);
        const auto* operation = resolve_control_operation(kOperationId, 1);
        ControlJsonSchemaDiagnostics diagnostics;
        if (operation == nullptr || !validate_control_output_json_schema(
                                        result_json, operation->output_schema_json, &diagnostics)) {
            return failure(ControlResultCode::InternalError,
                           "timeline document session produced an invalid v1 result");
        }
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = std::move(result_json)}};
    };
}

} // namespace pulp::inspect
