#include <choc/text/choc_JSON.h>
#include <pulp/host/signal_graph_control.hpp>
#include <pulp/inspect/control_signal_graph_executor.hpp>

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace {
ControlExecutionOutcome fail(ControlResultCode code, std::string explanation) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .explanation = std::move(explanation)}};
}
} // namespace

ControlOperationExecutor
make_control_signal_graph_executor(ControlSignalGraphAuthorityResolver resolve) {
    return [resolve = std::move(resolve)](
               const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
               const ControlExecutionContext& context) -> ControlExecutionOutcome {
        if (!resolve || request.operation_id != "dev.pulp.graph/modulation-route.edit@1" ||
            request.operation_version != 1 || !context.checkpoint)
            return fail(ControlResultCode::InvalidRequest,
                        "modulation route executor request binding is invalid");
        if (context.checkpoint() != ControlExecutionCheckpoint::Continue)
            return fail(ControlResultCode::Cancelled, "modulation route authority is not live");
        host::SignalGraphControlAuthority* authority = resolve(plan);
        if (!authority)
            return fail(ControlResultCode::HostUnavailable,
                        "the graph modulation authority is unavailable");
        std::vector<host::SignalGraphRouteCommand> commands;
        try {
            const auto params = choc::json::parse(request.params_json);
            if (!params.isObject() || !params["commands"].isArray())
                return fail(ControlResultCode::InvalidRequest, "commands must be an array");
            const auto values = params["commands"];
            if (values.size() == 0 ||
                values.size() > host::SignalGraphControlAuthority::kMaxDenseCommands)
                return fail(ControlResultCode::ResourceExhausted,
                            "dense modulation command batch exceeds the bounded capacity");
            commands.reserve(values.size());
            for (const auto command_value : values) {
                if (!command_value.isObject() || !command_value["kind"].isString())
                    return fail(ControlResultCode::InvalidRequest, "route command is malformed");
                host::SignalGraphRouteCommand command;
                const auto kind = command_value["kind"].getString();
                if (kind == "insert")
                    command.kind = decltype(command.kind)::Insert;
                else if (kind == "rewire")
                    command.kind = decltype(command.kind)::Rewire;
                else if (kind == "remove")
                    command.kind = decltype(command.kind)::Remove;
                else
                    return fail(ControlResultCode::InvalidRequest, "route command kind is invalid");
                for (const auto* field : {"source", "source_port", "destination", "parameter_id",
                                          "previous_source", "previous_source_port"}) {
                    const auto value = command_value[field];
                    if (value.isVoid())
                        continue;
                    if (!value.isInt() || value.getInt64() < 0 ||
                        value.getInt64() > std::numeric_limits<std::uint32_t>::max())
                        return fail(ControlResultCode::InvalidRequest,
                                    "route command identifier exceeds the uint32 range");
                }
                command.audio_rate =
                    command_value["audio_rate"].isBool() && command_value["audio_rate"].getBool();
                command.source = static_cast<host::NodeId>(command_value["source"].getInt64());
                command.source_port =
                    static_cast<host::PortIndex>(command_value["source_port"].getInt64());
                command.destination =
                    static_cast<host::NodeId>(command_value["destination"].getInt64());
                command.parameter_id =
                    static_cast<std::uint32_t>(command_value["parameter_id"].getInt64());
                if (command_value["previous_source"].isInt())
                    command.previous_source =
                        static_cast<host::NodeId>(command_value["previous_source"].getInt64());
                if (command_value["previous_source_port"].isInt())
                    command.previous_source_port = static_cast<host::PortIndex>(
                        command_value["previous_source_port"].getInt64());
                if (command_value["range_lo"].isFloat() || command_value["range_lo"].isInt())
                    command.range_lo = static_cast<float>(
                        command_value["range_lo"].getWithDefault<double>(command.range_lo));
                if (command_value["range_hi"].isFloat() || command_value["range_hi"].isInt())
                    command.range_hi = static_cast<float>(
                        command_value["range_hi"].getWithDefault<double>(command.range_hi));
                if (command_value["smoothing_ms"].isFloat() ||
                    command_value["smoothing_ms"].isInt())
                    command.smoothing_ms = static_cast<float>(
                        command_value["smoothing_ms"].getWithDefault<double>(command.smoothing_ms));
                commands.push_back(command);
            }
        } catch (...) {
            return fail(ControlResultCode::InvalidRequest, "route command JSON is invalid");
        }
        const auto result = authority->apply(commands);
        auto detail = choc::value::createObject("");
        detail.addMember("receipt_id", plan.receipt_id.value);
        detail.addMember(
            "code",
            result ? "applied"
            : result.code == host::SignalGraphRouteResultCode::DenseQueueOverflow
                ? "dense-queue-overflow"
            : result.code == host::SignalGraphRouteResultCode::EmptyBatch    ? "empty-batch"
            : result.code == host::SignalGraphRouteResultCode::PrepareFailed ? "prepare-failed"
            : result.code == host::SignalGraphRouteResultCode::CommitFailed  ? "commit-failed"
                                                                             : "invalid-command");
        detail.addMember("applied", static_cast<std::int64_t>(result.applied));
        detail.addMember("capacity", static_cast<std::int64_t>(result.capacity));
        detail.addMember("generation", static_cast<std::int64_t>(result.generation));
        const auto code = result
                              ? ControlResultCode::InvalidRequest
                              : (result.code == host::SignalGraphRouteResultCode::DenseQueueOverflow
                                     ? ControlResultCode::ResourceExhausted
                                     : ControlResultCode::InvalidRequest);
        if (!result)
            return {.terminal_state = ControlReceiptState::Failed,
                    .result = {.result_code = code,
                               .explanation = "modulation route batch refused",
                               .detail_json = choc::json::toString(detail, false)}};
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(detail, false)}};
    };
}
} // namespace pulp::inspect
