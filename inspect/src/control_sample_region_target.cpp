#include <algorithm>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <limits>
#include <map>
#include <pulp/host/baked_codec.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_protocol.hpp>
#include <pulp/inspect/control_sample_region_target.hpp>
#include <pulp/state/store.hpp>
#include <set>
#include <stdexcept>
#include <tuple>

namespace pulp::inspect {
namespace {
using namespace host;
using Value = choc::value::Value;
using View = choc::value::ValueView;
constexpr std::uint64_t max_generation = 9007199254740991ULL;

Value object() {
    return choc::value::createObject("RegionControl");
}
void number(Value& out, const char* name, std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::out_of_range("region integer exceeds signed JSON representation");
    out.setMember(name, static_cast<std::int64_t>(value));
}
ControlExecutionOutcome refusal(ControlResultCode code, std::string message, std::string path = "/",
                                std::string reason = "PrepareFailed") {
    auto detail = object();
    detail.setMember("code", reason);
    detail.setMember("path", path.substr(0, 1024));
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code,
                       .explanation = message.substr(0, 512),
                       .detail_json = choc::json::toString(detail)}};
}
std::optional<ControlExecutionOutcome> checkpoint(const ControlExecutionContext& context) {
    if (!context.checkpoint)
        return refusal(ControlResultCode::InvalidRequest, "missing execution authority");
    switch (context.checkpoint()) {
    case ControlExecutionCheckpoint::Continue:
        return {};
    case ControlExecutionCheckpoint::DeadlineExceeded:
        return refusal(ControlResultCode::DeadlineExceeded, "region control deadline elapsed");
    default:
        return ControlExecutionOutcome{
            .terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = "region authority cancelled before publication",
                       .cancellation_reason = "cancelled or revoked"}};
    }
}

bool resources_fit_wire(const SampleRegionResourceStats& r) {
    constexpr auto cap = SampleRegionLimits::v1();
    return r.member_nodes <= cap.max_member_nodes &&
           r.internal_connections <= cap.max_internal_connections &&
           r.input_boundaries <= cap.max_input_boundaries &&
           r.output_boundaries <= cap.max_output_boundaries &&
           r.delay_nodes <= cap.max_delay_nodes &&
           r.promoted_parameters <= cap.max_promoted_parameters &&
           r.state_bytes <= cap.max_state_bytes && r.state_alignment <= alignof(float) &&
           r.logical_boundary_bytes <= cap.max_logical_boundary_bytes &&
           r.work_per_frame <= cap.max_work_per_frame && r.work_per_block <= cap.max_work_per_block;
}
Value resources(const SampleRegionResourceStats& r) {
    if (!resources_fit_wire(r))
        throw std::out_of_range("region resources exceed closed wire bounds");
    auto result = object();
#define FIELD(name) number(result, #name, r.name)
    FIELD(member_nodes);
    FIELD(internal_connections);
    FIELD(input_boundaries);
    FIELD(output_boundaries);
    FIELD(delay_nodes);
    FIELD(promoted_parameters);
    FIELD(state_bytes);
    FIELD(state_alignment);
    FIELD(logical_boundary_bytes);
    FIELD(work_per_frame);
    FIELD(work_per_block);
#undef FIELD
    return result;
}
Value limits(const SampleRegionLimits& r) {
    auto result = object();
#define FIELD(name) number(result, #name, r.name)
    FIELD(max_member_nodes);
    FIELD(max_internal_connections);
    FIELD(max_input_boundaries);
    FIELD(max_output_boundaries);
    FIELD(max_delay_nodes);
    FIELD(max_promoted_parameters);
    FIELD(max_state_bytes);
    FIELD(max_logical_boundary_bytes);
    FIELD(max_work_per_frame);
    FIELD(max_work_per_block);
#undef FIELD
    return result;
}
Value config(const SampleKernelConfig& c) {
    auto result = object();
    switch (c.kind) {
    case SampleKernelConfigKind::None:
        result.setMember("kind", "none");
        break;
    case SampleKernelConfigKind::BoundaryIndex:
        result.setMember("kind", "boundary_index");
        number(result, "boundary_index", c.boundary_index_or_parameter_id);
        break;
    case SampleKernelConfigKind::FiniteConstant:
        result.setMember("kind", "finite_constant");
        result.setMember("value", c.constant);
        break;
    case SampleKernelConfigKind::PromotedParameterId:
        result.setMember("kind", "promoted_parameter_id");
        number(result, "promoted_parameter_id", c.boundary_index_or_parameter_id);
        break;
    default:
        result.setMember("kind", "invalid");
        break;
    }
    return result;
}
Value connection(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port,
                 bool crossing) {
    auto out = object();
    number(out, "source_node_id", source);
    number(out, "source_port", source_port);
    number(out, "destination_node_id", dest);
    number(out, "destination_port", dest_port);
    out.setMember("crossing", crossing);
    return out;
}
std::string_view reason_name(SampleRegionRefusalReason reason) {
    switch (reason) {
    case SampleRegionRefusalReason::None:
        return "Accepted";
    case SampleRegionRefusalReason::UnknownRegion:
        return "UnknownRegion";
    case SampleRegionRefusalReason::UnknownMember:
        return "UnknownMember";
    case SampleRegionRefusalReason::MemberInMultipleRegions:
        return "MemberInMultipleRegions";
    case SampleRegionRefusalReason::InvalidBoundary:
        return "InvalidBoundary";
    case SampleRegionRefusalReason::InvalidBoundaryCrossing:
        return "InvalidBoundaryCrossing";
    case SampleRegionRefusalReason::InvalidProducerCardinality:
        return "InvalidProducerCardinality";
    case SampleRegionRefusalReason::DisconnectedMember:
        return "DisconnectedMember";
    case SampleRegionRefusalReason::InvalidKernelConfig:
        return "InvalidKernelConfig";
    case SampleRegionRefusalReason::UnsupportedNodeKind:
        return "UnsupportedNodeKind";
    case SampleRegionRefusalReason::SampleKernelOutsideRegion:
        return "SampleKernelOutsideRegion";
    case SampleRegionRefusalReason::UnresolvedSampleKernel:
        return "UnresolvedSampleKernel";
    case SampleRegionRefusalReason::UnsupportedConnectionLane:
        return "UnsupportedConnectionLane";
    case SampleRegionRefusalReason::LegacyFeedbackInRegion:
        return "LegacyFeedbackInRegion";
    case SampleRegionRefusalReason::CycleCrossesRegionBoundary:
        return "CycleCrossesRegionBoundary";
    case SampleRegionRefusalReason::InstantaneousCycle:
        return "InstantaneousCycle";
    case SampleRegionRefusalReason::NonZeroCompensatableLatency:
        return "NonZeroCompensatableLatency";
    case SampleRegionRefusalReason::DuplicatePromotedParameter:
        return "DuplicatePromotedParameter";
    case SampleRegionRefusalReason::ParameterContractMismatch:
        return "ParameterContractMismatch";
    case SampleRegionRefusalReason::RegionLimitExceeded:
        return "RegionLimitExceeded";
    case SampleRegionRefusalReason::NodeLimitExceeded:
        return "NodeLimitExceeded";
    case SampleRegionRefusalReason::ConnectionLimitExceeded:
        return "ConnectionLimitExceeded";
    case SampleRegionRefusalReason::DelayStateLimitExceeded:
        return "DelayStateLimitExceeded";
    case SampleRegionRefusalReason::ParameterLimitExceeded:
        return "ParameterLimitExceeded";
    case SampleRegionRefusalReason::StateBudgetExceeded:
        return "StateBudgetExceeded";
    case SampleRegionRefusalReason::BoundaryBufferBudgetExceeded:
        return "BoundaryBufferBudgetExceeded";
    case SampleRegionRefusalReason::WorkBudgetExceeded:
        return "WorkBudgetExceeded";
    case SampleRegionRefusalReason::ArithmeticOverflow:
        return "ArithmeticOverflow";
    case SampleRegionRefusalReason::PrepareFailed:
        return "PrepareFailed";
    }
    return "PrepareFailed";
}
Value proof(const SampleRegionProof& p, const SampleRegionDefinition& definition) {
    auto out = object();
    out.setMember("accepted", p.accepted);
    out.setMember("code", std::string(p.accepted ? "Accepted" : reason_name(p.reason)));
    if (!p.accepted) {
        out.setMember("path", "/regions/" + std::to_string(p.region_id));
        if (!p.message.empty())
            out.setMember("message", p.message.substr(0, 512));
        if (p.offending_node)
            number(out, "offending_node_id", p.offending_node);
        const auto member = [&](NodeId node) {
            return std::any_of(definition.members.begin(), definition.members.end(),
                               [&](const auto& entry) { return entry.node == node; });
        };
        if (p.has_offending_connection)
            out.setMember("offending_connection",
                          connection(p.offending_connection.source,
                                     p.offending_connection.source_port,
                                     p.offending_connection.destination,
                                     p.offending_connection.destination_port,
                                     member(p.offending_connection.source) !=
                                         member(p.offending_connection.destination)));
    }
    return out;
}
Value definition(const SampleRegionDefinition& d, const std::vector<Connection>& edges) {
    auto out = object();
    auto members = choc::value::createEmptyArray();
    std::set<NodeId> ids;
    auto sorted = d.members;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.node < b.node; });
    for (const auto& m : sorted) {
        ids.insert(m.node);
        auto item = object();
        number(item, "node_id", m.node);
        item.setMember("type_id", m.type_id);
        number(item, "type_version", m.version);
        item.setMember("config", config(m.config));
        members.addArrayElement(item);
    }
    out.setMember("members", members);
    auto inputs = choc::value::createEmptyArray(), outputs = choc::value::createEmptyArray();
    for (auto id : d.input_boundaries)
        inputs.addArrayElement(static_cast<std::int64_t>(id));
    for (auto id : d.output_boundaries)
        outputs.addArrayElement(static_cast<std::int64_t>(id));
    out.setMember("input_boundaries", inputs);
    out.setMember("output_boundaries", outputs);
    auto connections = choc::value::createEmptyArray();
    auto ordered_edges = edges;
    std::sort(ordered_edges.begin(), ordered_edges.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_node, a.source_port, a.dest_node, a.dest_port) <
               std::tie(b.source_node, b.source_port, b.dest_node, b.dest_port);
    });
    for (const auto& e : ordered_edges) {
        const bool source = ids.contains(e.source_node), dest = ids.contains(e.dest_node);
        if (source || dest)
            connections.addArrayElement(
                connection(e.source_node, e.source_port, e.dest_node, e.dest_port, source != dest));
    }
    out.setMember("connections", connections);
    auto parameters = choc::value::createEmptyArray();
    auto promoted = d.promoted_parameters;
    std::sort(promoted.begin(), promoted.end(),
              [](const auto& a, const auto& b) { return a.param_id < b.param_id; });
    for (const auto& p : promoted) {
        auto item = object();
        number(item, "param_id", p.param_id);
        item.setMember("key", p.key);
        item.setMember("name", p.name);
        item.setMember("unit", p.unit);
        item.setMember("minimum", p.range.min);
        item.setMember("maximum", p.range.max);
        item.setMember("default_value", p.range.default_value);
        item.setMember("step", p.range.step);
        item.setMember("skew", p.range.skew);
        item.setMember("symmetric_skew", p.range.symmetric_skew);
        item.setMember("rate", "control");
        item.setMember("smoothing_ramp_seconds", 0);
        number(item, "bound_node_id", p.bound_node_id);
        number(item, "bound_port", p.bound_port);
        parameters.addArrayElement(item);
    }
    out.setMember("promoted_parameters", parameters);
    out.setMember("limits", limits(d.limits));
    return out;
}
bool canonical_request(const ControlRequestEnvelope& request) {
    const auto* op = resolve_control_operation(request.operation_id, request.operation_version);
    return op && validate_control_json_schema(request.params_json, op->input_schema_json);
}
bool exact_float(View value, float& result) {
    const auto number = value.getWithDefault<double>(std::numeric_limits<double>::quiet_NaN());
    result = static_cast<float>(number);
    return std::isfinite(number) && std::isfinite(result) && static_cast<double>(result) == number;
}
bool parse_config(View value, SampleKernelConfig& result) {
    const auto kind = value["kind"].getString();
    if (kind == "none")
        result.kind = SampleKernelConfigKind::None;
    else if (kind == "boundary_index") {
        result.kind = SampleKernelConfigKind::BoundaryIndex;
        result.boundary_index_or_parameter_id =
            static_cast<std::uint32_t>(value["boundary_index"].getInt64());
    } else if (kind == "promoted_parameter_id") {
        result.kind = SampleKernelConfigKind::PromotedParameterId;
        result.boundary_index_or_parameter_id =
            static_cast<std::uint32_t>(value["promoted_parameter_id"].getInt64());
    } else if (kind == "finite_constant") {
        result.kind = SampleKernelConfigKind::FiniteConstant;
        return exact_float(value["value"], result.constant);
    } else
        return false;
    return true;
}
} // namespace

struct ControlSampleRegionTarget::Impl {
    host::SignalGraph* graph = nullptr;
    const host::BakedPlan* plan = nullptr;
    state::StateStore* store = nullptr;
    ControlSampleRegionGeneration* generation = nullptr;
    FrozenProof frozen_proof;
    std::function<bool()> frozen_prepared;
    double sample_rate = 0;
    int max_block_size = 0;
};
ControlSampleRegionTarget::ControlSampleRegionTarget(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ControlSampleRegionTarget::~ControlSampleRegionTarget() = default;
std::shared_ptr<ControlSampleRegionTarget>
ControlSampleRegionTarget::editable(host::SignalGraph& graph, state::StateStore& store,
                                    ControlSampleRegionGeneration& generation) {
    auto impl = std::make_unique<Impl>();
    impl->graph = &graph;
    impl->store = &store;
    impl->generation = &generation;
    return std::shared_ptr<ControlSampleRegionTarget>(
        new ControlSampleRegionTarget(std::move(impl)));
}
std::shared_ptr<ControlSampleRegionTarget>
ControlSampleRegionTarget::frozen(const host::BakedPlan& plan, state::StateStore& store,
                                  ControlSampleRegionGeneration& generation, FrozenProof proof,
                                  std::function<bool()> prepared) {
    if (plan.format_version != 2 || !proof || !prepared)
        return {};
    auto impl = std::make_unique<Impl>();
    impl->plan = &plan;
    impl->store = &store;
    impl->generation = &generation;
    impl->frozen_proof = std::move(proof);
    impl->frozen_prepared = std::move(prepared);
    return std::shared_ptr<ControlSampleRegionTarget>(
        new ControlSampleRegionTarget(std::move(impl)));
}
bool ControlSampleRegionTarget::can_read() const noexcept {
    return impl_ && impl_->store && impl_->generation &&
           (impl_->graph || (impl_->plan && impl_->frozen_proof && impl_->frozen_prepared));
}
bool ControlSampleRegionTarget::uses_state_store(const state::StateStore& store) const noexcept {
    return impl_ && impl_->store == &store;
}
bool ControlSampleRegionTarget::can_edit() const noexcept {
    return can_read() && impl_->graph;
}
void ControlSampleRegionTarget::set_preparation_context(double rate, int maximum) noexcept {
    impl_->sample_rate = rate;
    impl_->max_block_size = maximum > 0 ? maximum
                                        : (impl_->graph ? impl_->graph->prepared_max_block_size()
                                                        : impl_->max_block_size);
}

ControlExecutionOutcome ControlSampleRegionTarget::read(const ControlAdmissionPlan&,
                                                        const ControlRequestEnvelope& request,
                                                        const ControlExecutionContext& context) {
    if (auto stopped = checkpoint(context))
        return *stopped;
    if (!can_read() || !canonical_request(request))
        return refusal(ControlResultCode::InvalidRequest, "invalid region read request");
    if (impl_->generation->value < 1 || impl_->generation->value > max_generation)
        return refusal(ControlResultCode::ResourceExhausted,
                       "graph generation exceeds wire bounds");
    try {
        const auto params = choc::json::parse(request.params_json);
        const auto wanted =
            params.hasObjectMember("region_id") ? params["region_id"].getInt64() : 0;
        const auto include = params["include_definition"].getWithDefault<bool>(false);
        std::vector<SampleRegionDefinition> definitions;
        std::vector<Connection> edges;
        if (impl_->graph) {
            for (const auto& d : impl_->graph->sample_regions())
                definitions.push_back(d);
            edges = impl_->graph->connections();
        } else {
            definitions = impl_->plan->sample_regions;
            for (const auto& e : impl_->plan->connections)
                edges.push_back({e.src_node, static_cast<PortIndex>(e.src_port), e.dst_node,
                                 static_cast<PortIndex>(e.dst_port), e.feedback});
        }
        if (definitions.size() > 16)
            return refusal(ControlResultCode::ResourceExhausted, "region count exceeds wire bound");
        std::sort(definitions.begin(), definitions.end(),
                  [](const auto& a, const auto& b) { return a.region_id < b.region_id; });
        auto result = object();
        number(result, "graph_generation", impl_->generation->value);
        auto regions = choc::value::createEmptyArray();
        for (const auto& d : definitions) {
            if (wanted && wanted != d.region_id)
                continue;
            const auto p = impl_->graph ? impl_->graph->prove_sample_region(d.region_id)
                                        : impl_->frozen_proof(d.region_id, impl_->max_block_size);
            if (!resources_fit_wire(p.resources))
                return refusal(ControlResultCode::ResourceExhausted,
                               "region resources exceed closed wire bounds");
            auto item = object();
            number(item, "region_id", d.region_id);
            if (d.input_boundaries.empty())
                return refusal(ControlResultCode::InternalError, "region has no anchor");
            number(item, "anchor_node_id", d.input_boundaries.front());
            item.setMember("prepared",
                           impl_->graph ? impl_->graph->is_prepared() : impl_->frozen_prepared());
            item.setMember("proof", proof(p, d));
            item.setMember("resources", resources(p.resources));
            auto ids = choc::value::createEmptyArray();
            for (const auto& parameter : d.promoted_parameters)
                ids.addArrayElement(static_cast<std::int64_t>(parameter.param_id));
            item.setMember("promoted_parameter_ids", ids);
            if (include)
                item.setMember("definition", definition(d, edges));
            regions.addArrayElement(item);
        }
        if (wanted && regions.size() == 0)
            return refusal(ControlResultCode::InvalidRequest, "region does not exist", "/region_id",
                           "UnknownRegion");
        result.setMember("regions", regions);
        const auto json = choc::json::toString(result);
        const auto* op = resolve_control_operation(request.operation_id, 1);
        if (!validate_control_output_json_schema(json, op->output_schema_json))
            return refusal(ControlResultCode::ResourceExhausted,
                           "region response exceeds its closed wire contract");
        if (auto stopped = checkpoint(context))
            return *stopped;
        return {.terminal_state = ControlReceiptState::Completed, .result = {.detail_json = json}};
    } catch (...) {
        return refusal(ControlResultCode::InvalidRequest, "region read decoding failed");
    }
}

ControlExecutionOutcome ControlSampleRegionTarget::edit(const ControlAdmissionPlan& plan,
                                                        const ControlRequestEnvelope& request,
                                                        const ControlExecutionContext& context) {
    if (auto stopped = checkpoint(context))
        return *stopped;
    if (!can_edit() || !canonical_request(request))
        return refusal(ControlResultCode::InvalidRequest, "invalid or unavailable region edit");
    bool published = false;
    try {
        const auto params = choc::json::parse(request.params_json);
        const auto region = static_cast<SampleRegionId>(params["region_id"].getInt64());
        const auto expected =
            static_cast<std::uint64_t>(params["expected_graph_generation"].getInt64());
        if (impl_->generation->value != expected)
            return refusal(ControlResultCode::StateConflict, "graph generation changed",
                           "/expected_graph_generation");
        if (expected >= max_generation)
            return refusal(ControlResultCode::ResourceExhausted, "graph generation exhausted",
                           "/expected_graph_generation");
        if (!impl_->graph->sample_region(region))
            return refusal(ControlResultCode::InvalidRequest, "region does not exist", "/region_id",
                           "UnknownRegion");
        if (!impl_->graph->is_prepared() || !std::isfinite(impl_->sample_rate) ||
            impl_->sample_rate <= 0 || impl_->max_block_size <= 0)
            return refusal(ControlResultCode::HostUnavailable,
                           "region graph has no valid preparation context");
        auto edit = impl_->graph->begin_prepared_topology_edit();
        if (!edit)
            return refusal(ControlResultCode::HostUnavailable,
                           "private topology candidate is unavailable");
        std::map<std::string, NodeId> temporary;
        const auto actions = params["actions"];
        for (std::uint32_t index = 0; index < actions.size(); ++index) {
            const auto action = actions[index];
            const auto op = action["op"].getString();
            const auto path = "/actions/" + std::to_string(index);
            auto failed = [&](std::string message, std::string reason = "InvalidKernelConfig") {
                return refusal(ControlResultCode::InvalidRequest, std::move(message), path,
                               std::move(reason));
            };
            const auto current = edit->sample_region(region);
            if (!current)
                return failed("candidate region disappeared", "UnknownRegion");
            const auto member = [&](NodeId id) -> const SampleRegionKernelNode* {
                const auto it = std::find_if(current->members.begin(), current->members.end(),
                                             [&](const auto& m) { return m.node == id; });
                return it == current->members.end() ? nullptr : &*it;
            };
            const auto boundary = [&](NodeId id) {
                return std::find(current->input_boundaries.begin(), current->input_boundaries.end(),
                                 id) != current->input_boundaries.end() ||
                       std::find(current->output_boundaries.begin(),
                                 current->output_boundaries.end(),
                                 id) != current->output_boundaries.end();
            };
            const auto stable = [&](const char* key) {
                return static_cast<NodeId>(action[key].getInt64());
            };
            if (op == "add_supported_kernel") {
                const std::string name(action["temporary_node_id"].getString());
                const std::string type(action["type_id"].getString());
                const auto version = action["type_version"].getInt64();
                static const std::set<std::string_view> supported{
                    "pulp.core.sample-region.constant", "pulp.core.sample-region.parameter",
                    "pulp.core.sample-region.add", "pulp.core.sample-region.multiply",
                    "pulp.core.unit-delay"};
                if (!supported.contains(type) || version != 1)
                    return failed("kernel identity is outside the editable cohort",
                                  "UnsupportedNodeKind");
                if (temporary.contains(name))
                    return failed("temporary identity was already declared");
                SampleKernelConfig authored;
                if (!parse_config(action["config"], authored))
                    return failed("constant is not an exact finite float");
                const auto* descriptor = impl_->graph->sample_kernel_type(type, 1);
                if (!descriptor || !sample_region_config_matches(authored, *descriptor))
                    return failed("kernel configuration does not match its exact descriptor");
                const auto display = action.hasObjectMember("display_name")
                                         ? std::string(action["display_name"].getString())
                                         : std::string{};
                const auto id = edit->add_custom_node(type, 1, display);
                if (!id || !edit->add_sample_region_member(region, id, authored).accepted)
                    return failed("kernel could not be added to the candidate");
                temporary.emplace(name, id);
            } else if (op == "remove_kernel") {
                const auto id = stable("node_id");
                if (!member(id))
                    return failed("node is not a region member", "UnknownMember");
                if (boundary(id))
                    return failed("builder-owned boundary cannot be removed", "InvalidBoundary");
                const auto edges = edit->connections();
                for (const auto& edge : edges) {
                    if (edge.source_node != id && edge.dest_node != id)
                        continue;
                    if (!member(edge.source_node) || !member(edge.dest_node))
                        return failed("crossing connection cannot be removed",
                                      "InvalidBoundaryCrossing");
                    if (!edit->disconnect(edge.source_node, edge.source_port, edge.dest_node,
                                          edge.dest_port))
                        return failed("incident edge could not be removed");
                }
                if (!edit->remove_sample_region_member(region, id).accepted ||
                    !edit->remove_node(id))
                    return failed("member removal violates the frozen region contract",
                                  "ParameterContractMismatch");
            } else if (op == "connect" || op == "disconnect") {
                const auto endpoint = [&](const char* stable_key,
                                          const char* temporary_key) -> NodeId {
                    if (action.hasObjectMember(stable_key))
                        return stable(stable_key);
                    if (!action.hasObjectMember(temporary_key))
                        return 0;
                    const auto it = temporary.find(std::string(action[temporary_key].getString()));
                    return it == temporary.end() ? 0 : it->second;
                };
                const auto source = endpoint("source_node_id", "source_temporary_node_id");
                const auto dest = endpoint("destination_node_id", "destination_temporary_node_id");
                if (!source || !dest || !member(source) || !member(dest))
                    return failed(
                        "endpoint is absent, forward-referenced, removed, or outside the region",
                        "UnknownMember");
                const auto source_port = static_cast<PortIndex>(action["source_port"].getInt64());
                const auto dest_port =
                    static_cast<PortIndex>(action["destination_port"].getInt64());
                const auto& edges = edit->connections();
                const bool present = std::any_of(edges.begin(), edges.end(), [&](const auto& e) {
                    return e.source_node == source && e.source_port == source_port &&
                           e.dest_node == dest && e.dest_port == dest_port;
                });
                if (op == "connect") {
                    if (present)
                        return failed("connection already exists", "InvalidProducerCardinality");
                    const auto result = edit->connect_in_sample_region(region, source, source_port,
                                                                       dest, dest_port);
                    if (!result.accepted)
                        return failed(result.message, std::string(reason_name(result.reason)));
                } else if (!present || !edit->disconnect(source, source_port, dest, dest_port)) {
                    return failed("exact connection is absent", "InvalidProducerCardinality");
                }
            } else if (op == "set_finite_constant") {
                const auto id = stable("node_id");
                const auto* m = member(id);
                if (!m || m->type_id != "pulp.core.sample-region.constant" || m->version != 1)
                    return failed("node is not an exact constant kernel", "UnknownMember");
                SampleKernelConfig authored{.kind = SampleKernelConfigKind::FiniteConstant};
                if (!exact_float(action["value"], authored.constant))
                    return failed("value is not an exact finite float");
                const auto result = edit->set_sample_kernel_config(region, id, authored);
                if (!result.accepted)
                    return failed(result.message, std::string(reason_name(result.reason)));
            } else if (op == "change_boundary_mapping") {
                const auto id = stable("node_id");
                const auto* m = member(id);
                const auto kind = action["boundary_kind"].getString();
                const auto type = kind == "input" ? "pulp.core.sample-region.input"
                                                  : "pulp.core.sample-region.output";
                if (!m || !boundary(id) || m->type_id != type)
                    return failed("boundary kind does not match the addressed node",
                                  "InvalidBoundary");
                SampleKernelConfig authored{
                    .kind = SampleKernelConfigKind::BoundaryIndex,
                    .boundary_index_or_parameter_id =
                        static_cast<std::uint32_t>(action["boundary_index"].getInt64())};
                const auto result = edit->set_sample_kernel_config(region, id, authored);
                if (!result.accepted)
                    return failed(result.message, std::string(reason_name(result.reason)));
            } else
                return failed("unsupported action");
        }
        const auto candidate_proof = edit->prove_sample_region(region);
        if (!candidate_proof.accepted)
            return refusal(ControlResultCode::InvalidRequest, candidate_proof.message, "/actions",
                           std::string(reason_name(candidate_proof.reason)));
        if (edit->prepare(impl_->sample_rate, impl_->max_block_size) !=
            SignalGraph::PreparedTopologyEdit::Result::Prepared)
            return refusal(ControlResultCode::InvalidRequest,
                           "candidate failed preparation or frozen parameter contract", "/actions",
                           "PrepareFailed");
        const auto prepared_proof = edit->prove_sample_region(region);
        if (!prepared_proof.accepted || !resources_fit_wire(prepared_proof.resources))
            return refusal(ControlResultCode::ResourceExhausted,
                           "prepared region exceeds receipt bounds", "/actions");
        // Preparation can run long enough for cancellation or lease revocation.
        if (auto stopped = checkpoint(context))
            return *stopped;
        if (impl_->generation->value != expected)
            return refusal(ControlResultCode::StateConflict,
                           "graph generation changed during prepare", "/expected_graph_generation");
        if (edit->commit() != SignalGraph::PreparedTopologyEdit::Result::Committed)
            return refusal(ControlResultCode::StateConflict, "candidate was stale at publication",
                           "/expected_graph_generation");
        published = true;
        ++impl_->generation->value;
        auto receipt = object();
        receipt.setMember("receipt_id", plan.receipt_id.value);
        receipt.setMember("applied", true);
        number(receipt, "region_id", region);
        number(receipt, "old_graph_generation", expected);
        number(receipt, "new_graph_generation", impl_->generation->value);
        number(receipt, "applied_action_count", actions.size());
        auto mapping = choc::value::createEmptyArray();
        for (const auto& [name, id] : temporary) {
            auto item = object();
            item.setMember("temporary_node_id", name);
            number(item, "node_id", id);
            mapping.addArrayElement(item);
        }
        receipt.setMember("node_mapping", mapping);
        std::uint32_t retained = 0, fresh = 0;
        auto stats = candidate_proof.resources;
        for (const auto& runtime : impl_->graph->sample_region_runtime_receipts()) {
            if (runtime.region_id != region)
                continue;
            stats = runtime.resources;
            retained = runtime.retained_state_cells;
            fresh = runtime.fresh_state_cells;
        }
        receipt.setMember("resources", resources(stats));
        number(receipt, "state_retained_count", retained);
        number(receipt, "state_reset_count", fresh);
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = choc::json::toString(receipt)}};
    } catch (...) {
        if (published)
            return {.terminal_state = ControlReceiptState::UnknownNeedsRefresh,
                    .result = {.result_code = ControlResultCode::UnknownNeedsRefresh,
                               .explanation = "region published but receipt construction failed"}};
        return refusal(ControlResultCode::InternalError,
                       "region candidate allocation or decoding failed");
    }
}
} // namespace pulp::inspect
