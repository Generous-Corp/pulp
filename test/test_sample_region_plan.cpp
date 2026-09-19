#include <catch2/catch_test_macros.hpp>

#include <pulp/host/sample_region_plan.hpp>

#include <algorithm>

using namespace pulp::host;

namespace {

void scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
            const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs == nullptr ? 1.0f : inputs[0];
}
bool construct_delay(void* state, const SampleKernelPrepareContext&) noexcept {
    *static_cast<float*>(state) = 0.0f;
    return true;
}
void reset_delay(void* state) noexcept {
    *static_cast<float*>(state) = 0.0f;
}
void destroy_delay(void*) noexcept {}
void publish_delay(const void* state, const PreparedSampleKernelConfig&, float* output) noexcept {
    output[0] = *static_cast<const float*>(state);
}
void commit_delay(void* state, const PreparedSampleKernelConfig&, const float* input) noexcept {
    *static_cast<float*>(state) = input[0];
}

SampleKernelDescriptor
descriptor(std::string id, std::uint32_t inputs, SampleKernelConfigKind kind,
           SampleKernelCausality causality = SampleKernelCausality::Combinational) {
    SampleKernelDescriptor value;
    value.type_id = std::move(id);
    value.num_input_ports = inputs;
    value.num_output_ports = 1;
    value.authored_config_kind = kind;
    value.causality = causality;
    if (causality == SampleKernelCausality::Combinational) {
        value.process = scalar;
    } else {
        value.state_size = sizeof(float);
        value.state_alignment = alignof(float);
        value.construct = construct_delay;
        value.reset = reset_delay;
        value.destroy = destroy_delay;
        value.delay_publish = publish_delay;
        value.delay_commit = commit_delay;
    }
    return value;
}

const SampleKernelDescriptor* resolve_kernel(const void*, std::string_view type_id,
                                             int version) noexcept {
    static const std::vector<SampleKernelDescriptor> descriptors = {
        descriptor("pulp.core.sample-region.input", 1, SampleKernelConfigKind::BoundaryIndex),
        descriptor("pulp.core.sample-region.output", 1, SampleKernelConfigKind::BoundaryIndex),
        descriptor("pulp.core.sample-region.parameter", 0,
                   SampleKernelConfigKind::PromotedParameterId),
        descriptor("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None),
        descriptor("pulp.core.unit-delay", 1, SampleKernelConfigKind::None,
                   SampleKernelCausality::OneSampleDelay),
    };
    const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& value) {
        return value.type_id == type_id && value.version == version;
    });
    return found == descriptors.end() ? nullptr : &*found;
}

struct SingleReadRegistry {
    mutable std::uint32_t reads = 0;
};

const SampleKernelDescriptor*
resolve_once_per_identity(const void* context, std::string_view type_id, int version) noexcept {
    const auto& registry = *static_cast<const SingleReadRegistry*>(context);
    if (++registry.reads > 5)
        return nullptr;
    return resolve_kernel(nullptr, type_id, version);
}

SampleKernelConfig config(SampleKernelConfigKind kind, std::uint32_t value = 0) {
    SampleKernelConfig result;
    result.kind = kind;
    result.boundary_index_or_parameter_id = value;
    return result;
}

SampleRegionCandidate candidate() {
    SampleRegionCandidate value;
    value.region_id = 77;
    value.registry = {nullptr, resolve_kernel};
    value.max_block_size = 32;
    value.promoted_parameters = {29};
    value.members = {
        {40, "pulp.core.sample-region.output", 1, config(SampleKernelConfigKind::BoundaryIndex)},
        {20, "pulp.core.sample-region.add", 1, config(SampleKernelConfigKind::None)},
        {25, "pulp.core.sample-region.add", 1, config(SampleKernelConfigKind::None)},
        {10, "pulp.core.sample-region.input", 1, config(SampleKernelConfigKind::BoundaryIndex)},
        {30, "pulp.core.unit-delay", 1, config(SampleKernelConfigKind::None)},
        {15, "pulp.core.sample-region.parameter", 1,
         config(SampleKernelConfigKind::PromotedParameterId, 29)},
    };
    value.connections = {
        {40, 0, 101, 0}, {25, 0, 40, 0}, {25, 0, 30, 0}, {30, 0, 25, 1},
        {20, 0, 25, 0},  {15, 0, 20, 1}, {10, 0, 20, 0}, {100, 0, 10, 0},
    };
    return value;
}

} // namespace

TEST_CASE("Sample-region planner freezes deterministic publish evaluate commit order",
          "[host][sample-region][plan]") {
    const auto result = build_sample_region_plan(candidate());
    REQUIRE(result.proof.accepted);
    REQUIRE(result.plan.has_value());
    const auto& plan = *result.plan;

    REQUIRE(plan.kernels.size() == 6);
    REQUIRE(plan.kernels[0].node == 10);
    REQUIRE(plan.kernels[1].node == 15);
    REQUIRE(plan.kernels[2].node == 20);
    REQUIRE(plan.kernels[3].node == 25);
    REQUIRE(plan.kernels[4].node == 30);
    REQUIRE(plan.kernels[5].node == 40);
    const std::uint32_t expected_input_offsets[] = {0, 2, 3, 6, 9, 11};
    const std::uint32_t expected_output_offsets[] = {1, 2, 5, 8, 10, 12};
    for (std::size_t i = 0; i < plan.kernels.size(); ++i) {
        REQUIRE(plan.kernels[i].input_slot_offset == expected_input_offsets[i]);
        REQUIRE(plan.kernels[i].output_slot_offset == expected_output_offsets[i]);
        REQUIRE(plan.kernels[i].descriptor.process ==
                resolve_kernel(nullptr, plan.kernels[i].descriptor.type_id,
                               plan.kernels[i].descriptor.version)
                    ->process);
    }
    REQUIRE(plan.scalar_slot_count == 13);
    REQUIRE(plan.delay_publish_order == std::vector<std::size_t>{4});
    REQUIRE(plan.delay_commit_order == std::vector<std::size_t>{4});
    REQUIRE(plan.combinational_order == std::vector<std::size_t>{0, 1, 2, 3, 5});
    REQUIRE(plan.kernels[1].config.kind == PreparedSampleKernelConfigKind::PromotedParameterIndex);
    REQUIRE(plan.kernels[1].config.boundary_or_parameter_index == 0);
    REQUIRE(plan.transfers == std::vector<PreparedSampleRegionTransfer>{
                                  {10, 0, 20, 0, 1, 3},
                                  {15, 0, 20, 1, 2, 4},
                                  {20, 0, 25, 0, 5, 6},
                                  {30, 0, 25, 1, 10, 7},
                                  {25, 0, 30, 0, 8, 9},
                                  {25, 0, 40, 0, 8, 11},
                              });
    REQUIRE(plan.operations == std::vector<PreparedSampleRegionOperation>{
                                   {PreparedSampleRegionOperationKind::DelayPublish, 4},
                                   {PreparedSampleRegionOperationKind::Transfer, 3},
                                   {PreparedSampleRegionOperationKind::Process, 0},
                                   {PreparedSampleRegionOperationKind::Transfer, 0},
                                   {PreparedSampleRegionOperationKind::Process, 1},
                                   {PreparedSampleRegionOperationKind::Transfer, 1},
                                   {PreparedSampleRegionOperationKind::Process, 2},
                                   {PreparedSampleRegionOperationKind::Transfer, 2},
                                   {PreparedSampleRegionOperationKind::Process, 3},
                                   {PreparedSampleRegionOperationKind::Transfer, 4},
                                   {PreparedSampleRegionOperationKind::Transfer, 5},
                                   {PreparedSampleRegionOperationKind::Process, 5},
                                   {PreparedSampleRegionOperationKind::DelayCommit, 4},
                               });
}

TEST_CASE("Sample-region planner canonicalizes authored iteration order",
          "[host][sample-region][plan]") {
    auto first = candidate();
    auto second = first;
    std::reverse(second.members.begin(), second.members.end());
    std::reverse(second.connections.begin(), second.connections.end());

    const auto lhs = build_sample_region_plan(first);
    const auto rhs = build_sample_region_plan(second);
    REQUIRE(lhs.proof.accepted);
    REQUIRE(rhs.proof.accepted);
    REQUIRE(lhs.plan->scalar_slot_count == rhs.plan->scalar_slot_count);
    REQUIRE(lhs.plan->combinational_order == rhs.plan->combinational_order);
    REQUIRE(lhs.plan->delay_publish_order == rhs.plan->delay_publish_order);
    REQUIRE(lhs.plan->delay_commit_order == rhs.plan->delay_commit_order);
    REQUIRE(lhs.plan->transfers == rhs.plan->transfers);
    REQUIRE(lhs.plan->operations == rhs.plan->operations);
}

TEST_CASE("Sample-region planner snapshots each exact registry descriptor once",
          "[host][sample-region][plan][registry]") {
    SingleReadRegistry registry;
    auto value = candidate();
    value.registry = {&registry, resolve_once_per_identity};

    const auto result = build_sample_region_plan(value);
    REQUIRE(result.proof.accepted);
    REQUIRE(result.plan.has_value());
    REQUIRE(registry.reads == 5);
}

TEST_CASE("Sample-region planner returns the value-owned proof on refusal",
          "[host][sample-region][plan][negative]") {
    auto invalid = candidate();
    invalid.connections.erase(invalid.connections.begin() + 1);
    const auto result = build_sample_region_plan(invalid);
    REQUIRE_FALSE(result.proof.accepted);
    REQUIRE_FALSE(result.plan.has_value());
    REQUIRE(result.proof.offending_node != 0);
}
