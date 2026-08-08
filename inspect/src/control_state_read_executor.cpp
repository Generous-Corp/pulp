#include <pulp/inspect/control_state_read_executor.hpp>

#include <pulp/state/param_json.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

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
                                          ? "state read cancelled"
                                          : "state read authority revoked",
                       .cancellation_reason = checkpoint == ControlExecutionCheckpoint::Cancelled
                                                  ? "client-cancelled"
                                                  : "authority-revoked"}};
    }
    return failure(ControlResultCode::DeadlineExceeded, "state read deadline exceeded");
}

const char* rate_id(state::ParamRate rate) {
    return rate == state::ParamRate::AudioRate ? "audio" : "control";
}

} // namespace

ControlOperationExecutor
make_control_state_read_executor(ControlStateReadSourceResolver resolve_source) {
    return [resolve_source = std::move(resolve_source)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (request.operation_id != "dev.pulp.state/read@1" || request.operation_version != 1 ||
            !resolve_source || !context.checkpoint) {
            return failure(ControlResultCode::InvalidRequest,
                           "state read executor is unavailable for this operation");
        }

        bool include_sensitive = false;
        bool include_catalog = true;
        bool parameter_filter_present = false;
        std::vector<state::ParamID> requested_ids;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject())
                return failure(ControlResultCode::InvalidRequest,
                               "state read request was not canonical");
            if (params.hasObjectMember("include_sensitive")) {
                if (!params["include_sensitive"].isBool())
                    return failure(ControlResultCode::InvalidRequest,
                                   "include_sensitive must be boolean");
                include_sensitive = params["include_sensitive"].getBool();
            }
            if (params.hasObjectMember("include_catalog")) {
                if (!params["include_catalog"].isBool())
                    return failure(ControlResultCode::InvalidRequest,
                                   "include_catalog must be boolean");
                include_catalog = params["include_catalog"].getBool();
            }
            if (params.hasObjectMember("parameter_ids")) {
                parameter_filter_present = true;
                const auto ids = params["parameter_ids"];
                if (!ids.isArray() || ids.size() > 4096)
                    return failure(ControlResultCode::InvalidRequest,
                                   "parameter_ids exceeded its schema bounds");
                std::unordered_set<state::ParamID> unique;
                for (std::uint32_t index = 0; index < ids.size(); ++index) {
                    if (!ids[index].isInt())
                        return failure(ControlResultCode::InvalidRequest,
                                       "parameter_ids must contain integers");
                    const auto raw = ids[index].getInt64();
                    if (raw < 0 || raw > std::numeric_limits<state::ParamID>::max() ||
                        !unique.emplace(static_cast<state::ParamID>(raw)).second)
                        return failure(ControlResultCode::InvalidRequest,
                                       "parameter_ids must be unique uint32 values");
                    requested_ids.push_back(static_cast<state::ParamID>(raw));
                }
            }
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "state read request could not be decoded");
        }

        auto checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);
        const auto source = resolve_source(plan);
        if (!source || !source->store || !source->current_state_generation ||
            !source->is_sensitive ||
            source->registration_id != plan.registration_id ||
            source->host_tier == ControlHostTier::SharedPluginHost)
            return failure(ControlResultCode::HostUnavailable, "exact state source is unavailable",
                           ControlRetryClassification::AfterRefresh);
        if (source->state_generation > std::numeric_limits<std::int64_t>::max() ||
            source->catalog_generation > std::numeric_limits<std::int64_t>::max()) {
            return failure(ControlResultCode::ResourceExhausted,
                           "state generation exceeded the wire domain");
        }
        if (plan.expected_state_generation != 0 &&
            plan.expected_state_generation != source->state_generation) {
            return failure(ControlResultCode::StateConflict,
                           "state generation changed before the snapshot",
                           ControlRetryClassification::AfterRefresh);
        }
        if (!parameter_filter_present && source->store->all_params().size() > 4096) {
            return failure(ControlResultCode::ResourceExhausted,
                           "parameter catalog exceeded the snapshot bound");
        }

        const auto selected = [&](state::ParamID id) {
            return !parameter_filter_present ||
                   std::ranges::find(requested_ids, id) != requested_ids.end();
        };
        auto parameters = choc::value::createEmptyArray();
        std::uint64_t redacted = 0;
        for (const auto& info : source->store->all_params()) {
            if (!selected(info.id))
                continue;
            const bool sensitive = source->is_sensitive(info.id);
            if (sensitive && !include_sensitive) {
                ++redacted;
                continue;
            }
            auto item = include_catalog
                            ? state::param_catalog_snapshot_to_value(*source->store, info)
                            : state::param_snapshot_to_value(*source->store, info);
            item.addMember("sensitive", choc::value::createBool(sensitive));
            if (include_catalog)
                item.addMember("rate", choc::value::createString(rate_id(info.rate)));
            parameters.addArrayElement(std::move(item));
        }
        checkpoint = context.checkpoint();
        if (checkpoint != ControlExecutionCheckpoint::Continue)
            return checkpoint_failure(checkpoint);
        if (source->current_state_generation() != source->state_generation) {
            return failure(ControlResultCode::StateConflict,
                           "state generation changed during the snapshot",
                           ControlRetryClassification::AfterRefresh);
        }

        auto detail = choc::value::createObject("ControlStateReadResult");
        detail.setMember("state_generation", static_cast<std::int64_t>(source->state_generation));
        detail.setMember("catalog_generation",
                         static_cast<std::int64_t>(source->catalog_generation));
        detail.setMember("catalog_included", include_catalog);
        detail.setMember("redacted_count", static_cast<std::int64_t>(redacted));
        detail.setMember("parameters", std::move(parameters));
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, true)}};
    };
}

} // namespace pulp::inspect
