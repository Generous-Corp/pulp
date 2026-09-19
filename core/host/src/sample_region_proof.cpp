#include <pulp/host/sample_region_proof.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string_view>

namespace pulp::host {
namespace {

constexpr std::string_view kInput = "pulp.core.sample-region.input";
constexpr std::string_view kOutput = "pulp.core.sample-region.output";
constexpr std::string_view kConstant = "pulp.core.sample-region.constant";
constexpr std::string_view kParameter = "pulp.core.sample-region.parameter";
constexpr std::string_view kAdd = "pulp.core.sample-region.add";
constexpr std::string_view kMultiply = "pulp.core.sample-region.multiply";
constexpr std::string_view kDelay = "pulp.core.unit-delay";

constexpr std::uint64_t kGraphRegions = 16;
constexpr std::uint64_t kGraphMembers = 512;
constexpr std::uint64_t kGraphWorkPerFrame = 1'280;

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return false;
    out = a + b;
    return true;
}

bool checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    out = a * b;
    return true;
}

template <class T> bool checked_add_to(std::uint64_t value, T& total) noexcept {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max() - total))
        return false;
    total = static_cast<T>(total + value);
    return true;
}

SampleRegionProof refuse(const SampleRegionCandidate& candidate, SampleRegionRefusalReason reason,
                         std::string message, NodeId node = 0, std::uint64_t actual = 0,
                         std::uint64_t limit = 0) {
    SampleRegionProof result;
    result.region_id = candidate.region_id;
    result.reason = reason;
    result.offending_node = node;
    result.actual = actual;
    result.limit = limit;
    result.message = std::move(message);
    return result;
}

SampleRegionProof refuse_connection(const SampleRegionCandidate& candidate,
                                    SampleRegionRefusalReason reason,
                                    const SampleRegionConnection& connection, std::string message) {
    auto result = refuse(candidate, reason, std::move(message), connection.destination);
    result.offending_connection = connection;
    result.has_offending_connection = true;
    return result;
}

bool descriptor_is_v1(const SampleKernelDescriptor& descriptor) noexcept {
    if (descriptor.version != 1 || descriptor.scope != SampleKernelScope::RegionOnly ||
        descriptor.abi_version != SampleKernelDescriptor::kAbiVersion ||
        descriptor.latency_samples != 0) {
        return false;
    }
    const auto id = std::string_view(descriptor.type_id);
    const auto combinational = [&](std::uint32_t inputs, std::uint32_t outputs,
                                   SampleKernelConfigKind kind) {
        return descriptor.num_input_ports == inputs && descriptor.num_output_ports == outputs &&
               descriptor.authored_config_kind == kind && descriptor.state_size == 0 &&
               descriptor.state_alignment == 1 && descriptor.construct == nullptr &&
               descriptor.reset == nullptr && descriptor.destroy == nullptr &&
               descriptor.process != nullptr && descriptor.delay_publish == nullptr &&
               descriptor.delay_commit == nullptr &&
               descriptor.causality == SampleKernelCausality::Combinational;
    };
    if (id == kInput || id == kOutput)
        return combinational(1, 1, SampleKernelConfigKind::BoundaryIndex);
    if (id == kConstant)
        return combinational(0, 1, SampleKernelConfigKind::FiniteConstant);
    if (id == kParameter)
        return combinational(0, 1, SampleKernelConfigKind::PromotedParameterId);
    if (id == kAdd || id == kMultiply)
        return combinational(2, 1, SampleKernelConfigKind::None);
    if (id == kDelay) {
        return descriptor.num_input_ports == 1 && descriptor.num_output_ports == 1 &&
               descriptor.authored_config_kind == SampleKernelConfigKind::None &&
               descriptor.causality == SampleKernelCausality::OneSampleDelay &&
               descriptor.state_size == sizeof(float) &&
               descriptor.state_alignment == alignof(float) && descriptor.construct != nullptr &&
               descriptor.reset != nullptr && descriptor.destroy != nullptr &&
               descriptor.process == nullptr && descriptor.delay_publish != nullptr &&
               descriptor.delay_commit != nullptr;
    }
    return false;
}

bool config_matches(const SampleKernelConfig& config,
                    const SampleKernelDescriptor& descriptor) noexcept {
    if (config.kind != descriptor.authored_config_kind)
        return false;
    switch (config.kind) {
    case SampleKernelConfigKind::None:
        return config.boundary_index_or_parameter_id == 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::BoundaryIndex:
        return config.constant == 0.0f;
    case SampleKernelConfigKind::FiniteConstant:
        return std::isfinite(config.constant) && config.boundary_index_or_parameter_id == 0;
    case SampleKernelConfigKind::PromotedParameterId:
        return config.boundary_index_or_parameter_id != 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

bool limits_within_v1(const SampleRegionLimits& value) noexcept {
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

SampleRegionProof prove_sample_region_impl(const SampleRegionCandidate& candidate,
                                           bool compute_block_work) {
    if (candidate.region_id == 0)
        return refuse(candidate, SampleRegionRefusalReason::UnknownRegion,
                      "sample region ID must be nonzero");
    if (candidate.max_block_size == 0)
        return refuse(candidate, SampleRegionRefusalReason::PrepareFailed,
                      "maximum block size must be nonzero");
    if (!limits_within_v1(candidate.limits))
        return refuse(candidate, SampleRegionRefusalReason::RegionLimitExceeded,
                      "authored limits exceed the closed v1 admission limits");

    std::map<NodeId, std::size_t> member_index;
    std::vector<bool> is_delay(candidate.members.size());
    std::vector<bool> is_input(candidate.members.size());
    std::vector<bool> is_output(candidate.members.size());
    std::vector<bool> is_source(candidate.members.size());
    std::vector<const SampleKernelDescriptor*> descriptors(candidate.members.size());
    std::set<std::uint32_t> input_indexes;
    std::set<std::uint32_t> output_indexes;
    std::set<state::ParamID> promoted;
    std::set<state::ParamID> referenced_parameters;
    std::uint32_t input_count = 0;
    std::uint32_t output_count = 0;

    for (const auto parameter : candidate.promoted_parameters) {
        if (parameter == 0 || !promoted.insert(parameter).second) {
            return refuse(candidate, SampleRegionRefusalReason::DuplicatePromotedParameter,
                          "promoted parameter IDs must be unique and nonzero");
        }
    }

    for (std::size_t i = 0; i < candidate.members.size(); ++i) {
        const auto& member = candidate.members[i];
        if (member.node == 0 || !member_index.emplace(member.node, i).second)
            return refuse(candidate, SampleRegionRefusalReason::UnknownMember,
                          "member IDs must be unique and nonzero", member.node);
        const auto* descriptor = candidate.registry.find(member.type_id, member.version);
        if (descriptor == nullptr || descriptor->type_id != member.type_id ||
            descriptor->version != member.version || !descriptor_is_v1(*descriptor)) {
            return refuse(candidate, SampleRegionRefusalReason::UnresolvedSampleKernel,
                          "member does not resolve to an exact registered v1 descriptor",
                          member.node);
        }
        descriptors[i] = descriptor;
        if (descriptor->latency_samples != 0) {
            return refuse(candidate, SampleRegionRefusalReason::NonZeroCompensatableLatency,
                          "sample-region kernels must report zero compensatable latency",
                          member.node, descriptor->latency_samples, 0);
        }
        if (!config_matches(member.config, *descriptor))
            return refuse(candidate, SampleRegionRefusalReason::InvalidKernelConfig,
                          "authored config does not match the exact kernel contract", member.node);

        const auto id = std::string_view(descriptor->type_id);
        is_delay[i] = id == kDelay;
        is_input[i] = id == kInput;
        is_output[i] = id == kOutput;
        is_source[i] = is_input[i] || id == kConstant || id == kParameter;
        if (is_input[i]) {
            ++input_count;
            input_indexes.insert(member.config.boundary_index_or_parameter_id);
        }
        if (is_output[i]) {
            ++output_count;
            output_indexes.insert(member.config.boundary_index_or_parameter_id);
        }
        if (id == kParameter) {
            referenced_parameters.insert(member.config.boundary_index_or_parameter_id);
            if (!promoted.contains(member.config.boundary_index_or_parameter_id)) {
                return refuse(candidate, SampleRegionRefusalReason::ParameterContractMismatch,
                              "parameter kernel is absent from the frozen promoted manifest",
                              member.node);
            }
        }
    }

    const auto contiguous = [](const std::set<std::uint32_t>& indexes) {
        std::uint32_t expected = 0;
        for (const auto index : indexes) {
            if (index != expected++)
                return false;
        }
        return true;
    };
    if (input_indexes.empty() || output_indexes.empty() || input_indexes.size() != input_count ||
        output_indexes.size() != output_count || !contiguous(input_indexes) ||
        !contiguous(output_indexes)) {
        return refuse(candidate, SampleRegionRefusalReason::InvalidBoundary,
                      "boundary indexes must be nonempty contiguous zero-based ranges");
    }
    if (referenced_parameters != promoted)
        return refuse(candidate, SampleRegionRefusalReason::ParameterContractMismatch,
                      "promoted manifest and parameter kernels must match exactly");

    std::vector<std::vector<SampleRegionConnection>> incoming(candidate.members.size());
    std::vector<std::vector<SampleRegionConnection>> outgoing(candidate.members.size());
    std::vector<std::vector<std::size_t>> forward(candidate.members.size());
    std::vector<std::vector<std::size_t>> reverse(candidate.members.size());
    std::uint64_t internal_connections = 0;

    for (const auto& connection : candidate.connections) {
        if (connection.lane != SampleRegionConnectionLane::PlainAudio)
            return refuse_connection(candidate,
                                     SampleRegionRefusalReason::UnsupportedConnectionLane,
                                     connection, "only plain-audio edges are admitted in v1");
        if (connection.legacy_feedback)
            return refuse_connection(candidate, SampleRegionRefusalReason::LegacyFeedbackInRegion,
                                     connection, "legacy feedback edges are not region edges");
        const auto source = member_index.find(connection.source);
        const auto destination = member_index.find(connection.destination);
        const bool source_inside = source != member_index.end();
        const bool destination_inside = destination != member_index.end();
        if (!source_inside && !destination_inside)
            return refuse_connection(candidate, SampleRegionRefusalReason::UnknownMember,
                                     connection, "connection does not touch the region");

        if (source_inside &&
            connection.source_port >= descriptors[source->second]->num_output_ports)
            return refuse_connection(candidate,
                                     SampleRegionRefusalReason::InvalidProducerCardinality,
                                     connection, "source port is outside the descriptor");
        if (destination_inside &&
            connection.destination_port >= descriptors[destination->second]->num_input_ports)
            return refuse_connection(candidate,
                                     SampleRegionRefusalReason::InvalidProducerCardinality,
                                     connection, "destination port is outside the descriptor");

        if (source_inside && destination_inside) {
            ++internal_connections;
            incoming[destination->second].push_back(connection);
            outgoing[source->second].push_back(connection);
            forward[source->second].push_back(destination->second);
            reverse[destination->second].push_back(source->second);
        } else if (!source_inside) {
            if (!is_input[destination->second])
                return refuse_connection(
                    candidate, SampleRegionRefusalReason::InvalidBoundaryCrossing, connection,
                    "outside edges may enter only input-boundary nodes");
            incoming[destination->second].push_back(connection);
        } else {
            if (!is_output[source->second])
                return refuse_connection(
                    candidate, SampleRegionRefusalReason::InvalidBoundaryCrossing, connection,
                    "outside edges may leave only output-boundary nodes");
            outgoing[source->second].push_back(connection);
        }
    }

    for (std::size_t i = 0; i < candidate.members.size(); ++i) {
        const auto& member = candidate.members[i];
        if (is_input[i]) {
            const auto external_in =
                std::count_if(incoming[i].begin(), incoming[i].end(),
                              [&](const auto& c) { return !member_index.contains(c.source); });
            const auto internal_in = incoming[i].size() - static_cast<std::size_t>(external_in);
            if (external_in != 1 || internal_in != 0)
                return refuse(candidate, SampleRegionRefusalReason::InvalidProducerCardinality,
                              "input boundary requires exactly one external producer", member.node,
                              incoming[i].size(), 1);
            continue;
        }
        if (is_output[i]) {
            const auto internal_in =
                std::count_if(incoming[i].begin(), incoming[i].end(),
                              [&](const auto& c) { return member_index.contains(c.source); });
            const auto internal_out =
                std::count_if(outgoing[i].begin(), outgoing[i].end(),
                              [&](const auto& c) { return member_index.contains(c.destination); });
            if (internal_in != 1 || internal_out != 0)
                return refuse(
                    candidate, SampleRegionRefusalReason::InvalidProducerCardinality,
                    "output boundary requires one internal producer and no internal consumer",
                    member.node);
            continue;
        }
        std::vector<std::uint32_t> producer_count(descriptors[i]->num_input_ports);
        for (const auto& edge : incoming[i]) {
            if (!member_index.contains(edge.source))
                return refuse_connection(candidate,
                                         SampleRegionRefusalReason::InvalidBoundaryCrossing, edge,
                                         "ordinary members cannot consume outside edges");
            ++producer_count[edge.destination_port];
        }
        if (std::any_of(producer_count.begin(), producer_count.end(),
                        [](auto count) { return count != 1; }))
            return refuse(candidate, SampleRegionRefusalReason::InvalidProducerCardinality,
                          "every ordinary kernel input requires exactly one internal producer",
                          member.node);
    }

    std::vector<bool> reachable_from_source(candidate.members.size());
    std::queue<std::size_t> queue;
    for (std::size_t i = 0; i < is_source.size(); ++i) {
        if (is_source[i]) {
            reachable_from_source[i] = true;
            queue.push(i);
        }
    }
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        for (const auto next : forward[current]) {
            if (!reachable_from_source[next]) {
                reachable_from_source[next] = true;
                queue.push(next);
            }
        }
    }
    std::vector<bool> reaches_output(candidate.members.size());
    for (std::size_t i = 0; i < is_output.size(); ++i) {
        if (is_output[i]) {
            reaches_output[i] = true;
            queue.push(i);
        }
    }
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        for (const auto previous : reverse[current]) {
            if (!reaches_output[previous]) {
                reaches_output[previous] = true;
                queue.push(previous);
            }
        }
    }
    for (std::size_t i = 0; i < candidate.members.size(); ++i) {
        if (!reachable_from_source[i] || !reaches_output[i])
            return refuse(candidate, SampleRegionRefusalReason::DisconnectedMember,
                          "every member must lie on a source-to-output path",
                          candidate.members[i].node);
    }

    std::vector<std::uint32_t> indegree(candidate.members.size());
    std::vector<std::vector<std::size_t>> instantaneous(candidate.members.size());
    for (std::size_t source = 0; source < forward.size(); ++source) {
        for (const auto destination : forward[source]) {
            if (is_delay[source] || is_delay[destination])
                continue;
            instantaneous[source].push_back(destination);
            ++indegree[destination];
        }
    }
    std::priority_queue<NodeId, std::vector<NodeId>, std::greater<>> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i) {
        if (!is_delay[i] && indegree[i] == 0)
            ready.push(candidate.members[i].node);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto node = ready.top();
        ready.pop();
        ++visited;
        const auto index = member_index.at(node);
        for (const auto next : instantaneous[index]) {
            if (--indegree[next] == 0)
                ready.push(candidate.members[next].node);
        }
    }
    const auto combinational_count =
        candidate.members.size() -
        static_cast<std::size_t>(std::count(is_delay.begin(), is_delay.end(), true));
    if (visited != combinational_count) {
        const auto offender =
            std::find_if(indegree.begin(), indegree.end(), [](auto degree) { return degree != 0; });
        return refuse(
            candidate, SampleRegionRefusalReason::InstantaneousCycle,
            "instantaneous dependency graph is cyclic",
            candidate.members[static_cast<std::size_t>(offender - indegree.begin())].node);
    }

    SampleRegionResourceStats stats;
    stats.member_nodes = static_cast<std::uint32_t>(candidate.members.size());
    stats.internal_connections = static_cast<std::uint32_t>(internal_connections);
    stats.input_boundaries = static_cast<std::uint32_t>(input_indexes.size());
    stats.output_boundaries = static_cast<std::uint32_t>(output_indexes.size());
    stats.delay_nodes =
        static_cast<std::uint32_t>(std::count(is_delay.begin(), is_delay.end(), true));
    stats.promoted_parameters = static_cast<std::uint32_t>(promoted.size());
    for (const auto* descriptor : descriptors) {
        if (!checked_add(stats.state_bytes, descriptor->state_size, stats.state_bytes))
            return refuse(candidate, SampleRegionRefusalReason::ArithmeticOverflow,
                          "kernel state byte sum overflowed");
        stats.state_alignment = std::max(stats.state_alignment, descriptor->state_alignment);
    }
    std::uint64_t boundary_count = 0;
    if (!checked_add(stats.input_boundaries, stats.output_boundaries, boundary_count) ||
        !checked_multiply(boundary_count, candidate.max_block_size, stats.logical_boundary_bytes) ||
        !checked_multiply(stats.logical_boundary_bytes, sizeof(float),
                          stats.logical_boundary_bytes))
        return refuse(candidate, SampleRegionRefusalReason::ArithmeticOverflow,
                      "logical boundary footprint overflowed");
    const std::uint64_t combinational = stats.member_nodes - stats.delay_nodes;
    if (!checked_add(combinational, 2ULL * stats.delay_nodes, stats.work_per_frame) ||
        !checked_add(stats.work_per_frame, stats.internal_connections, stats.work_per_frame) ||
        !checked_add(stats.work_per_frame, boundary_count, stats.work_per_frame))
        return refuse(candidate, SampleRegionRefusalReason::ArithmeticOverflow,
                      "work accounting overflowed");
    if (compute_block_work &&
        !checked_multiply(stats.work_per_frame, candidate.max_block_size, stats.work_per_block))
        return refuse(candidate, SampleRegionRefusalReason::ArithmeticOverflow,
                      "block work accounting overflowed");

    const struct Guard {
        std::uint64_t actual;
        std::uint64_t limit;
        SampleRegionRefusalReason reason;
        const char* message;
    } guards[] = {
        {stats.member_nodes, candidate.limits.max_member_nodes,
         SampleRegionRefusalReason::NodeLimitExceeded, "member limit exceeded"},
        {stats.internal_connections, candidate.limits.max_internal_connections,
         SampleRegionRefusalReason::ConnectionLimitExceeded, "connection limit exceeded"},
        {stats.input_boundaries, candidate.limits.max_input_boundaries,
         SampleRegionRefusalReason::InvalidBoundary, "input boundary limit exceeded"},
        {stats.output_boundaries, candidate.limits.max_output_boundaries,
         SampleRegionRefusalReason::InvalidBoundary, "output boundary limit exceeded"},
        {stats.delay_nodes, candidate.limits.max_delay_nodes,
         SampleRegionRefusalReason::DelayStateLimitExceeded, "delay limit exceeded"},
        {stats.promoted_parameters, candidate.limits.max_promoted_parameters,
         SampleRegionRefusalReason::ParameterLimitExceeded, "parameter limit exceeded"},
        {stats.state_bytes, candidate.limits.max_state_bytes,
         SampleRegionRefusalReason::StateBudgetExceeded, "state byte limit exceeded"},
        {stats.logical_boundary_bytes, candidate.limits.max_logical_boundary_bytes,
         SampleRegionRefusalReason::BoundaryBufferBudgetExceeded,
         "logical boundary byte limit exceeded"},
        {stats.work_per_frame, candidate.limits.max_work_per_frame,
         SampleRegionRefusalReason::WorkBudgetExceeded, "frame work limit exceeded"},
    };
    for (const auto& guard : guards) {
        if (guard.actual > guard.limit) {
            auto result =
                refuse(candidate, guard.reason, guard.message, 0, guard.actual, guard.limit);
            result.resources = stats;
            return result;
        }
    }
    if (compute_block_work && stats.work_per_block > candidate.limits.max_work_per_block) {
        auto result = refuse(candidate, SampleRegionRefusalReason::WorkBudgetExceeded,
                             "block work limit exceeded", 0, stats.work_per_block,
                             candidate.limits.max_work_per_block);
        result.resources = stats;
        return result;
    }

    SampleRegionProof result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    result.region_id = candidate.region_id;
    result.resources = stats;
    return result;
}

} // namespace

bool is_sample_region_v1_descriptor(const SampleKernelDescriptor& descriptor) noexcept {
    return descriptor_is_v1(descriptor);
}

bool sample_region_config_matches(const SampleKernelConfig& config,
                                  const SampleKernelDescriptor& descriptor) noexcept {
    return config_matches(config, descriptor);
}

SampleRegionProof prove_sample_region(const SampleRegionCandidate& candidate) {
    return prove_sample_region_impl(candidate, true);
}

SampleRegionGraphProof prove_sample_regions(const std::vector<SampleRegionCandidate>& candidates) {
    SampleRegionGraphProof result;
    if (candidates.size() > kGraphRegions) {
        result.reason = SampleRegionRefusalReason::RegionLimitExceeded;
        result.actual = candidates.size();
        result.limit = kGraphRegions;
        return result;
    }
    std::set<SampleRegionId> region_ids;
    std::set<NodeId> member_ids;
    std::uint64_t member_count = 0;
    for (const auto& candidate : candidates) {
        if (!region_ids.insert(candidate.region_id).second) {
            result.reason = SampleRegionRefusalReason::UnknownRegion;
            result.offending_region = candidate.region_id;
            return result;
        }
        if (!checked_add(member_count, candidate.members.size(), member_count)) {
            result.reason = SampleRegionRefusalReason::ArithmeticOverflow;
            result.offending_region = candidate.region_id;
            return result;
        }
        for (const auto& member : candidate.members) {
            if (!member_ids.insert(member.node).second) {
                result.reason = SampleRegionRefusalReason::MemberInMultipleRegions;
                result.offending_region = candidate.region_id;
                return result;
            }
        }
    }
    for (const auto& candidate : candidates) {
        if (candidate.region_id == 0) {
            result.reason = SampleRegionRefusalReason::UnknownRegion;
            result.offending_region = candidate.region_id;
            result.region_proof =
                refuse(candidate, result.reason, "sample region ID must be nonzero");
            return result;
        }
        if (candidate.max_block_size == 0) {
            result.reason = SampleRegionRefusalReason::PrepareFailed;
            result.offending_region = candidate.region_id;
            result.region_proof =
                refuse(candidate, result.reason, "maximum block size must be nonzero");
            return result;
        }
        if (!limits_within_v1(candidate.limits)) {
            result.reason = SampleRegionRefusalReason::RegionLimitExceeded;
            result.offending_region = candidate.region_id;
            result.region_proof = refuse(candidate, result.reason,
                                         "authored limits exceed the closed v1 admission limits");
            return result;
        }
        if (candidate.members.size() > candidate.limits.max_member_nodes) {
            result.reason = SampleRegionRefusalReason::NodeLimitExceeded;
            result.offending_region = candidate.region_id;
            result.region_proof =
                refuse(candidate, result.reason, "member limit exceeded", 0,
                       candidate.members.size(), candidate.limits.max_member_nodes);
            return result;
        }
    }
    if (member_count > kGraphMembers) {
        result.reason = SampleRegionRefusalReason::NodeLimitExceeded;
        result.actual = member_count;
        result.limit = kGraphMembers;
        return result;
    }

    std::vector<SampleRegionProof> proofs;
    proofs.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        auto proof = prove_sample_region_impl(candidate, false);
        if (!proof.accepted) {
            result.reason = proof.reason;
            result.offending_region = candidate.region_id;
            result.region_proof = std::move(proof);
            return result;
        }
        result.totals.state_alignment =
            std::max(result.totals.state_alignment, proof.resources.state_alignment);
        if (!checked_add_to(proof.resources.member_nodes, result.totals.member_nodes) ||
            !checked_add_to(proof.resources.internal_connections,
                            result.totals.internal_connections) ||
            !checked_add_to(proof.resources.input_boundaries, result.totals.input_boundaries) ||
            !checked_add_to(proof.resources.output_boundaries, result.totals.output_boundaries) ||
            !checked_add_to(proof.resources.delay_nodes, result.totals.delay_nodes) ||
            !checked_add_to(proof.resources.promoted_parameters,
                            result.totals.promoted_parameters) ||
            !checked_add_to(proof.resources.state_bytes, result.totals.state_bytes) ||
            !checked_add_to(proof.resources.logical_boundary_bytes,
                            result.totals.logical_boundary_bytes) ||
            !checked_add_to(proof.resources.work_per_frame, result.totals.work_per_frame)) {
            result.reason = SampleRegionRefusalReason::ArithmeticOverflow;
            result.offending_region = candidate.region_id;
            return result;
        }
        proofs.push_back(std::move(proof));
    }
    if (result.totals.work_per_frame > kGraphWorkPerFrame) {
        result.reason = SampleRegionRefusalReason::WorkBudgetExceeded;
        result.actual = result.totals.work_per_frame;
        result.limit = kGraphWorkPerFrame;
        return result;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!checked_multiply(proofs[i].resources.work_per_frame, candidates[i].max_block_size,
                              proofs[i].resources.work_per_block) ||
            !checked_add_to(proofs[i].resources.work_per_block, result.totals.work_per_block)) {
            result.reason = SampleRegionRefusalReason::ArithmeticOverflow;
            result.offending_region = candidates[i].region_id;
            return result;
        }
        if (proofs[i].resources.work_per_block > candidates[i].limits.max_work_per_block) {
            proofs[i].accepted = false;
            proofs[i].reason = SampleRegionRefusalReason::WorkBudgetExceeded;
            proofs[i].actual = proofs[i].resources.work_per_block;
            proofs[i].limit = candidates[i].limits.max_work_per_block;
            proofs[i].message = "block work limit exceeded";
            result.reason = proofs[i].reason;
            result.offending_region = candidates[i].region_id;
            result.region_proof = std::move(proofs[i]);
            return result;
        }
    }
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    return result;
}

SampleRegionParserProof prove_sample_region_parser_shape(const SampleRegionParserShape& shape) {
    if (shape.arithmetic_overflow)
        return {false, SampleRegionRefusalReason::ArithmeticOverflow, 0, 0};
    const struct Ceiling {
        std::uint64_t actual;
        std::uint64_t limit;
        SampleRegionRefusalReason reason;
    } ceilings[] = {
        {shape.regions, 16, SampleRegionRefusalReason::RegionLimitExceeded},
        {shape.members_per_region, 64, SampleRegionRefusalReason::NodeLimitExceeded},
        {shape.members_total, 512, SampleRegionRefusalReason::NodeLimitExceeded},
        {shape.connections_per_region, 128, SampleRegionRefusalReason::ConnectionLimitExceeded},
        {shape.connections_total, 2'048, SampleRegionRefusalReason::ConnectionLimitExceeded},
        {shape.input_boundaries_per_region, 8, SampleRegionRefusalReason::InvalidBoundary},
        {shape.output_boundaries_per_region, 8, SampleRegionRefusalReason::InvalidBoundary},
        {shape.input_boundaries_total, 128, SampleRegionRefusalReason::InvalidBoundary},
        {shape.output_boundaries_total, 128, SampleRegionRefusalReason::InvalidBoundary},
        {shape.delays_per_region, 32, SampleRegionRefusalReason::DelayStateLimitExceeded},
        {shape.delays_total, 512, SampleRegionRefusalReason::DelayStateLimitExceeded},
        {shape.parameters_per_region, 16, SampleRegionRefusalReason::ParameterLimitExceeded},
        {shape.parameters_total, 256, SampleRegionRefusalReason::ParameterLimitExceeded},
        {shape.kernel_state_bytes, 256, SampleRegionRefusalReason::StateBudgetExceeded},
        {shape.kernel_state_alignment, 16, SampleRegionRefusalReason::StateBudgetExceeded},
        {shape.state_bytes_per_region, 4'096, SampleRegionRefusalReason::StateBudgetExceeded},
        {shape.state_bytes_total, 65'536, SampleRegionRefusalReason::StateBudgetExceeded},
        {shape.logical_boundary_bytes_per_region, 1'048'576,
         SampleRegionRefusalReason::BoundaryBufferBudgetExceeded},
        {shape.logical_boundary_bytes_total, 16'777'216,
         SampleRegionRefusalReason::BoundaryBufferBudgetExceeded},
        {shape.work_per_frame_per_region, 256, SampleRegionRefusalReason::WorkBudgetExceeded},
        {shape.work_per_frame_total, 4'096, SampleRegionRefusalReason::WorkBudgetExceeded},
        {shape.work_per_block_per_region, 4'194'304, SampleRegionRefusalReason::WorkBudgetExceeded},
        {shape.work_per_block_total, 67'108'864, SampleRegionRefusalReason::WorkBudgetExceeded},
    };
    for (const auto& ceiling : ceilings) {
        if (ceiling.actual > ceiling.limit)
            return {false, ceiling.reason, ceiling.actual, ceiling.limit};
    }
    if (shape.declared_bytes > shape.available_bytes)
        return {false, SampleRegionRefusalReason::PrepareFailed, shape.declared_bytes,
                shape.available_bytes};
    return {true, SampleRegionRefusalReason::None, 0, 0};
}

} // namespace pulp::host
