#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/sample_region_runtime.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

using namespace pulp::host;

namespace {

struct AbiProbe {
    const void* state = nullptr;
    const PreparedSampleKernelConfig* config = nullptr;
    const float* promoted = nullptr;
    const float* input = nullptr;
    float* output = nullptr;
    std::uint32_t promoted_count = 0;
    bool called = false;
};

AbiProbe g_probe;

void reset_probe() {
    g_probe = {};
}

bool state_construct(void* state, const SampleKernelPrepareContext&) noexcept {
    *static_cast<float*>(state) = 0.0f;
    return true;
}

void state_reset(void* state) noexcept {
    *static_cast<float*>(state) = 0.0f;
}

void state_destroy(void*) noexcept {}

void pass_boundary(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                   const float* input, float* output) noexcept {
    output[0] = input[0];
}

void capture_process(void* state, const PreparedSampleKernelConfig& config,
                     const SampleFrameContext& frame, const float* input, float* output) noexcept {
    g_probe.state = state;
    g_probe.config = &config;
    g_probe.promoted = frame.promoted_values;
    g_probe.promoted_count = frame.promoted_value_count;
    g_probe.input = input;
    g_probe.output = output;
    g_probe.called = true;
    output[0] = input[0] + (frame.promoted_value_count == 0 ? 0.0f : frame.promoted_values[0]);
}

SampleKernelDescriptor boundary_descriptor(std::string type_id) {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = std::move(type_id);
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = SampleKernelConfigKind::BoundaryIndex;
    descriptor.process = pass_boundary;
    descriptor.metadata.category = "i2-test";
    return descriptor;
}

SampleKernelDescriptor capture_descriptor() {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = "pulp.test.i2.abi-capture";
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = SampleKernelConfigKind::PromotedParameterId;
    descriptor.state_size = sizeof(float);
    descriptor.state_alignment = alignof(float);
    descriptor.construct = state_construct;
    descriptor.reset = state_reset;
    descriptor.destroy = state_destroy;
    descriptor.process = capture_process;
    descriptor.metadata.category = "i2-test";
    return descriptor;
}

PreparedSampleRegionPlan capture_plan() {
    const PreparedSampleKernelConfig input_config{PreparedSampleKernelConfigKind::BoundaryIndex, 0,
                                                  0.0f};
    const PreparedSampleKernelConfig promoted_config{
        PreparedSampleKernelConfigKind::PromotedParameterIndex, 0, 0.0f};
    const PreparedSampleKernelConfig output_config{PreparedSampleKernelConfigKind::BoundaryIndex, 0,
                                                   0.0f};

    PreparedSampleRegionPlan plan;
    plan.region_id = 9009;
    plan.promoted_parameters = {9909};
    plan.scalar_slot_count = 6;
    plan.resources.member_nodes = 3;
    plan.resources.internal_connections = 2;
    plan.resources.input_boundaries = 1;
    plan.resources.output_boundaries = 1;
    plan.resources.promoted_parameters = 1;
    plan.resources.state_bytes = sizeof(float);
    plan.resources.state_alignment = alignof(float);
    plan.resources.logical_boundary_bytes = 2 * 8 * sizeof(float);
    plan.resources.work_per_frame = 3;
    plan.resources.work_per_block = 24;
    plan.kernels = {
        {1, boundary_descriptor("pulp.core.sample-region.input"), input_config, 0, 1},
        {2, capture_descriptor(), promoted_config, 2, 3},
        {3, boundary_descriptor("pulp.core.sample-region.output"), output_config, 4, 5},
    };
    plan.combinational_order = {0, 1, 2};
    plan.transfers = {
        {1, 0, 2, 0, 1, 2},
        {2, 0, 3, 0, 3, 4},
    };
    plan.operations = {
        {PreparedSampleRegionOperationKind::Process, 0},
        {PreparedSampleRegionOperationKind::Transfer, 0},
        {PreparedSampleRegionOperationKind::Process, 1},
        {PreparedSampleRegionOperationKind::Transfer, 1},
        {PreparedSampleRegionOperationKind::Process, 2},
    };
    return plan;
}

} // namespace

TEST_CASE("Prepared sample region ABI keeps callback storage disjoint",
          "[sample-region][i2][negative][NEG-09]") {
    reset_probe();
    auto plan = capture_plan();
    const std::array plans{plan};
    auto bank = SampleRegionStateBank::create_fresh(plans, 48'000.0, 8, 1);
    REQUIRE(bank);
    auto region = PreparedSampleRegion::create(std::move(plan), bank, nullptr);
    REQUIRE(region);

    float input_sample = 0.75f;
    float output_sample = -1.0f;
    const float* input_channels[] = {&input_sample};
    float* output_channels[] = {&output_sample};
    pulp::audio::BufferView<const float> input(input_channels, 1, 1);
    pulp::audio::BufferView<float> output(output_channels, 1, 1);
    region->process(output, input, 1);

    REQUIRE(g_probe.called);
    CHECK(output_sample == 0.75f);
    CHECK(g_probe.input != nullptr);
    CHECK(g_probe.output != nullptr);
    CHECK(g_probe.state != nullptr);
    CHECK(g_probe.config != nullptr);
    CHECK(g_probe.promoted != nullptr);
    CHECK(g_probe.promoted_count == 1);
    CHECK(g_probe.output != g_probe.input);
    CHECK(static_cast<const void*>(g_probe.output) != g_probe.state);
    CHECK(static_cast<const void*>(g_probe.output) != static_cast<const void*>(g_probe.config));
    CHECK(g_probe.output != g_probe.promoted);

    const auto& process_binding = region->plan().kernels[1];
    const SampleFrameContext frame{g_probe.promoted, g_probe.promoted_count, 0};
    CHECK(sample_kernel_storage_is_disjoint(process_binding.descriptor, g_probe.state,
                                            *g_probe.config, frame, g_probe.input, g_probe.output));

    // Deliberately alias the callback's input/output storage.  The same ABI
    // probe must reject this fixture before a production callback can run.
    std::array<float, 1> aliased{1.0f};
    CHECK_FALSE(sample_kernel_storage_is_disjoint(process_binding.descriptor, g_probe.state,
                                                  *g_probe.config, frame, aliased.data(),
                                                  aliased.data()));

    // Each storage class is a separate ABI boundary.  Alias the callback's
    // output with it in turn; the production helper must reject every case.
    CHECK_FALSE(sample_kernel_storage_is_disjoint(
        process_binding.descriptor, g_probe.state, *g_probe.config, frame, g_probe.input,
        static_cast<float*>(const_cast<void*>(g_probe.state))));
    CHECK_FALSE(sample_kernel_storage_is_disjoint(
        process_binding.descriptor, g_probe.state, *g_probe.config, frame, g_probe.input,
        reinterpret_cast<float*>(const_cast<PreparedSampleKernelConfig*>(g_probe.config))));
    CHECK_FALSE(sample_kernel_storage_is_disjoint(process_binding.descriptor, g_probe.state,
                                                  *g_probe.config, frame, g_probe.input,
                                                  const_cast<float*>(g_probe.promoted)));
}
