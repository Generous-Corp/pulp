#include <catch2/catch_test_macros.hpp>

#include <pulp/host/sample_region_proof.hpp>

#include <algorithm>
#include <limits>

using namespace pulp::host;

namespace {

void scalar_copy(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                 const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0];
}
void scalar_add(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0] + inputs[1];
}
bool delay_construct(void* state, const SampleKernelPrepareContext&) noexcept {
    *static_cast<float*>(state) = 0.0f;
    return true;
}
void delay_reset(void* state) noexcept {
    *static_cast<float*>(state) = 0.0f;
}
void delay_destroy(void*) noexcept {}
void delay_publish(const void* state, const PreparedSampleKernelConfig&, float* outputs) noexcept {
    outputs[0] = *static_cast<const float*>(state);
}
void delay_commit(void* state, const PreparedSampleKernelConfig&, const float* inputs) noexcept {
    *static_cast<float*>(state) = inputs[0];
}

SampleKernelDescriptor combinational(std::string id, std::uint32_t inputs,
                                     SampleKernelConfigKind kind) {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = std::move(id);
    descriptor.num_input_ports = inputs;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = kind;
    descriptor.process = inputs == 2 ? scalar_add : scalar_copy;
    return descriptor;
}

SampleKernelDescriptor delay_descriptor() {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = "pulp.core.unit-delay";
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = SampleKernelConfigKind::None;
    descriptor.causality = SampleKernelCausality::OneSampleDelay;
    descriptor.state_size = sizeof(float);
    descriptor.state_alignment = alignof(float);
    descriptor.construct = delay_construct;
    descriptor.reset = delay_reset;
    descriptor.destroy = delay_destroy;
    descriptor.delay_publish = delay_publish;
    descriptor.delay_commit = delay_commit;
    return descriptor;
}

const SampleKernelDescriptor* resolve_kernel(const void*, std::string_view type_id,
                                             int version) noexcept {
    static const std::vector<SampleKernelDescriptor> descriptors = {
        combinational("pulp.core.sample-region.input", 1, SampleKernelConfigKind::BoundaryIndex),
        combinational("pulp.core.sample-region.output", 1, SampleKernelConfigKind::BoundaryIndex),
        combinational("pulp.core.sample-region.constant", 0,
                      SampleKernelConfigKind::FiniteConstant),
        combinational("pulp.core.sample-region.parameter", 0,
                      SampleKernelConfigKind::PromotedParameterId),
        combinational("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None),
        combinational("pulp.core.sample-region.multiply", 2, SampleKernelConfigKind::None),
        delay_descriptor(),
    };
    const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& value) {
        return value.type_id == type_id && value.version == version;
    });
    return found == descriptors.end() ? nullptr : &*found;
}

SampleRegionKernelRegistryView registry() {
    return {nullptr, resolve_kernel};
}

SampleRegionKernelNode node(NodeId id, SampleKernelDescriptor descriptor,
                            SampleKernelConfig config) {
    return {id, std::move(descriptor.type_id), descriptor.version, config};
}

SampleKernelConfig none() {
    SampleKernelConfig config;
    config.kind = SampleKernelConfigKind::None;
    return config;
}

SampleKernelConfig boundary(std::uint32_t index) {
    SampleKernelConfig config;
    config.kind = SampleKernelConfigKind::BoundaryIndex;
    config.boundary_index_or_parameter_id = index;
    return config;
}

SampleKernelConfig parameter(pulp::state::ParamID id) {
    SampleKernelConfig config;
    config.kind = SampleKernelConfigKind::PromotedParameterId;
    config.boundary_index_or_parameter_id = id;
    return config;
}

SampleRegionCandidate delayed_cycle() {
    SampleRegionCandidate candidate;
    candidate.region_id = 9;
    candidate.registry = registry();
    candidate.max_block_size = 64;
    candidate.members = {
        node(1,
             combinational("pulp.core.sample-region.input", 1,
                           SampleKernelConfigKind::BoundaryIndex),
             boundary(0)),
        node(2, combinational("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None),
             none()),
        node(3, delay_descriptor(), none()),
        node(4,
             combinational("pulp.core.sample-region.output", 1,
                           SampleKernelConfigKind::BoundaryIndex),
             boundary(0)),
    };
    candidate.connections = {
        {100, 0, 1, 0}, {1, 0, 2, 0}, {3, 0, 2, 1}, {2, 0, 3, 0}, {2, 0, 4, 0}, {4, 0, 101, 0},
    };
    return candidate;
}

SampleRegionCandidate instantaneous_cycle() {
    auto candidate = delayed_cycle();
    candidate.members[2] =
        node(3, combinational("pulp.core.sample-region.multiply", 2, SampleKernelConfigKind::None),
             none());
    candidate.connections.insert(candidate.connections.begin() + 4, {1, 0, 3, 1});
    return candidate;
}

SampleRegionCandidate delayed_cycle_with_parameter() {
    auto candidate = delayed_cycle();
    candidate.promoted_parameters = {29};
    candidate.members.push_back(node(5,
                                     combinational("pulp.core.sample-region.parameter", 0,
                                                   SampleKernelConfigKind::PromotedParameterId),
                                     parameter(29)));
    candidate.members.push_back(node(
        6, combinational("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None), none()));
    candidate.connections.erase(candidate.connections.begin() + 4);
    candidate.connections.insert(candidate.connections.begin() + 4,
                                 {{2, 0, 6, 0}, {5, 0, 6, 1}, {6, 0, 4, 0}});
    return candidate;
}

SampleRegionCandidate high_work_region(SampleRegionId region_id, NodeId base) {
    SampleRegionCandidate candidate;
    candidate.region_id = region_id;
    candidate.registry = registry();
    candidate.max_block_size = 64;
    candidate.members.reserve(64);
    candidate.connections.reserve(95);

    const auto input = base + 1;
    const auto first_add = base + 2;
    const auto first_delay = base + 32;
    const auto output = base + 64;
    candidate.members.push_back(node(
        input,
        combinational("pulp.core.sample-region.input", 1, SampleKernelConfigKind::BoundaryIndex),
        boundary(0)));
    for (std::uint32_t i = 0; i < 30; ++i) {
        candidate.members.push_back(node(
            first_add + i,
            combinational("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None), none()));
    }
    for (std::uint32_t i = 0; i < 32; ++i)
        candidate.members.push_back(node(first_delay + i, delay_descriptor(), none()));
    candidate.members.push_back(node(
        output,
        combinational("pulp.core.sample-region.output", 1, SampleKernelConfigKind::BoundaryIndex),
        boundary(0)));

    candidate.connections.push_back({base + 1'000, 0, input, 0});
    for (std::uint32_t i = 0; i < 30; ++i) {
        candidate.connections.push_back({i == 0 ? input : first_add + i - 1, 0, first_add + i, 0});
        candidate.connections.push_back({first_delay + i, 0, first_add + i, 1});
    }
    const auto last_add = first_add + 29;
    for (std::uint32_t i = 0; i < 31; ++i)
        candidate.connections.push_back({last_add, 0, first_delay + i, 0});
    candidate.connections.push_back({first_delay + 30, 0, first_delay + 31, 0});
    candidate.connections.push_back({first_delay + 31, 0, output, 0});
    candidate.connections.push_back({output, 0, base + 1'001, 0});
    return candidate;
}

} // namespace

TEST_CASE("Sample-region proof accepts a delayed cycle and reports exact resources",
          "[host][sample-region][proof]") {
    const auto proof = prove_sample_region(delayed_cycle());
    REQUIRE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::None);
    REQUIRE(proof.resources.member_nodes == 4);
    REQUIRE(proof.resources.internal_connections == 4);
    REQUIRE(proof.resources.delay_nodes == 1);
    REQUIRE(proof.resources.state_bytes == 4);
    REQUIRE(proof.resources.state_alignment == alignof(float));
    REQUIRE(proof.resources.logical_boundary_bytes == 512);
    REQUIRE(proof.resources.work_per_frame == 11);
    REQUIRE(proof.resources.work_per_block == 704);
}

TEST_CASE("Sample-region proof resolves exact registry identities and versions",
          "[host][sample-region][proof][registry][negative]") {
    auto candidate = delayed_cycle();
    candidate.members[1].version = 2;
    const auto proof = prove_sample_region(candidate);
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::UnresolvedSampleKernel);
    REQUIRE(proof.offending_node == candidate.members[1].node);
}

TEST_CASE("Sample-region proof rejects the planted instantaneous bypass cycle",
          "[host][sample-region][proof][negative]") {
    const auto proof = prove_sample_region(instantaneous_cycle());
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::InstantaneousCycle);
}

TEST_CASE("Sample-region boundary and producer controls fail closed",
          "[host][sample-region][proof][negative]") {
    auto candidate = delayed_cycle();
    candidate.connections.erase(candidate.connections.begin());
    auto proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::InvalidProducerCardinality);

    candidate = delayed_cycle();
    candidate.connections.push_back({102, 0, 2, 0});
    proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::InvalidBoundaryCrossing);

    candidate = delayed_cycle();
    candidate.connections[1].lane = SampleRegionConnectionLane::Midi;
    proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::UnsupportedConnectionLane);

    candidate = delayed_cycle();
    candidate.connections.push_back({1, 0, 2, 1});
    proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::InvalidProducerCardinality);

    candidate = delayed_cycle();
    candidate.members.push_back(node(
        7,
        combinational("pulp.core.sample-region.output", 1, SampleKernelConfigKind::BoundaryIndex),
        boundary(0)));
    proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::InvalidBoundary);
}

TEST_CASE("Sample-region authored limits preserve the fixed refusal order",
          "[host][sample-region][proof][limits]") {
    auto candidate = delayed_cycle();
    candidate.limits.max_member_nodes = 3;
    candidate.limits.max_internal_connections = 0;
    auto proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::NodeLimitExceeded);
    REQUIRE(proof.actual == 4);
    REQUIRE(proof.limit == 3);

    candidate = delayed_cycle();
    candidate.limits.max_work_per_frame = 10;
    proof = prove_sample_region(candidate);
    REQUIRE(proof.reason == SampleRegionRefusalReason::WorkBudgetExceeded);
    REQUIRE(proof.actual == 11);

    const auto require_refusal = [](SampleRegionCandidate value, SampleRegionRefusalReason reason) {
        const auto rejected = prove_sample_region(value);
        REQUIRE_FALSE(rejected.accepted);
        REQUIRE(rejected.reason == reason);
        REQUIRE(rejected.actual == rejected.limit + 1);
    };
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_internal_connections = 5;
    require_refusal(candidate, SampleRegionRefusalReason::ConnectionLimitExceeded);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_input_boundaries = 0;
    require_refusal(candidate, SampleRegionRefusalReason::InvalidBoundary);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_output_boundaries = 0;
    require_refusal(candidate, SampleRegionRefusalReason::InvalidBoundary);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_delay_nodes = 0;
    require_refusal(candidate, SampleRegionRefusalReason::DelayStateLimitExceeded);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_promoted_parameters = 0;
    require_refusal(candidate, SampleRegionRefusalReason::ParameterLimitExceeded);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_state_bytes = 3;
    require_refusal(candidate, SampleRegionRefusalReason::StateBudgetExceeded);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_logical_boundary_bytes = 511;
    require_refusal(candidate, SampleRegionRefusalReason::BoundaryBufferBudgetExceeded);
    candidate = delayed_cycle_with_parameter();
    candidate.limits.max_work_per_block = 959;
    require_refusal(candidate, SampleRegionRefusalReason::WorkBudgetExceeded);
}

TEST_CASE("Sample-region graph proof rejects overlapping ownership",
          "[host][sample-region][proof][negative]") {
    auto first = delayed_cycle();
    auto second = delayed_cycle();
    second.region_id = 10;
    const auto proof = prove_sample_regions({first, second});
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::MemberInMultipleRegions);
}

TEST_CASE("Sample-region graph member ceiling precedes region connection refusal",
          "[host][sample-region][proof][limits][negative]") {
    std::vector<SampleRegionCandidate> candidates;
    for (std::uint32_t i = 0; i < 9; ++i)
        candidates.push_back(high_work_region(i + 1, (i + 1) * 10'000));
    candidates.front().connections.front().lane = SampleRegionConnectionLane::Midi;

    const auto proof = prove_sample_regions(candidates);
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::NodeLimitExceeded);
    REQUIRE(proof.actual == 576);
    REQUIRE(proof.limit == 512);
}

TEST_CASE("Sample-region member ceiling precedes aggregate graph member ceiling",
          "[host][sample-region][proof][limits][negative]") {
    std::vector<SampleRegionCandidate> candidates;
    for (std::uint32_t i = 0; i < 9; ++i)
        candidates.push_back(high_work_region(i + 1, (i + 1) * 10'000));
    candidates.front().limits.max_member_nodes = 63;

    const auto proof = prove_sample_regions(candidates);
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::NodeLimitExceeded);
    REQUIRE(proof.offending_region == candidates.front().region_id);
    REQUIRE(proof.region_proof.actual == 64);
    REQUIRE(proof.region_proof.limit == 63);
}

TEST_CASE("Sample-region graph frame work ceiling precedes region block work refusal",
          "[host][sample-region][proof][limits][negative]") {
    std::vector<SampleRegionCandidate> candidates;
    for (std::uint32_t i = 0; i < 8; ++i)
        candidates.push_back(high_work_region(i + 1, (i + 1) * 10'000));
    candidates.front().max_block_size = 16'385;

    const auto proof = prove_sample_regions(candidates);
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::WorkBudgetExceeded);
    REQUIRE(proof.actual == 1'528);
    REQUIRE(proof.limit == 1'280);
    REQUIRE(proof.totals.member_nodes == 512);
    REQUIRE(proof.totals.state_alignment == alignof(float));
    REQUIRE(proof.totals.work_per_block == 0);
}

TEST_CASE("Sample-region graph checks region block work after graph frame admission",
          "[host][sample-region][proof][limits][negative]") {
    auto candidate = high_work_region(1, 10'000);
    candidate.max_block_size = 16'385;

    const auto proof = prove_sample_regions({candidate});
    REQUIRE_FALSE(proof.accepted);
    REQUIRE(proof.reason == SampleRegionRefusalReason::WorkBudgetExceeded);
    REQUIRE(proof.offending_region == candidate.region_id);
    REQUIRE(proof.region_proof.actual == 3'129'535);
    REQUIRE(proof.region_proof.limit == 3'129'344);
    REQUIRE(proof.totals.work_per_block == 3'129'535);
}

TEST_CASE("Sample-region parser ceilings accept exact and reject one-over and truncation",
          "[host][sample-region][parser][negative]") {
    SampleRegionParserShape exact;
    exact.regions = 16;
    exact.members_per_region = 64;
    exact.members_total = 512;
    exact.connections_per_region = 128;
    exact.connections_total = 2'048;
    exact.input_boundaries_per_region = 8;
    exact.output_boundaries_per_region = 8;
    exact.input_boundaries_total = 128;
    exact.output_boundaries_total = 128;
    exact.delays_per_region = 32;
    exact.delays_total = 512;
    exact.parameters_per_region = 16;
    exact.parameters_total = 256;
    exact.kernel_state_bytes = 256;
    exact.kernel_state_alignment = 16;
    exact.state_bytes_per_region = 4'096;
    exact.state_bytes_total = 65'536;
    exact.logical_boundary_bytes_per_region = 1'048'576;
    exact.logical_boundary_bytes_total = 16'777'216;
    exact.work_per_frame_per_region = 256;
    exact.work_per_frame_total = 4'096;
    exact.work_per_block_per_region = 4'194'304;
    exact.work_per_block_total = 67'108'864;
    exact.declared_bytes = 4'096;
    exact.available_bytes = 4'096;
    REQUIRE(prove_sample_region_parser_shape(exact).accepted);

    const struct CeilingCase {
        std::uint64_t SampleRegionParserShape::* field;
        std::uint64_t limit;
        SampleRegionRefusalReason reason;
    } cases[] = {
        {&SampleRegionParserShape::regions, 16, SampleRegionRefusalReason::RegionLimitExceeded},
        {&SampleRegionParserShape::members_per_region, 64,
         SampleRegionRefusalReason::NodeLimitExceeded},
        {&SampleRegionParserShape::members_total, 512,
         SampleRegionRefusalReason::NodeLimitExceeded},
        {&SampleRegionParserShape::connections_per_region, 128,
         SampleRegionRefusalReason::ConnectionLimitExceeded},
        {&SampleRegionParserShape::connections_total, 2'048,
         SampleRegionRefusalReason::ConnectionLimitExceeded},
        {&SampleRegionParserShape::input_boundaries_per_region, 8,
         SampleRegionRefusalReason::InvalidBoundary},
        {&SampleRegionParserShape::output_boundaries_per_region, 8,
         SampleRegionRefusalReason::InvalidBoundary},
        {&SampleRegionParserShape::input_boundaries_total, 128,
         SampleRegionRefusalReason::InvalidBoundary},
        {&SampleRegionParserShape::output_boundaries_total, 128,
         SampleRegionRefusalReason::InvalidBoundary},
        {&SampleRegionParserShape::delays_per_region, 32,
         SampleRegionRefusalReason::DelayStateLimitExceeded},
        {&SampleRegionParserShape::delays_total, 512,
         SampleRegionRefusalReason::DelayStateLimitExceeded},
        {&SampleRegionParserShape::parameters_per_region, 16,
         SampleRegionRefusalReason::ParameterLimitExceeded},
        {&SampleRegionParserShape::parameters_total, 256,
         SampleRegionRefusalReason::ParameterLimitExceeded},
        {&SampleRegionParserShape::kernel_state_bytes, 256,
         SampleRegionRefusalReason::StateBudgetExceeded},
        {&SampleRegionParserShape::kernel_state_alignment, 16,
         SampleRegionRefusalReason::StateBudgetExceeded},
        {&SampleRegionParserShape::state_bytes_per_region, 4'096,
         SampleRegionRefusalReason::StateBudgetExceeded},
        {&SampleRegionParserShape::state_bytes_total, 65'536,
         SampleRegionRefusalReason::StateBudgetExceeded},
        {&SampleRegionParserShape::logical_boundary_bytes_per_region, 1'048'576,
         SampleRegionRefusalReason::BoundaryBufferBudgetExceeded},
        {&SampleRegionParserShape::logical_boundary_bytes_total, 16'777'216,
         SampleRegionRefusalReason::BoundaryBufferBudgetExceeded},
        {&SampleRegionParserShape::work_per_frame_per_region, 256,
         SampleRegionRefusalReason::WorkBudgetExceeded},
        {&SampleRegionParserShape::work_per_frame_total, 4'096,
         SampleRegionRefusalReason::WorkBudgetExceeded},
        {&SampleRegionParserShape::work_per_block_per_region, 4'194'304,
         SampleRegionRefusalReason::WorkBudgetExceeded},
        {&SampleRegionParserShape::work_per_block_total, 67'108'864,
         SampleRegionRefusalReason::WorkBudgetExceeded},
    };
    for (const auto& value : cases) {
        auto one_over = exact;
        one_over.*value.field = value.limit + 1;
        const auto rejected = prove_sample_region_parser_shape(one_over);
        REQUIRE_FALSE(rejected.accepted);
        REQUIRE(rejected.reason == value.reason);
        REQUIRE(rejected.actual == value.limit + 1);
        REQUIRE(rejected.limit == value.limit);
    }

    auto truncated = exact;
    truncated.available_bytes--;
    auto proof = prove_sample_region_parser_shape(truncated);
    REQUIRE(proof.reason == SampleRegionRefusalReason::PrepareFailed);

    auto overflow = exact;
    overflow.arithmetic_overflow = true;
    proof = prove_sample_region_parser_shape(overflow);
    REQUIRE(proof.reason == SampleRegionRefusalReason::ArithmeticOverflow);
}
