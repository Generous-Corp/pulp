// Persisted sample-region validation for GraphSerializer loads: authored
// limits, member kernel contracts, boundary and promoted-parameter bindings,
// parser shape accounting and per-region topology/causality checks. These
// read the graph through its public queries only and never mutate it.

#include "graph_serializer_regions.hpp"

#include <pulp/host/sample_region_parameters.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::host::detail {
namespace {

bool persisted_region_limits_within_v1(const SampleRegionLimits& value) noexcept {
    const auto cap = SampleRegionLimits::v1();
    return value.max_member_nodes <= cap.max_member_nodes &&
           value.max_internal_connections <= cap.max_internal_connections &&
           value.max_input_boundaries <= cap.max_input_boundaries &&
           value.max_output_boundaries <= cap.max_output_boundaries &&
           value.max_delay_nodes <= cap.max_delay_nodes &&
           value.max_promoted_parameters <= cap.max_promoted_parameters &&
           value.max_state_bytes <= cap.max_state_bytes &&
           value.max_logical_boundary_bytes <= cap.max_logical_boundary_bytes &&
           value.max_work_per_frame <= cap.max_work_per_frame &&
           value.max_work_per_block <= cap.max_work_per_block;
}

bool valid_persisted_kernel_config(const SampleKernelConfig& config) noexcept {
    switch (config.kind) {
    case SampleKernelConfigKind::None:
        return config.boundary_index_or_parameter_id == 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::BoundaryIndex:
        return config.constant == 0.0f;
    case SampleKernelConfigKind::FiniteConstant:
        return config.boundary_index_or_parameter_id == 0 && std::isfinite(config.constant);
    case SampleKernelConfigKind::PromotedParameterId:
        return config.boundary_index_or_parameter_id != 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

struct PersistedV1KernelContract {
    std::uint32_t inputs;
    std::uint32_t outputs;
    SampleKernelConfigKind config_kind;
    SampleKernelCausality causality;
};

std::optional<PersistedV1KernelContract> persisted_v1_kernel_contract(std::string_view type_id,
                                                                      int version) {
    if (version != 1)
        return std::nullopt;
    using Kind = SampleKernelConfigKind;
    using Causality = SampleKernelCausality;
    if (type_id == "pulp.core.sample-region.input" || type_id == "pulp.core.sample-region.output")
        return PersistedV1KernelContract{1, 1, Kind::BoundaryIndex, Causality::Combinational};
    if (type_id == "pulp.core.sample-region.constant")
        return PersistedV1KernelContract{0, 1, Kind::FiniteConstant, Causality::Combinational};
    if (type_id == "pulp.core.sample-region.parameter")
        return PersistedV1KernelContract{0, 1, Kind::PromotedParameterId, Causality::Combinational};
    if (type_id == "pulp.core.sample-region.add" || type_id == "pulp.core.sample-region.multiply")
        return PersistedV1KernelContract{2, 1, Kind::None, Causality::Combinational};
    if (type_id == "pulp.core.unit-delay")
        return PersistedV1KernelContract{1, 1, Kind::None, Causality::OneSampleDelay};
    return std::nullopt;
}

} // namespace

bool validate_persisted_region_metadata(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::string& error) {
    const auto parameters = SampleRegionParameterContract::from_regions(definitions);
    if (!parameters.valid()) {
        error = "invalid promoted parameter metadata";
        return false;
    }

    for (const auto& definition : definitions) {
        if (!persisted_region_limits_within_v1(definition.limits) ||
            definition.members.size() > definition.limits.max_member_nodes ||
            definition.input_boundaries.size() > definition.limits.max_input_boundaries ||
            definition.output_boundaries.size() > definition.limits.max_output_boundaries ||
            definition.promoted_parameters.size() > definition.limits.max_promoted_parameters) {
            error = "sample region metadata exceeds authored limits";
            return false;
        }

        std::unordered_map<NodeId, const SampleRegionKernelNode*> members;
        std::size_t delay_nodes = 0;
        for (const auto& member : definition.members) {
            const auto* current = graph.node(member.node);
            const auto* descriptor = graph.sample_kernel_type(member.type_id, member.version);
            const auto contract = persisted_v1_kernel_contract(member.type_id, member.version);
            if (member.node == 0 || !members.emplace(member.node, &member).second ||
                current == nullptr || current->type != NodeType::Custom ||
                current->custom_type_id != member.type_id ||
                current->custom_type_version != member.version ||
                !valid_persisted_kernel_config(member.config) ||
                (descriptor != nullptr &&
                 (!is_sample_region_v1_descriptor(*descriptor) ||
                  current->num_input_ports != static_cast<int>(descriptor->num_input_ports) ||
                  current->num_output_ports != static_cast<int>(descriptor->num_output_ports) ||
                  !sample_region_config_matches(member.config, *descriptor))) ||
                (descriptor == nullptr && contract.has_value() &&
                 (current->num_input_ports != static_cast<int>(contract->inputs) ||
                  current->num_output_ports != static_cast<int>(contract->outputs) ||
                  member.config.kind != contract->config_kind))) {
                error = "invalid or duplicate sample region member";
                return false;
            }
            if (member.type_id == "pulp.core.unit-delay")
                ++delay_nodes;
        }
        if (delay_nodes > definition.limits.max_delay_nodes) {
            error = "sample region metadata exceeds authored limits";
            return false;
        }

        const auto validate_boundaries = [&](std::span<const NodeId> boundaries,
                                             std::string_view type) {
            if (boundaries.empty())
                return false;
            std::unordered_set<NodeId> unique;
            std::unordered_set<std::uint32_t> indexes;
            for (const auto id : boundaries) {
                const auto found = members.find(id);
                if (!unique.insert(id).second || found == members.end() ||
                    found->second->type_id != type ||
                    found->second->config.kind != SampleKernelConfigKind::BoundaryIndex ||
                    found->second->config.constant != 0.0f ||
                    !indexes.insert(found->second->config.boundary_index_or_parameter_id).second) {
                    return false;
                }
            }
            for (std::uint32_t expected = 0; expected < indexes.size(); ++expected)
                if (!indexes.contains(expected))
                    return false;
            for (const auto& member : definition.members) {
                if (member.type_id == type && !unique.contains(member.node))
                    return false;
            }
            return true;
        };
        if (!validate_boundaries(definition.input_boundaries, "pulp.core.sample-region.input") ||
            !validate_boundaries(definition.output_boundaries, "pulp.core.sample-region.output")) {
            error = "invalid sample region boundary metadata";
            return false;
        }

        std::unordered_set<state::ParamID> bound_parameters;
        for (const auto& parameter : definition.promoted_parameters) {
            const auto found = members.find(parameter.bound_node_id);
            if (found == members.end() ||
                found->second->type_id != "pulp.core.sample-region.parameter" ||
                found->second->config.kind != SampleKernelConfigKind::PromotedParameterId ||
                found->second->config.boundary_index_or_parameter_id != parameter.param_id ||
                found->second->config.constant != 0.0f ||
                !bound_parameters.insert(parameter.param_id).second) {
                error = "invalid promoted parameter binding";
                return false;
            }
        }
        for (const auto& member : definition.members) {
            if (member.type_id == "pulp.core.sample-region.parameter" &&
                !bound_parameters.contains(member.config.boundary_index_or_parameter_id)) {
                error = "invalid promoted parameter binding";
                return false;
            }
        }
    }
    return true;
}

void accumulate_parser_region_shape(const SignalGraph& graph,
                                    const SampleRegionDefinition& definition,
                                    SampleRegionParserShape& shape) {
    const auto add = [&](std::uint64_t value, std::uint64_t& total) {
        if (value > std::numeric_limits<std::uint64_t>::max() - total) {
            shape.arithmetic_overflow = true;
            return;
        }
        total += value;
    };

    shape.members_per_region =
        std::max(shape.members_per_region, (std::uint64_t)definition.members.size());
    add(definition.members.size(), shape.members_total);
    shape.input_boundaries_per_region = std::max(shape.input_boundaries_per_region,
                                                 (std::uint64_t)definition.input_boundaries.size());
    shape.output_boundaries_per_region = std::max(
        shape.output_boundaries_per_region, (std::uint64_t)definition.output_boundaries.size());
    add(definition.input_boundaries.size(), shape.input_boundaries_total);
    add(definition.output_boundaries.size(), shape.output_boundaries_total);
    shape.parameters_per_region =
        std::max(shape.parameters_per_region, (std::uint64_t)definition.promoted_parameters.size());
    add(definition.promoted_parameters.size(), shape.parameters_total);

    std::uint64_t delays = 0;
    for (const auto& member : definition.members) {
        const auto* descriptor = graph.sample_kernel_type(member.type_id, member.version);
        const bool canonical_delay =
            member.type_id == "pulp.core.unit-delay" && member.version == 1;
        if ((descriptor != nullptr &&
             descriptor->causality == SampleKernelCausality::OneSampleDelay) ||
            (descriptor == nullptr && canonical_delay)) {
            ++delays;
        }
        if (descriptor != nullptr) {
            shape.kernel_state_bytes =
                std::max(shape.kernel_state_bytes, (std::uint64_t)descriptor->state_size);
            shape.kernel_state_alignment =
                std::max(shape.kernel_state_alignment, (std::uint64_t)descriptor->state_alignment);
        } else if (canonical_delay) {
            shape.kernel_state_bytes =
                std::max(shape.kernel_state_bytes, (std::uint64_t)sizeof(float));
            shape.kernel_state_alignment =
                std::max(shape.kernel_state_alignment, (std::uint64_t)alignof(float));
        }
    }
    shape.delays_per_region = std::max(shape.delays_per_region, delays);
    add(delays, shape.delays_total);

    shape.state_bytes_per_region =
        std::max(shape.state_bytes_per_region, (std::uint64_t)definition.limits.max_state_bytes);
    shape.logical_boundary_bytes_per_region = std::max(
        shape.logical_boundary_bytes_per_region, definition.limits.max_logical_boundary_bytes);
    shape.work_per_frame_per_region = std::max(shape.work_per_frame_per_region,
                                               (std::uint64_t)definition.limits.max_work_per_frame);
    shape.work_per_block_per_region =
        std::max(shape.work_per_block_per_region, definition.limits.max_work_per_block);
    add(definition.limits.max_state_bytes, shape.state_bytes_total);
    add(definition.limits.max_logical_boundary_bytes, shape.logical_boundary_bytes_total);
    add(definition.limits.max_work_per_frame, shape.work_per_frame_total);
    add(definition.limits.max_work_per_block, shape.work_per_block_total);
}

bool validate_persisted_region_topology(const SignalGraph& graph,
                                        std::span<const SampleRegionDefinition> definitions,
                                        std::span<const Connection> connections,
                                        std::string& error) {
    const auto reject = [&] {
        error = "invalid persisted sample region topology";
        return false;
    };
    std::uint64_t graph_known_work = 0;
    for (const auto& definition : definitions) {
        std::unordered_map<NodeId, std::size_t> member_index;
        for (std::size_t i = 0; i < definition.members.size(); ++i)
            member_index.emplace(definition.members[i].node, i);
        const std::unordered_set<NodeId> input_boundaries(definition.input_boundaries.begin(),
                                                          definition.input_boundaries.end());
        const std::unordered_set<NodeId> output_boundaries(definition.output_boundaries.begin(),
                                                           definition.output_boundaries.end());
        std::vector<std::vector<std::size_t>> internal_producers;
        std::vector<std::size_t> internal_consumers(definition.members.size());
        std::vector<std::size_t> external_producers(definition.members.size());
        std::vector<std::vector<std::size_t>> forward(definition.members.size());
        std::vector<std::vector<std::size_t>> reverse(definition.members.size());
        internal_producers.reserve(definition.members.size());
        for (const auto& member : definition.members) {
            const auto* node = graph.node(member.node);
            if (node == nullptr || node->num_input_ports < 0)
                return reject();
            internal_producers.emplace_back(static_cast<std::size_t>(node->num_input_ports));
        }

        std::size_t internal_connections = 0;
        for (const auto& connection : connections) {
            const auto source = member_index.find(connection.source_node);
            const auto destination = member_index.find(connection.dest_node);
            const bool source_inside = source != member_index.end();
            const bool destination_inside = destination != member_index.end();
            if (!source_inside && !destination_inside)
                continue;
            if (connection.midi || connection.automation || connection.audio_rate_modulation ||
                connection.sidechain || connection.feedback)
                return reject();
            if (source_inside) {
                const auto* node = graph.node(connection.source_node);
                if (node == nullptr || node->num_output_ports <= 0 ||
                    connection.source_port >= static_cast<PortIndex>(node->num_output_ports))
                    return reject();
            }
            if (destination_inside) {
                const auto* node = graph.node(connection.dest_node);
                if (node == nullptr || node->num_input_ports <= 0 ||
                    connection.dest_port >= static_cast<PortIndex>(node->num_input_ports))
                    return reject();
            }
            if (source_inside && destination_inside) {
                ++internal_connections;
                ++internal_consumers[source->second];
                ++internal_producers[destination->second][connection.dest_port];
                forward[source->second].push_back(destination->second);
                reverse[destination->second].push_back(source->second);
            } else if (destination_inside) {
                if (!input_boundaries.contains(connection.dest_node))
                    return reject();
                ++external_producers[destination->second];
            } else if (!output_boundaries.contains(connection.source_node)) {
                return reject();
            }
        }
        if (internal_connections > definition.limits.max_internal_connections)
            return reject();

        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            const auto& member = definition.members[i];
            const auto producer_count = std::accumulate(internal_producers[i].begin(),
                                                        internal_producers[i].end(), std::size_t{});
            if (input_boundaries.contains(member.node)) {
                if (external_producers[i] != 1 || producer_count != 0)
                    return reject();
            } else if (output_boundaries.contains(member.node)) {
                if (producer_count != 1 || internal_consumers[i] != 0)
                    return reject();
            } else if (std::any_of(internal_producers[i].begin(), internal_producers[i].end(),
                                   [](const auto count) { return count != 1; })) {
                return reject();
            }
        }

        std::vector<bool> reachable_from_source(definition.members.size());
        std::vector<bool> reaches_output(definition.members.size());
        std::queue<std::size_t> ready;
        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            const auto* node = graph.node(definition.members[i].node);
            if (input_boundaries.contains(definition.members[i].node) ||
                (node != nullptr && node->num_input_ports == 0)) {
                reachable_from_source[i] = true;
                ready.push(i);
            }
        }
        while (!ready.empty()) {
            const auto current = ready.front();
            ready.pop();
            for (const auto next : forward[current]) {
                if (!reachable_from_source[next]) {
                    reachable_from_source[next] = true;
                    ready.push(next);
                }
            }
        }
        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            if (output_boundaries.contains(definition.members[i].node)) {
                reaches_output[i] = true;
                ready.push(i);
            }
        }
        while (!ready.empty()) {
            const auto current = ready.front();
            ready.pop();
            for (const auto previous : reverse[current]) {
                if (!reaches_output[previous]) {
                    reaches_output[previous] = true;
                    ready.push(previous);
                }
            }
        }
        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            if (!reachable_from_source[i] || !reaches_output[i])
                return reject();
        }

        std::vector<std::optional<SampleKernelCausality>> causalities(definition.members.size());
        std::size_t known_combinational = 0;
        for (std::size_t i = 0; i < definition.members.size(); ++i) {
            const auto& member = definition.members[i];
            if (const auto* descriptor = graph.sample_kernel_type(member.type_id, member.version))
                causalities[i] = descriptor->causality;
            else if (const auto contract =
                         persisted_v1_kernel_contract(member.type_id, member.version))
                causalities[i] = contract->causality;
            if (causalities[i] == SampleKernelCausality::Combinational) {
                ++known_combinational;
            }
        }
        std::vector<std::size_t> indegree(definition.members.size());
        std::vector<std::vector<std::size_t>> instantaneous(definition.members.size());
        for (std::size_t source = 0; source < forward.size(); ++source) {
            if (!causalities[source].has_value() ||
                causalities[source] == SampleKernelCausality::OneSampleDelay) {
                continue;
            }
            for (const auto destination : forward[source]) {
                if (!causalities[destination].has_value() ||
                    causalities[destination] == SampleKernelCausality::OneSampleDelay) {
                    continue;
                }
                instantaneous[source].push_back(destination);
                ++indegree[destination];
            }
        }
        std::queue<std::size_t> acyclic;
        for (std::size_t i = 0; i < causalities.size(); ++i) {
            if (causalities[i] == SampleKernelCausality::Combinational && indegree[i] == 0) {
                acyclic.push(i);
            }
        }
        std::size_t visited = 0;
        while (!acyclic.empty()) {
            const auto current = acyclic.front();
            acyclic.pop();
            ++visited;
            for (const auto next : instantaneous[current]) {
                if (--indegree[next] == 0)
                    acyclic.push(next);
            }
        }
        if (visited != known_combinational)
            return reject();

        std::uint64_t known_member_work = 0;
        std::uint64_t known_state_bytes = 0;
        for (const auto causality : causalities) {
            if (!causality.has_value())
                continue;
            if (causality == SampleKernelCausality::OneSampleDelay) {
                known_member_work += 2;
                known_state_bytes += sizeof(float);
            } else {
                ++known_member_work;
            }
        }
        const std::uint64_t boundary_count =
            definition.input_boundaries.size() + definition.output_boundaries.size();
        const std::uint64_t known_work = known_member_work + internal_connections + boundary_count;
        const std::uint64_t logical_boundary_bytes = boundary_count * sizeof(float);
        graph_known_work += known_work;
        if (known_state_bytes > definition.limits.max_state_bytes ||
            logical_boundary_bytes > definition.limits.max_logical_boundary_bytes ||
            known_work > definition.limits.max_work_per_frame ||
            known_work > definition.limits.max_work_per_block || graph_known_work > 1'280) {
            return reject();
        }
    }
    return true;
}

} // namespace pulp::host::detail
