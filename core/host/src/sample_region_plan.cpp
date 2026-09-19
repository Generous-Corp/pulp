#include <pulp/host/sample_region_plan.hpp>

#include <algorithm>
#include <map>
#include <queue>

namespace pulp::host {
namespace {

struct ResolvedKernelSet {
    std::vector<SampleKernelDescriptor> descriptors;

    static const SampleKernelDescriptor* resolve(const void* context, std::string_view type_id,
                                                 int version) noexcept {
        const auto& self = *static_cast<const ResolvedKernelSet*>(context);
        const auto found =
            std::find_if(self.descriptors.begin(), self.descriptors.end(), [&](const auto& value) {
                return value.type_id == type_id && value.version == version;
            });
        return found == self.descriptors.end() ? nullptr : &*found;
    }
};

PreparedSampleKernelConfig prepare_config(const SampleKernelConfig& authored,
                                          const std::vector<state::ParamID>& parameters) {
    PreparedSampleKernelConfig prepared;
    switch (authored.kind) {
    case SampleKernelConfigKind::None:
        prepared.kind = PreparedSampleKernelConfigKind::None;
        break;
    case SampleKernelConfigKind::BoundaryIndex:
        prepared.kind = PreparedSampleKernelConfigKind::BoundaryIndex;
        prepared.boundary_or_parameter_index = authored.boundary_index_or_parameter_id;
        break;
    case SampleKernelConfigKind::FiniteConstant:
        prepared.kind = PreparedSampleKernelConfigKind::FiniteConstant;
        prepared.constant = authored.constant;
        break;
    case SampleKernelConfigKind::PromotedParameterId: {
        prepared.kind = PreparedSampleKernelConfigKind::PromotedParameterIndex;
        const auto found =
            std::lower_bound(parameters.begin(), parameters.end(),
                             static_cast<state::ParamID>(authored.boundary_index_or_parameter_id));
        prepared.boundary_or_parameter_index =
            static_cast<std::uint32_t>(found - parameters.begin());
        break;
    }
    case SampleKernelConfigKind::Invalid:
        break;
    }
    return prepared;
}

} // namespace

SampleRegionPlanResult build_sample_region_plan(const SampleRegionCandidate& candidate) {
    ResolvedKernelSet resolved;
    resolved.descriptors.reserve(candidate.members.size());
    for (const auto& member : candidate.members) {
        const auto already_resolved = std::any_of(
            resolved.descriptors.begin(), resolved.descriptors.end(), [&](const auto& value) {
                return value.type_id == member.type_id && value.version == member.version;
            });
        if (already_resolved)
            continue;
        const auto* descriptor = candidate.registry.find(member.type_id, member.version);
        if (descriptor != nullptr)
            resolved.descriptors.push_back(*descriptor);
    }
    auto frozen = candidate;
    frozen.registry = {&resolved, ResolvedKernelSet::resolve};

    SampleRegionPlanResult result;
    result.proof = prove_sample_region(frozen);
    if (!result.proof.accepted)
        return result;

    PreparedSampleRegionPlan plan;
    plan.region_id = candidate.region_id;
    plan.resources = result.proof.resources;
    plan.promoted_parameters = candidate.promoted_parameters;
    std::sort(plan.promoted_parameters.begin(), plan.promoted_parameters.end());

    std::vector<const SampleRegionKernelNode*> ordered;
    ordered.reserve(candidate.members.size());
    for (const auto& member : candidate.members)
        ordered.push_back(&member);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto* lhs, const auto* rhs) { return lhs->node < rhs->node; });

    std::map<NodeId, std::size_t> plan_index;
    for (const auto* member : ordered) {
        PreparedSampleKernelBinding binding;
        binding.node = member->node;
        binding.descriptor = *frozen.registry.find(member->type_id, member->version);
        binding.config = prepare_config(member->config, plan.promoted_parameters);
        binding.input_slot_offset = plan.scalar_slot_count;
        plan.scalar_slot_count += binding.descriptor.num_input_ports;
        binding.output_slot_offset = plan.scalar_slot_count;
        plan.scalar_slot_count += binding.descriptor.num_output_ports;
        plan_index.emplace(binding.node, plan.kernels.size());
        plan.kernels.push_back(std::move(binding));
    }

    for (std::size_t i = 0; i < plan.kernels.size(); ++i) {
        if (plan.kernels[i].descriptor.causality == SampleKernelCausality::OneSampleDelay) {
            plan.delay_publish_order.push_back(i);
            plan.delay_commit_order.push_back(i);
        }
    }

    std::vector<std::uint32_t> indegree(plan.kernels.size());
    std::vector<std::vector<std::size_t>> dependencies(plan.kernels.size());
    for (const auto& connection : candidate.connections) {
        const auto source = plan_index.find(connection.source);
        const auto destination = plan_index.find(connection.destination);
        if (source == plan_index.end() || destination == plan_index.end())
            continue;

        PreparedSampleRegionTransfer transfer;
        transfer.source = connection.source;
        transfer.source_port = connection.source_port;
        transfer.destination = connection.destination;
        transfer.destination_port = connection.destination_port;
        transfer.source_slot =
            plan.kernels[source->second].output_slot_offset + connection.source_port;
        transfer.destination_slot =
            plan.kernels[destination->second].input_slot_offset + connection.destination_port;
        plan.transfers.push_back(transfer);

        const auto source_delay = plan.kernels[source->second].descriptor.causality ==
                                  SampleKernelCausality::OneSampleDelay;
        const auto destination_delay = plan.kernels[destination->second].descriptor.causality ==
                                       SampleKernelCausality::OneSampleDelay;
        if (!source_delay && !destination_delay) {
            dependencies[source->second].push_back(destination->second);
            ++indegree[destination->second];
        }
    }
    std::sort(plan.transfers.begin(), plan.transfers.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.destination != rhs.destination)
            return lhs.destination < rhs.destination;
        if (lhs.destination_port != rhs.destination_port)
            return lhs.destination_port < rhs.destination_port;
        if (lhs.source != rhs.source)
            return lhs.source < rhs.source;
        return lhs.source_port < rhs.source_port;
    });

    using Ready = std::pair<NodeId, std::size_t>;
    std::priority_queue<Ready, std::vector<Ready>, std::greater<>> ready;
    for (std::size_t i = 0; i < plan.kernels.size(); ++i) {
        if (plan.kernels[i].descriptor.causality == SampleKernelCausality::Combinational &&
            indegree[i] == 0) {
            ready.emplace(plan.kernels[i].node, i);
        }
    }
    while (!ready.empty()) {
        const auto [node, index] = ready.top();
        static_cast<void>(node);
        ready.pop();
        plan.combinational_order.push_back(index);
        for (const auto dependent : dependencies[index]) {
            if (--indegree[dependent] == 0)
                ready.emplace(plan.kernels[dependent].node, dependent);
        }
    }

    const auto append_transfers = [&](NodeId source) {
        for (std::size_t i = 0; i < plan.transfers.size(); ++i) {
            if (plan.transfers[i].source == source) {
                plan.operations.push_back({PreparedSampleRegionOperationKind::Transfer, i});
            }
        }
    };
    for (const auto index : plan.delay_publish_order) {
        plan.operations.push_back({PreparedSampleRegionOperationKind::DelayPublish, index});
        append_transfers(plan.kernels[index].node);
    }
    for (const auto index : plan.combinational_order) {
        plan.operations.push_back({PreparedSampleRegionOperationKind::Process, index});
        append_transfers(plan.kernels[index].node);
    }
    for (const auto index : plan.delay_commit_order)
        plan.operations.push_back({PreparedSampleRegionOperationKind::DelayCommit, index});

    result.plan = std::move(plan);
    return result;
}

} // namespace pulp::host
