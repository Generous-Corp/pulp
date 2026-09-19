#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

using namespace pulp::host;

namespace {

void copy_scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                 const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0];
}

void block_copy_a(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                  int) {}
void block_copy_b(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                  int) {}

SampleKernelDescriptor descriptor(std::string id = "pulp.test.scalar") {
    SampleKernelDescriptor result;
    result.type_id = std::move(id);
    result.num_input_ports = 1;
    result.num_output_ports = 1;
    result.authored_config_kind = SampleKernelConfigKind::None;
    result.process = copy_scalar;
    result.metadata.category = "test";
    return result;
}

CustomNodeType block_type(std::string id = "pulp.test.scalar") {
    CustomNodeType result;
    result.type_id = std::move(id);
    result.num_input_ports = 1;
    result.num_output_ports = 1;
    result.default_name = "Scalar Test";
    return result;
}

} // namespace

static_assert(std::is_same_v<SampleKernelConstructFn,
                             bool (*)(void*, const SampleKernelPrepareContext&) noexcept>);
static_assert(std::is_same_v<SampleKernelProcessFn,
                             void (*)(void*, const PreparedSampleKernelConfig&,
                                      const SampleFrameContext&, const float*, float*) noexcept>);
static_assert(std::is_same_v<SampleKernelResetFn, void (*)(void*) noexcept>);
static_assert(std::is_same_v<SampleKernelDestroyFn, void (*)(void*) noexcept>);
static_assert(
    std::is_same_v<SampleDelayPublishFn,
                   void (*)(const void*, const PreparedSampleKernelConfig&, float*) noexcept>);
static_assert(std::is_same_v<SampleDelayCommitFn, void (*)(void*, const PreparedSampleKernelConfig&,
                                                           const float*) noexcept>);
static_assert(sizeof(float) == 4);
static_assert(alignof(float) == 4);
static_assert(static_cast<std::uint8_t>(SampleKernelConfigKind::Invalid) == 0);
static_assert(static_cast<std::uint8_t>(SampleKernelConfigKind::PromotedParameterId) == 4);
static_assert(static_cast<std::uint8_t>(PreparedSampleKernelConfigKind::Invalid) == 0);
static_assert(static_cast<std::uint8_t>(PreparedSampleKernelConfigKind::PromotedParameterIndex) ==
              4);

TEST_CASE("Sample kernel descriptor validates the fixed lifecycle matrix",
          "[host][sample-kernel][abi]") {
    auto value = descriptor();
    REQUIRE(value.is_valid_registration());

    value.abi_version = 2;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.scope = static_cast<SampleKernelScope>(1);
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.authored_config_kind = static_cast<SampleKernelConfigKind>(255);
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.causality = static_cast<SampleKernelCausality>(255);
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.type_id.clear();
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.version = 0;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.state_alignment = 0;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.state_alignment = 3;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.latency_samples = 1;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.metadata.category.clear();
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.state_size = sizeof(float);
    value.state_alignment = alignof(float);
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.delay_publish = [](const void*, const auto&, float*) noexcept {};
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.metadata.has_value_range = true;
    value.metadata.minimum_value = 2.0f;
    value.metadata.maximum_value = 1.0f;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.metadata.has_value_range = true;
    value.metadata.minimum_value = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.state_size = sizeof(float);
    value.state_alignment = alignof(float);
    value.construct = [](void*, const auto&) noexcept { return true; };
    value.reset = [](void*) noexcept {};
    value.destroy = [](void*) noexcept {};
    value.process = nullptr;
    value.delay_publish = [](const void*, const auto&, float*) noexcept {};
    value.delay_commit = [](void*, const auto&, const float*) noexcept {};
    REQUIRE_FALSE(value.is_valid_registration());

    value = descriptor();
    value.state_size = sizeof(float);
    value.state_alignment = alignof(float);
    value.construct = [](void*, const auto&) noexcept { return true; };
    value.reset = [](void*) noexcept {};
    value.destroy = [](void*) noexcept {};
    REQUIRE(value.is_valid_registration());
    value.destroy = nullptr;
    REQUIRE_FALSE(value.is_valid_registration());
    value = descriptor();
    value.causality = SampleKernelCausality::OneSampleDelay;
    value.process = nullptr;
    value.delay_publish = [](const void*, const auto&, float*) noexcept {};
    value.delay_commit = [](void*, const auto&, const float*) noexcept {};
    REQUIRE_FALSE(value.is_valid_registration());
}

TEST_CASE("Sample kernel registration is exact paired and additive",
          "[host][sample-kernel][registry]") {
    SignalGraph graph;
    auto custom = block_type();
    auto scalar = descriptor();
    REQUIRE(graph.register_custom_node_type(custom, scalar));
    REQUIRE(graph.custom_node_type_count() == 1);
    REQUIRE(graph.sample_kernel_type("pulp.test.scalar", 1) != nullptr);
    REQUIRE(graph.sample_kernel_type("pulp.test.scalar", 2) == nullptr);
    REQUIRE(graph.register_custom_node_type(custom, scalar));

    auto mismatch = scalar;
    mismatch.num_output_ports = 2;
    REQUIRE_FALSE(graph.register_custom_node_type(custom, mismatch));
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1)->num_output_ports == 1);

    // The historical overload still replaces the block type. It withdraws the
    // scalar companion rather than leaving an ABI paired with another block.
    custom.default_name = "Legacy Replacement";
    REQUIRE(graph.register_custom_node_type(custom));
    CHECK(graph.custom_node_type("pulp.test.scalar", 1)->default_name == "Legacy Replacement");
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) == nullptr);
}

TEST_CASE("Sample kernel lookup is exact by type and version", "[host][sample-kernel][registry]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(block_type(), descriptor()));
    auto custom_v2 = block_type();
    auto scalar_v2 = descriptor();
    custom_v2.version = 2;
    scalar_v2.version = 2;
    REQUIRE(graph.register_custom_node_type(custom_v2, scalar_v2));
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1)->version == 1);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 2)->version == 2);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 3) == nullptr);
}

TEST_CASE("Paired registration rejects opaque or different block callback identity",
          "[host][sample-kernel][registry]") {
    SignalGraph graph;
    auto custom = block_type();
    custom.process = &block_copy_a;
    REQUIRE(graph.register_custom_node_type(custom, descriptor()));
    REQUIRE(graph.register_custom_node_type(custom, descriptor()));

    auto conflict = custom;
    conflict.process = &block_copy_b;
    CHECK_FALSE(graph.register_custom_node_type(conflict, descriptor()));

    SignalGraph captured;
    auto first = block_type();
    first.process = [gain = 1.0f](auto&, const auto&, int) { (void)gain; };
    REQUIRE(captured.register_custom_node_type(first, descriptor()));
    CHECK_FALSE(captured.register_custom_node_type(first, descriptor()));
}

TEST_CASE("Prepared topology edit publishes paired scalar registration atomically",
          "[host][sample-kernel][prepared-edit]") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->register_custom_node_type(block_type(), descriptor()));
    REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    REQUIRE(graph.custom_node_type("pulp.test.scalar", 1) != nullptr);
    REQUIRE(graph.sample_kernel_type("pulp.test.scalar", 1) != nullptr);
}

TEST_CASE("Prepared edit upgrades compatible block types without replacement conflict",
          "[host][sample-kernel][prepared-edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(block_type()));
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->register_custom_node_type(block_type(), descriptor()));
    REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) != nullptr);

    auto repeat = graph.begin_prepared_topology_edit();
    REQUIRE(repeat->register_custom_node_type(block_type(), descriptor()));
    REQUIRE(repeat->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(repeat->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    SignalGraph with_node;
    REQUIRE(with_node.register_custom_node_type(block_type()));
    REQUIRE(with_node.add_custom_node("pulp.test.scalar") != 0);
    REQUIRE(with_node.register_custom_node_type(block_type(), descriptor()));
    CHECK(with_node.sample_kernel_type("pulp.test.scalar", 1) != nullptr);
}

TEST_CASE("Prepared scalar conflicts fail closed without changing the owner",
          "[host][sample-kernel][prepared-edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(block_type(), descriptor()));
    auto conflict = block_type();
    conflict.process = &block_copy_a;
    auto edit = graph.begin_prepared_topology_edit();
    CHECK_FALSE(edit->register_custom_node_type(conflict, descriptor()));
    CHECK(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
    CHECK(graph.custom_node_type("pulp.test.scalar", 1)->process == nullptr);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) != nullptr);

    SignalGraph builtin_conflict;
    auto occupied = block_type("pulp.core.sample-region.add");
    occupied.num_input_ports = 2;
    occupied.default_name = "Conflicting Add";
    REQUIRE(builtin_conflict.register_custom_node_type(std::move(occupied)));
    auto builtin_edit = builtin_conflict.begin_prepared_topology_edit();
    CHECK_FALSE(register_builtin_sample_region_types(*builtin_edit));
    CHECK(builtin_conflict.custom_node_type_count() == 1);
    CHECK(builtin_conflict.sample_kernel_type("pulp.core.sample-region.input", 1) == nullptr);
}

TEST_CASE("Prepared built-in registrar ignores unrelated scalar baseline",
          "[host][sample-kernel][prepared-edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(block_type(), descriptor()));
    CustomNodeType existing_add;
    existing_add.type_id = "pulp.core.sample-region.add";
    existing_add.num_input_ports = 2;
    existing_add.num_output_ports = 1;
    existing_add.default_name = "Add";
    REQUIRE(graph.register_custom_node_type(existing_add));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(register_builtin_sample_region_types(*edit));
    REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.custom_node_type_count() == 8);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) != nullptr);
    CHECK(graph.sample_kernel_type("pulp.core.sample-region.add", 1) != nullptr);
}

TEST_CASE("Built-in sample-region catalog is exact idempotent and conflict safe",
          "[host][sample-kernel][registry]") {
    SignalGraph graph;
    REQUIRE(register_builtin_sample_region_types(graph));
    REQUIRE(graph.custom_node_type_count() == 7);
    REQUIRE(register_builtin_sample_region_types(graph));
    REQUIRE(graph.custom_node_type_count() == 7);

    const auto* add = graph.sample_kernel_type("pulp.core.sample-region.add", 1);
    REQUIRE(add != nullptr);
    CHECK(add->num_input_ports == 2);
    CHECK(add->causality == SampleKernelCausality::Combinational);
    float add_inputs[] = {2.0f, 3.5f};
    float add_output = 0.0f;
    add->process(nullptr, {}, {}, add_inputs, &add_output);
    CHECK(add_output == 5.5f);

    const auto* input_kernel = graph.sample_kernel_type("pulp.core.sample-region.input", 1);
    const auto* output_kernel = graph.sample_kernel_type("pulp.core.sample-region.output", 1);
    REQUIRE(input_kernel != nullptr);
    REQUIRE(output_kernel != nullptr);
    float passthrough_input = 0.375f;
    float passthrough_output = 0.0f;
    input_kernel->process(nullptr, {}, {}, &passthrough_input, &passthrough_output);
    CHECK(passthrough_output == passthrough_input);
    passthrough_output = 0.0f;
    output_kernel->process(nullptr, {}, {}, &passthrough_input, &passthrough_output);
    CHECK(passthrough_output == passthrough_input);

    const auto* constant = graph.sample_kernel_type("pulp.core.sample-region.constant", 1);
    REQUIRE(constant != nullptr);
    PreparedSampleKernelConfig constant_config{PreparedSampleKernelConfigKind::FiniteConstant, 0,
                                               -2.25f};
    float constant_output = 0.0f;
    constant->process(nullptr, constant_config, {}, nullptr, &constant_output);
    CHECK(constant_output == -2.25f);

    const auto* parameter = graph.sample_kernel_type("pulp.core.sample-region.parameter", 1);
    REQUIRE(parameter != nullptr);
    const float promoted[] = {0.125f, 0.625f};
    PreparedSampleKernelConfig parameter_config{
        PreparedSampleKernelConfigKind::PromotedParameterIndex, 1, 0.0f};
    float parameter_output = 0.0f;
    parameter->process(nullptr, parameter_config, {promoted, 2, 0}, nullptr, &parameter_output);
    CHECK(parameter_output == promoted[1]);
    parameter_config.boundary_or_parameter_index = 2;
    parameter->process(nullptr, parameter_config, {promoted, 2, 0}, nullptr, &parameter_output);
    CHECK(parameter_output == 0.0f);

    const auto* multiply = graph.sample_kernel_type("pulp.core.sample-region.multiply", 1);
    REQUIRE(multiply != nullptr);
    float multiply_inputs[] = {-2.0f, 0.25f};
    float multiply_output = 0.0f;
    multiply->process(nullptr, {}, {}, multiply_inputs, &multiply_output);
    CHECK(multiply_output == -0.5f);

    const auto* delay = graph.sample_kernel_type("pulp.core.unit-delay", 1);
    REQUIRE(delay != nullptr);
    CHECK(delay->state_size == sizeof(float));
    CHECK(delay->state_alignment == alignof(float));
    alignas(float) std::byte state[sizeof(float)];
    SampleKernelPrepareContext prepare{
        48000.0, 64, {PreparedSampleKernelConfigKind::None, 0, 0.0f}};
    REQUIRE(delay->construct(state, prepare));
    CHECK_FALSE(delay->construct(nullptr, prepare));
    auto invalid_prepare = prepare;
    invalid_prepare.sample_rate = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(delay->construct(state, invalid_prepare));
    invalid_prepare = prepare;
    invalid_prepare.max_block_size = 0;
    CHECK_FALSE(delay->construct(state, invalid_prepare));
    invalid_prepare = prepare;
    invalid_prepare.config.kind = PreparedSampleKernelConfigKind::Invalid;
    CHECK_FALSE(delay->construct(state, invalid_prepare));
    float delayed = -1.0f;
    delay->delay_publish(state, prepare.config, &delayed);
    CHECK(delayed == 0.0f);
    const float next = 0.75f;
    delay->delay_commit(state, prepare.config, &next);
    delay->delay_publish(state, prepare.config, &delayed);
    CHECK(delayed == next);
    delay->reset(state);
    delay->delay_publish(state, prepare.config, &delayed);
    CHECK(delayed == 0.0f);
    delay->destroy(state);

    SignalGraph conflict;
    auto occupied = block_type("pulp.core.sample-region.add");
    occupied.num_input_ports = 2;
    occupied.default_name = "Conflicting Add";
    REQUIRE(conflict.register_custom_node_type(std::move(occupied)));
    REQUIRE_FALSE(register_builtin_sample_region_types(conflict));
    CHECK(conflict.custom_node_type_count() == 1);
    CHECK(conflict.sample_kernel_type("pulp.core.sample-region.input", 1) == nullptr);

    // A rejected conflict must not cancel an unrelated open swap transaction.
    conflict.begin_swap_edit();
    REQUIRE_FALSE(register_builtin_sample_region_types(conflict));
    CHECK(conflict.prepare_swap(48000.0, 64) != SignalGraph::SwapResult::NotInSwapEdit);
}

TEST_CASE("Built-in sample-region catalog has the closed seven exact rows",
          "[host][sample-kernel][registry]") {
    struct Expected {
        const char* id;
        std::uint32_t inputs;
        std::uint32_t outputs;
        SampleKernelConfigKind config;
        SampleKernelCausality causality;
        std::uint32_t state_size;
        std::uint32_t state_alignment;
    };
    const std::array<Expected, 7> expected{{
        {"pulp.core.sample-region.input", 1, 1, SampleKernelConfigKind::BoundaryIndex,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.sample-region.output", 1, 1, SampleKernelConfigKind::BoundaryIndex,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.sample-region.constant", 0, 1, SampleKernelConfigKind::FiniteConstant,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.sample-region.parameter", 0, 1, SampleKernelConfigKind::PromotedParameterId,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.sample-region.add", 2, 1, SampleKernelConfigKind::None,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.sample-region.multiply", 2, 1, SampleKernelConfigKind::None,
         SampleKernelCausality::Combinational, 0, 1},
        {"pulp.core.unit-delay", 1, 1, SampleKernelConfigKind::None,
         SampleKernelCausality::OneSampleDelay, 4, 4},
    }};
    SignalGraph graph;
    REQUIRE(register_builtin_sample_region_types(graph));
    for (const auto& row : expected) {
        const auto* scalar = graph.sample_kernel_type(row.id, 1);
        REQUIRE(scalar != nullptr);
        CHECK(scalar->num_input_ports == row.inputs);
        CHECK(scalar->num_output_ports == row.outputs);
        CHECK(scalar->authored_config_kind == row.config);
        CHECK(scalar->causality == row.causality);
        CHECK(scalar->scope == SampleKernelScope::RegionOnly);
        CHECK(scalar->state_size == row.state_size);
        CHECK(scalar->state_alignment == row.state_alignment);
    }
}

TEST_CASE("Prepared unregister and prune remove scalar companions",
          "[host][sample-kernel][prepared-edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(block_type(), descriptor()));
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->unregister_custom_node_type("pulp.test.scalar", 1));
    REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) == nullptr);

    REQUIRE(graph.register_custom_node_type(block_type(), descriptor()));
    auto prune = graph.begin_prepared_topology_edit();
    REQUIRE(prune->prune_unused_custom_node_types() == 1);
    REQUIRE(prune->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(prune->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.sample_kernel_type("pulp.test.scalar", 1) == nullptr);

    auto missing = graph.begin_prepared_topology_edit();
    CHECK_FALSE(missing->unregister_custom_node_type("pulp.test.missing", 1));

    SignalGraph in_use;
    REQUIRE(in_use.register_custom_node_type(block_type(), descriptor()));
    REQUIRE(in_use.add_custom_node("pulp.test.scalar") != 0);
    auto refused = in_use.begin_prepared_topology_edit();
    CHECK_FALSE(refused->unregister_custom_node_type("pulp.test.scalar", 1));
    CHECK(in_use.sample_kernel_type("pulp.test.scalar", 1) != nullptr);
}

TEST_CASE("Sample kernel ABI rejects output aliasing every immutable input",
          "[host][sample-kernel][alias]") {
    auto scalar = descriptor();
    PreparedSampleKernelConfig config{PreparedSampleKernelConfigKind::None, 0, 0.0f};
    float promoted = 0.25f;
    SampleFrameContext frame{&promoted, 1, 0};
    float input = 1.0f;
    float output = 0.0f;
    REQUIRE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, frame, &input, &output));
    CHECK_FALSE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, frame, &input, &input));
    CHECK_FALSE(
        sample_kernel_storage_is_disjoint(scalar, nullptr, config, frame, &input, &promoted));

    SignalGraph graph;
    REQUIRE(register_builtin_sample_region_types(graph));
    const auto& delay = *graph.sample_kernel_type("pulp.core.unit-delay", 1);
    alignas(float) float state = 0.0f;
    CHECK_FALSE(sample_kernel_storage_is_disjoint(delay, &state, config, {}, &input, &state));

    auto config_alias = reinterpret_cast<float*>(&config);
    CHECK_FALSE(
        sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, &input, config_alias));

    float partial[] = {1.0f, 2.0f};
    scalar.num_input_ports = 2;
    CHECK_FALSE(
        sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, partial, partial + 1));
    scalar = descriptor();
    CHECK_FALSE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, nullptr, &output));
    CHECK_FALSE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, &input, nullptr));
    CHECK_FALSE(
        sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, config_alias, &output));
    SampleFrameContext config_promoted{config_alias, 1, 0};
    CHECK_FALSE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, config_promoted, &input,
                                                  &output));
    CHECK_FALSE(sample_kernel_storage_is_disjoint(delay, &config, config, {}, &input, &output));

    const auto wrapping = reinterpret_cast<float*>(std::numeric_limits<std::uintptr_t>::max() - 1);
    CHECK_FALSE(sample_kernel_storage_is_disjoint(scalar, nullptr, config, {}, &input, wrapping));
}
