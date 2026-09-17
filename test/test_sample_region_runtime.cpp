#include "harness/scoped_rt_process_probe.hpp"
#include "support/audio_signal_generators.hpp"
#include "support/render_scenario.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/sample_region_runtime.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace pulp;
using namespace pulp::host;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kPreparedMaximum = 1024;
constexpr state::ParamID kCoefficientId = 2901;
constexpr SampleRegionId kRegionId = 901;

SampleKernelConfig none() {
    return {SampleKernelConfigKind::None, 0, 0.0f};
}

SampleKernelConfig boundary(std::uint32_t index = 0) {
    return {SampleKernelConfigKind::BoundaryIndex, index, 0.0f};
}

SampleKernelConfig constant(float value) {
    return {SampleKernelConfigKind::FiniteConstant, 0, value};
}

SampleKernelConfig parameter(state::ParamID id) {
    return {SampleKernelConfigKind::PromotedParameterId, id, 0.0f};
}

class CountingSource final : public PluginSlot {
  public:
    CountingSource() {
        info_.name = "SampleRegionAnticipationSource";
        info_.format = PluginFormat::BuiltIn;
        info_.num_inputs = 0;
        info_.num_outputs = 1;
        info_.category = "Generator";
    }

    const PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 const midi::MidiBuffer&, midi::MidiBuffer&, const ParameterEventQueue&,
                 int frames) override {
        const float value = static_cast<float>(blocks_.load(std::memory_order_relaxed) + 1);
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill_n(output.channel_ptr(channel), frames, value);
        blocks_.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<HostParamInfo> parameters() const override {
        return {};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return true;
    }
    int latency_samples() const override {
        return 0;
    }
    int tail_samples() const override {
        return 0;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}

    int blocks() const noexcept {
        return blocks_.load(std::memory_order_relaxed);
    }

  private:
    PluginInfo info_;
    std::atomic<int> blocks_{0};
};

struct AllpassFixture {
    // The parameter binding is borrowed by graph snapshots, so its owner must
    // be destroyed after graph.
    std::unique_ptr<SampleRegionParameterOwner> parameters;
    SignalGraph graph;
    NodeId graph_input = 0;
    NodeId outer_gain = 0;
    NodeId graph_output = 0;
    NodeId region_input = 0;
    NodeId region_output = 0;

    explicit AllpassFixture(float coefficient = 0.5f, double sample_rate = kSampleRate,
                            bool routed = true, bool parallel = false, bool anticipation = false,
                            bool dry_branch = false, int maximum = kPreparedMaximum) {
        graph_input = graph.add_input_node(1, "Input");
        outer_gain = graph.add_gain_node("Unrelated outer gain");
        graph_output = graph.add_output_node(1, "Output");
        REQUIRE(graph.connect(graph_input, 0, outer_gain, 0));
        REQUIRE(graph.connect(outer_gain, 0, graph_output, 0));
        REQUIRE(graph.set_node_gain(outer_gain, 1.0f));
        REQUIRE(graph.prepare(sample_rate, maximum));

        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit);
        REQUIRE(edit->disconnect(outer_gain, 0, graph_output, 0));
        REQUIRE(register_builtin_sample_region_types(*edit));

        region_input = edit->add_custom_node("pulp.core.sample-region.input");
        const auto coefficient_node = edit->add_custom_node("pulp.core.sample-region.parameter");
        const auto minus_one = edit->add_custom_node("pulp.core.sample-region.constant");
        const auto ax = edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto x_delay = edit->add_custom_node("pulp.core.unit-delay");
        const auto y_delay = edit->add_custom_node("pulp.core.unit-delay");
        const auto a_times_y_delay = edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto negative_a_times_y_delay =
            edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto sum_one = edit->add_custom_node("pulp.core.sample-region.add");
        const auto y = edit->add_custom_node("pulp.core.sample-region.add");
        region_output = edit->add_custom_node("pulp.core.sample-region.output");

        REQUIRE(edit->connect(outer_gain, 0, region_input, 0));
        REQUIRE(edit->connect(region_input, 0, ax, 0));
        REQUIRE(edit->connect(coefficient_node, 0, ax, 1));
        REQUIRE(edit->connect(region_input, 0, x_delay, 0));
        REQUIRE(edit->connect(y_delay, 0, a_times_y_delay, 0));
        REQUIRE(edit->connect(coefficient_node, 0, a_times_y_delay, 1));
        REQUIRE(edit->connect(minus_one, 0, negative_a_times_y_delay, 0));
        REQUIRE(edit->connect(a_times_y_delay, 0, negative_a_times_y_delay, 1));
        REQUIRE(edit->connect(ax, 0, sum_one, 0));
        REQUIRE(edit->connect(x_delay, 0, sum_one, 1));
        REQUIRE(edit->connect(sum_one, 0, y, 0));
        REQUIRE(edit->connect(negative_a_times_y_delay, 0, y, 1));
        REQUIRE(edit->connect(y, 0, region_output, 0));
        REQUIRE(edit->connect(region_output, 0, graph_output, 0));
        if (dry_branch)
            REQUIRE(edit->connect(outer_gain, 0, graph_output, 0));

        SampleRegionDefinition definition;
        definition.region_id = kRegionId;
        definition.members = {
            {region_input, "pulp.core.sample-region.input", 1, boundary()},
            {coefficient_node, "pulp.core.sample-region.parameter", 1, parameter(kCoefficientId)},
            {minus_one, "pulp.core.sample-region.constant", 1, constant(-1.0f)},
            {ax, "pulp.core.sample-region.multiply", 1, none()},
            {x_delay, "pulp.core.unit-delay", 1, none()},
            {y_delay, "pulp.core.unit-delay", 1, none()},
            {a_times_y_delay, "pulp.core.sample-region.multiply", 1, none()},
            {negative_a_times_y_delay, "pulp.core.sample-region.multiply", 1, none()},
            {sum_one, "pulp.core.sample-region.add", 1, none()},
            {y, "pulp.core.sample-region.add", 1, none()},
            {region_output, "pulp.core.sample-region.output", 1, boundary()},
        };
        definition.input_boundaries = {region_input};
        definition.output_boundaries = {region_output};
        SampleRegionPromotedParameter promoted;
        promoted.param_id = kCoefficientId;
        promoted.key = "coefficient";
        promoted.name = "Allpass Coefficient";
        promoted.range = state::ParamRange::linear(-0.99f, 0.99f, 0.5f);
        promoted.bound_node_id = coefficient_node;
        definition.promoted_parameters = {promoted};

        REQUIRE(edit->declare_sample_region(definition).accepted);
        // The only authored cycle is legal because this edge enters UnitDelay.
        REQUIRE(edit->connect_in_sample_region(kRegionId, y, 0, y_delay, 0).accepted);
        REQUIRE(edit->prove_sample_region(kRegionId).accepted);

        parameters =
            SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
        REQUIRE(parameters);
        parameters->store().set_value(kCoefficientId, coefficient);
        REQUIRE(edit->bind_sample_region_parameters(parameters->binding()).accepted);
        edit->set_canonical_executor_routing_enabled(routed);
        edit->set_parallel_routing_enabled(parallel);
        edit->set_anticipation_enabled(anticipation);
        const auto prepare_result = anticipation ? edit->prepare_quiesced(sample_rate, maximum)
                                                 : edit->prepare(sample_rate, maximum);
        REQUIRE(prepare_result == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(edit->routed_execution_ready(maximum) == routed);
        REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    }

    void set_coefficient(float value) {
        parameters->store().set_value(kCoefficientId, value);
    }
};

CountingSource* attach_counting_source(AllpassFixture& fixture, bool anticipation, int maximum) {
    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->disconnect(fixture.graph_input, 0, fixture.outer_gain, 0));
    auto source = std::make_unique<CountingSource>();
    auto* probe = source.get();
    const auto source_id =
        edit->add_owned_builtin_plugin_node(std::move(source), 0, 1, "Anticipation source");
    REQUIRE(source_id != 0);
    REQUIRE(edit->connect(source_id, 0, fixture.outer_gain, 0));
    edit->set_anticipation_enabled(anticipation);
    const auto prepared = anticipation ? edit->prepare_quiesced(kSampleRate, maximum)
                                       : edit->prepare(kSampleRate, maximum);
    REQUIRE(prepared == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    return probe;
}

std::vector<float> render(SignalGraph& graph, std::span<const float> input,
                          std::span<const int> schedule) {
    REQUIRE_FALSE(schedule.empty());
    std::vector<float> output(input.size(), 0.0f);
    std::size_t offset = 0;
    std::size_t step = 0;
    while (offset < input.size()) {
        const auto requested = static_cast<std::size_t>(schedule[step % schedule.size()]);
        REQUIRE(requested != 0);
        const auto count = std::min(requested, input.size() - offset);
        const float* input_channels[] = {input.data() + offset};
        float* output_channels[] = {output.data() + offset};
        audio::BufferView<const float> in(input_channels, 1, count);
        audio::BufferView<float> out(output_channels, 1, count);
        graph.process(out, in, static_cast<int>(count));
        offset += count;
        ++step;
    }
    return output;
}

std::vector<float> render(SignalGraph& graph, std::span<const float> input, int block) {
    const std::array schedule{block};
    return render(graph, input, schedule);
}

std::vector<float> allpass_oracle(std::span<const float> input, float coefficient) {
    std::vector<float> output(input.size(), 0.0f);
    float previous_input = 0.0f;
    float previous_output = 0.0f;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float current =
            coefficient * input[i] + previous_input - coefficient * previous_output;
        output[i] = current;
        previous_input = input[i];
        previous_output = current;
    }
    return output;
}

void require_near(std::span<const float> actual, std::span<const float> expected,
                  float tolerance = 1.0e-6f) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CAPTURE(i, actual[i], expected[i]);
        REQUIRE(std::abs(actual[i] - expected[i]) <= tolerance);
    }
}

void require_exact(std::span<const float> actual, std::span<const float> expected) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        CAPTURE(i, actual[i], expected[i]);
        REQUIRE(actual[i] == expected[i]);
    }
}

std::vector<float> seeded_input(int frames) {
    const auto generated = test::audio::make_white_noise(1, frames, 0x51a7e2u, 0.25f);
    const auto channel = generated.channel(0);
    return {channel.begin(), channel.end()};
}

audio::BufferView<const float> mono_view(const std::vector<float>& samples,
                                         std::array<const float*, 1>& channels) {
    channels = {samples.data()};
    return {channels.data(), 1, samples.size()};
}

std::vector<float> render_offline(AllpassFixture& fixture, const std::vector<float>& input,
                                  int block, double sample_rate = kSampleRate) {
    OfflineSignalGraphHost host(fixture.graph);
    OfflineSignalGraphConfig config;
    config.sample_rate = sample_rate;
    config.block_frames = block;
    config.input_channels = 1;
    config.output_channels = 1;
    REQUIRE(host.prepare(config));
    std::array<const float*, 1> channels{};
    OfflineSignalGraphOptions options;
    options.frame_count = input.size();
    options.input = mono_view(input, channels);
    const auto result = host.render(options);
    REQUIRE(result.ok);
    const auto channel = result.audio.channel(0);
    return {channel.begin(), channel.end()};
}

struct StateProbe {
    float value = 0.0f;
};

struct StateProbeCounters {
    std::atomic<int> constructs{0};
    std::atomic<int> resets{0};
    std::atomic<int> destroys{0};
    std::atomic<int> publishes{0};
    std::atomic<int> commits{0};
    std::thread::id expected_destroy_thread;
    std::atomic<bool> destroyed_on_expected_thread{false};
};

StateProbeCounters g_state_probe;

void reset_state_probe() {
    g_state_probe.constructs.store(0);
    g_state_probe.resets.store(0);
    g_state_probe.destroys.store(0);
    g_state_probe.publishes.store(0);
    g_state_probe.commits.store(0);
    g_state_probe.expected_destroy_thread = {};
    g_state_probe.destroyed_on_expected_thread.store(false);
}

bool probe_construct(void* storage, const SampleKernelPrepareContext&) noexcept {
    std::construct_at(static_cast<StateProbe*>(storage));
    g_state_probe.constructs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void probe_reset(void* storage) noexcept {
    static_cast<StateProbe*>(storage)->value = 0.0f;
    g_state_probe.resets.fetch_add(1, std::memory_order_relaxed);
}

void probe_destroy(void* storage) noexcept {
    std::destroy_at(static_cast<StateProbe*>(storage));
    g_state_probe.destroys.fetch_add(1, std::memory_order_relaxed);
    g_state_probe.destroyed_on_expected_thread.store(std::this_thread::get_id() ==
                                                         g_state_probe.expected_destroy_thread,
                                                     std::memory_order_relaxed);
}

void probe_publish(const void* storage, const PreparedSampleKernelConfig&, float* output) noexcept {
    output[0] = static_cast<const StateProbe*>(storage)->value;
    g_state_probe.publishes.fetch_add(1, std::memory_order_relaxed);
}

void probe_commit(void* storage, const PreparedSampleKernelConfig&, const float* input) noexcept {
    static_cast<StateProbe*>(storage)->value = input[0];
    g_state_probe.commits.fetch_add(1, std::memory_order_relaxed);
}

void pass_scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                 const float* input, float* output) noexcept {
    output[0] = input[0];
}

SampleKernelDescriptor boundary_descriptor(std::string id) {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = std::move(id);
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = SampleKernelConfigKind::BoundaryIndex;
    descriptor.process = pass_scalar;
    descriptor.metadata.category = "test";
    return descriptor;
}

SampleKernelDescriptor probe_delay_descriptor() {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = "pulp.test.sample-region.probe-delay";
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.causality = SampleKernelCausality::OneSampleDelay;
    descriptor.authored_config_kind = SampleKernelConfigKind::None;
    descriptor.state_size = sizeof(StateProbe);
    descriptor.state_alignment = alignof(StateProbe);
    descriptor.construct = probe_construct;
    descriptor.reset = probe_reset;
    descriptor.destroy = probe_destroy;
    descriptor.delay_publish = probe_publish;
    descriptor.delay_commit = probe_commit;
    descriptor.metadata.category = "test";
    return descriptor;
}

CustomNodeType scalar_node_type(std::string type_id, int inputs = 1, int outputs = 1) {
    CustomNodeType type;
    type.type_id = std::move(type_id);
    type.version = 1;
    type.num_input_ports = inputs;
    type.num_output_ports = outputs;
    type.default_name = type.type_id;
    return type;
}

bool float_probe_construct(void* state, const SampleKernelPrepareContext&) noexcept {
    if (state == nullptr)
        return false;
    *static_cast<float*>(state) = 0.0f;
    g_state_probe.constructs.fetch_add(1);
    return true;
}

void float_probe_reset(void* state) noexcept {
    if (state != nullptr)
        *static_cast<float*>(state) = 0.0f;
    g_state_probe.resets.fetch_add(1);
}

void float_probe_destroy(void*) noexcept {
    if (g_state_probe.expected_destroy_thread == std::this_thread::get_id())
        g_state_probe.destroyed_on_expected_thread.store(true);
    g_state_probe.destroys.fetch_add(1);
}

void float_probe_publish(const void* state, const PreparedSampleKernelConfig&,
                         float* outputs) noexcept {
    outputs[0] = state != nullptr ? *static_cast<const float*>(state) : 0.0f;
    g_state_probe.publishes.fetch_add(1);
}

void float_probe_commit(void* state, const PreparedSampleKernelConfig&,
                        const float* inputs) noexcept {
    if (state != nullptr)
        *static_cast<float*>(state) = inputs[0];
    g_state_probe.commits.fetch_add(1);
}

SampleKernelDescriptor instrumented_unit_delay_descriptor() {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = "pulp.core.unit-delay";
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.causality = SampleKernelCausality::OneSampleDelay;
    descriptor.authored_config_kind = SampleKernelConfigKind::None;
    descriptor.state_size = sizeof(float);
    descriptor.state_alignment = alignof(float);
    descriptor.construct = float_probe_construct;
    descriptor.reset = float_probe_reset;
    descriptor.destroy = float_probe_destroy;
    descriptor.delay_publish = float_probe_publish;
    descriptor.delay_commit = float_probe_commit;
    descriptor.metadata.category = "test";
    return descriptor;
}

PreparedSampleRegionPlan probe_plan(NodeId delay_node = 12) {
    const PreparedSampleKernelConfig boundary_config{PreparedSampleKernelConfigKind::BoundaryIndex,
                                                     0, 0.0f};
    const PreparedSampleKernelConfig none_config{PreparedSampleKernelConfigKind::None, 0, 0.0f};
    PreparedSampleRegionPlan plan;
    plan.region_id = 77;
    plan.scalar_slot_count = 6;
    plan.resources.member_nodes = 3;
    plan.resources.internal_connections = 2;
    plan.resources.input_boundaries = 1;
    plan.resources.output_boundaries = 1;
    plan.resources.delay_nodes = 1;
    plan.resources.state_bytes = sizeof(StateProbe);
    plan.resources.state_alignment = alignof(StateProbe);
    plan.resources.logical_boundary_bytes = 2 * sizeof(float);
    plan.resources.work_per_frame = 8;
    plan.kernels = {
        {11, boundary_descriptor("pulp.core.sample-region.input"), boundary_config, 0, 1},
        {delay_node, probe_delay_descriptor(), none_config, 2, 3},
        {13, boundary_descriptor("pulp.core.sample-region.output"), boundary_config, 4, 5},
    };
    plan.delay_publish_order = {1};
    plan.combinational_order = {0, 2};
    plan.delay_commit_order = {1};
    plan.transfers = {
        {11, 0, delay_node, 0, 1, 2},
        {delay_node, 0, 13, 0, 3, 4},
    };
    plan.operations = {
        {PreparedSampleRegionOperationKind::DelayPublish, 1},
        {PreparedSampleRegionOperationKind::Transfer, 1},
        {PreparedSampleRegionOperationKind::Process, 0},
        {PreparedSampleRegionOperationKind::Transfer, 0},
        {PreparedSampleRegionOperationKind::Process, 2},
        {PreparedSampleRegionOperationKind::DelayCommit, 1},
    };
    return plan;
}

float process_probe_region(PreparedSampleRegion& region, float input) {
    float output = -99.0f;
    const float* input_channels[] = {&input};
    float* output_channels[] = {&output};
    audio::BufferView<const float> in(input_channels, 1, 1);
    audio::BufferView<float> out(output_channels, 1, 1);
    region.process(out, in, 1);
    return output;
}

class SampleRegionAllpassProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "SampleRegionAllpassFixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.sample-region-allpass",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 1}},
            .output_buses = {{"Audio Out", 1}},
        };
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const format::PrepareContext& context) override {
        parameters_.reset();
        prepared_ = build_graph_(context.sample_rate, context.max_buffer_size);
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext& context) override {
        const auto frames =
            context.num_samples > 0
                ? context.num_samples
                : static_cast<int>(std::min(output.num_samples(), input.num_samples()));
        if (!prepared_) {
            output.clear();
            return;
        }
        graph_.process(output, input, frames, context);
    }

  private:
    bool build_graph_(double sample_rate, int maximum) {
        const auto graph_input = graph_.add_input_node(1, "Input");
        const auto outer_gain = graph_.add_gain_node("Unrelated outer gain");
        const auto graph_output = graph_.add_output_node(1, "Output");
        if (!graph_.connect(graph_input, 0, outer_gain, 0) ||
            !graph_.connect(outer_gain, 0, graph_output, 0) ||
            !graph_.set_node_gain(outer_gain, 1.0f) || !graph_.prepare(sample_rate, maximum)) {
            return false;
        }

        auto edit = graph_.begin_prepared_topology_edit();
        if (!edit || !edit->disconnect(outer_gain, 0, graph_output, 0) ||
            !register_builtin_sample_region_types(*edit)) {
            return false;
        }

        const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
        const auto coefficient_node = edit->add_custom_node("pulp.core.sample-region.parameter");
        const auto minus_one = edit->add_custom_node("pulp.core.sample-region.constant");
        const auto ax = edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto x_delay = edit->add_custom_node("pulp.core.unit-delay");
        const auto y_delay = edit->add_custom_node("pulp.core.unit-delay");
        const auto a_times_y_delay = edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto negative_a_times_y_delay =
            edit->add_custom_node("pulp.core.sample-region.multiply");
        const auto sum_one = edit->add_custom_node("pulp.core.sample-region.add");
        const auto y = edit->add_custom_node("pulp.core.sample-region.add");
        const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");

        if (!edit->connect(outer_gain, 0, region_input, 0) ||
            !edit->connect(region_input, 0, ax, 0) || !edit->connect(coefficient_node, 0, ax, 1) ||
            !edit->connect(region_input, 0, x_delay, 0) ||
            !edit->connect(y_delay, 0, a_times_y_delay, 0) ||
            !edit->connect(coefficient_node, 0, a_times_y_delay, 1) ||
            !edit->connect(minus_one, 0, negative_a_times_y_delay, 0) ||
            !edit->connect(a_times_y_delay, 0, negative_a_times_y_delay, 1) ||
            !edit->connect(ax, 0, sum_one, 0) || !edit->connect(x_delay, 0, sum_one, 1) ||
            !edit->connect(sum_one, 0, y, 0) || !edit->connect(negative_a_times_y_delay, 0, y, 1) ||
            !edit->connect(y, 0, region_output, 0) ||
            !edit->connect(region_output, 0, graph_output, 0)) {
            return false;
        }

        SampleRegionDefinition definition;
        definition.region_id = kRegionId;
        definition.members = {
            {region_input, "pulp.core.sample-region.input", 1, boundary()},
            {coefficient_node, "pulp.core.sample-region.parameter", 1, parameter(kCoefficientId)},
            {minus_one, "pulp.core.sample-region.constant", 1, constant(-1.0f)},
            {ax, "pulp.core.sample-region.multiply", 1, none()},
            {x_delay, "pulp.core.unit-delay", 1, none()},
            {y_delay, "pulp.core.unit-delay", 1, none()},
            {a_times_y_delay, "pulp.core.sample-region.multiply", 1, none()},
            {negative_a_times_y_delay, "pulp.core.sample-region.multiply", 1, none()},
            {sum_one, "pulp.core.sample-region.add", 1, none()},
            {y, "pulp.core.sample-region.add", 1, none()},
            {region_output, "pulp.core.sample-region.output", 1, boundary()},
        };
        definition.input_boundaries = {region_input};
        definition.output_boundaries = {region_output};
        SampleRegionPromotedParameter promoted;
        promoted.param_id = kCoefficientId;
        promoted.key = "coefficient";
        promoted.name = "Allpass Coefficient";
        promoted.range = state::ParamRange::linear(-0.99f, 0.99f, 0.5f);
        promoted.bound_node_id = coefficient_node;
        definition.promoted_parameters = {promoted};

        if (!edit->declare_sample_region(definition).accepted ||
            !edit->connect_in_sample_region(kRegionId, y, 0, y_delay, 0).accepted ||
            !edit->prove_sample_region(kRegionId).accepted) {
            return false;
        }

        parameters_ =
            SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
        if (!parameters_)
            return false;
        parameters_->store().set_value(kCoefficientId, 0.5f);
        if (!edit->bind_sample_region_parameters(parameters_->binding()).accepted)
            return false;
        edit->set_canonical_executor_routing_enabled(true);
        if (edit->prepare(sample_rate, maximum) !=
                SignalGraph::PreparedTopologyEdit::Result::Prepared ||
            !edit->routed_execution_ready(maximum) ||
            edit->commit() != SignalGraph::PreparedTopologyEdit::Result::Committed) {
            return false;
        }
        return true;
    }

    std::unique_ptr<SampleRegionParameterOwner> parameters_;
    SignalGraph graph_;
    bool prepared_ = false;
};

std::unique_ptr<format::Processor> create_sample_region_allpass_processor() {
    return std::make_unique<SampleRegionAllpassProcessor>();
}

} // namespace

TEST_CASE("Sample region state banks retain exact keys without reading active bytes",
          "[sample-region][runtime][continuity][state-bank]") {
    reset_state_probe();
    const auto plan = probe_plan();
    const std::array plans{plan};
    auto fresh = SampleRegionStateBank::create_fresh(plans, kSampleRate, 64, 1);
    REQUIRE(fresh);
    CHECK(fresh->binding_generation() == 1);
    CHECK(fresh->state_bytes() == sizeof(StateProbe));
    REQUIRE(fresh->ordered_cells().size() == 1);
    CHECK(fresh->ordered_cells()[0]->alignment() == alignof(StateProbe));
    auto first = PreparedSampleRegion::create(plan, fresh, nullptr);
    REQUIRE(first);
    CHECK(first->receipt().fresh_state_cells == 1);
    CHECK(first->receipt().retained_state_cells == 0);
    CHECK(first->receipt().region_id == plan.region_id);
    CHECK(first->receipt().resources.member_nodes == 3);
    CHECK(first->receipt().resources.delay_nodes == 1);
    CHECK(first->receipt().resources.state_bytes == sizeof(StateProbe));
    CHECK(first->receipt().resources.state_alignment == alignof(StateProbe));
    CHECK(first->receipt().resources.logical_boundary_bytes == 2 * sizeof(float));
    CHECK(first->receipt().resources.work_per_frame == 8);
    CHECK(first->receipt().resources.work_per_block == 0);
    CHECK(first->receipt().scalar_slots == 6);
    CHECK(first->receipt().physical_executor_bytes > 0);
    CHECK(first->receipt().private_boundary_copy_bytes == 0);

    CHECK(process_probe_region(*first, 0.75f) == 0.0f);
    const int publishes_before_adopt = g_state_probe.publishes.load();
    const int commits_before_adopt = g_state_probe.commits.load();
    const int constructs_before_adopt = g_state_probe.constructs.load();
    auto adopted = SampleRegionStateBank::adopt(plans, *fresh, kSampleRate, 64, 2);
    REQUIRE(adopted);
    CHECK(g_state_probe.publishes.load() == publishes_before_adopt);
    CHECK(g_state_probe.commits.load() == commits_before_adopt);
    CHECK(g_state_probe.constructs.load() == constructs_before_adopt);
    CHECK(&adopted->domain() == &fresh->domain());
    const SampleRegionStateKey key{77, 12, "pulp.test.sample-region.probe-delay", 1};
    CHECK(adopted->was_retained(key));
    CHECK(adopted->cell(key) == fresh->cell(key));
    auto second = PreparedSampleRegion::create(plan, adopted, nullptr);
    REQUIRE(second);
    CHECK(second->receipt().region_id == plan.region_id);
    CHECK(second->receipt().resources.member_nodes == first->receipt().resources.member_nodes);
    CHECK(second->receipt().resources.internal_connections ==
          first->receipt().resources.internal_connections);
    CHECK(second->receipt().resources.delay_nodes == first->receipt().resources.delay_nodes);
    CHECK(second->receipt().resources.state_bytes == first->receipt().resources.state_bytes);
    CHECK(second->receipt().resources.state_alignment ==
          first->receipt().resources.state_alignment);
    CHECK(second->receipt().resources.logical_boundary_bytes ==
          first->receipt().resources.logical_boundary_bytes);
    CHECK(second->receipt().resources.work_per_frame == first->receipt().resources.work_per_frame);
    CHECK(second->receipt().resources.work_per_block == first->receipt().resources.work_per_block);
    CHECK(second->receipt().physical_executor_bytes == first->receipt().physical_executor_bytes);
    CHECK(second->receipt().private_boundary_copy_bytes == 0);
    CHECK(second->receipt().retained_state_cells == 1);
    CHECK(second->receipt().fresh_state_cells == 0);
    CHECK(process_probe_region(*second, 0.0f) == 0.75f);

    auto reset_bank = SampleRegionStateBank::create_fresh(plans, kSampleRate, 64, 3);
    REQUIRE(reset_bank);
    auto reset_region = PreparedSampleRegion::create(plan, reset_bank, nullptr);
    REQUIRE(reset_region);
    CHECK(process_probe_region(*reset_region, 0.0f) == 0.0f);
    CHECK(g_state_probe.constructs.load() == constructs_before_adopt + 1);
}

TEST_CASE("Sample region state retention rejects changed prepared kernel configuration",
          "[sample-region][runtime][continuity][state-bank]") {
    reset_state_probe();
    auto first_plan = probe_plan();
    auto& first_delay = first_plan.kernels[1];
    first_delay.descriptor.authored_config_kind = SampleKernelConfigKind::FiniteConstant;
    first_delay.config = {PreparedSampleKernelConfigKind::FiniteConstant, 0, 0.25f};
    const std::array first_plans{first_plan};
    auto first = SampleRegionStateBank::create_fresh(first_plans, kSampleRate, 64, 1);
    REQUIRE(first);
    REQUIRE(g_state_probe.constructs.load() == 1);

    auto changed_plan = first_plan;
    changed_plan.kernels[1].config.constant = 0.75f;
    const std::array changed_plans{changed_plan};
    auto changed = SampleRegionStateBank::adopt(changed_plans, *first, kSampleRate, 64, 2);
    REQUIRE(changed);
    const SampleRegionStateKey key{77, 12, "pulp.test.sample-region.probe-delay", 1};
    CHECK_FALSE(changed->was_retained(key));
    CHECK(changed->cell(key) != first->cell(key));
    CHECK(g_state_probe.constructs.load() == 2);
}

TEST_CASE("Sample region execution domain excludes overlap and rejects old reentry",
          "[sample-region][runtime][concurrency]") {
    reset_state_probe();
    const auto plan = probe_plan();
    const std::array plans{plan};
    auto old_bank = SampleRegionStateBank::create_fresh(plans, kSampleRate, 64, 10);
    REQUIRE(old_bank);
    auto new_bank = SampleRegionStateBank::adopt(plans, *old_bank, kSampleRate, 64, 11);
    REQUIRE(new_bank);

    {
        auto old_callback = old_bank->domain().try_admit(old_bank->binding_generation());
        REQUIRE(old_callback);
        auto overlapping_new = new_bank->domain().try_admit(new_bank->binding_generation());
        CHECK_FALSE(overlapping_new);
        CHECK(old_bank->domain().adopted_generation() == 10);
    }
    {
        auto new_callback = new_bank->domain().try_admit(new_bank->binding_generation());
        REQUIRE(new_callback);
        CHECK(new_bank->domain().adopted_generation() == 11);
    }
    auto stale_old = old_bank->domain().try_admit(old_bank->binding_generation());
    CHECK_FALSE(stale_old);
    CHECK(new_bank->domain().adopted_generation() == 11);
}

TEST_CASE("Removed sample region state cells retire on the control thread",
          "[sample-region][runtime][concurrency][retirement]") {
    reset_state_probe();
    g_state_probe.expected_destroy_thread = std::this_thread::get_id();
    const auto plan = probe_plan();
    const std::array plans{plan};
    auto old_bank = SampleRegionStateBank::create_fresh(plans, kSampleRate, 64, 20);
    REQUIRE(old_bank);
    auto old_region = PreparedSampleRegion::create(plan, old_bank, nullptr);
    REQUIRE(old_region);
    const std::span<const PreparedSampleRegionPlan> no_plans;
    auto removed = SampleRegionStateBank::adopt(no_plans, *old_bank, kSampleRate, 64, 21);
    REQUIRE(removed);
    CHECK(removed->ordered_cells().empty());
    CHECK(g_state_probe.destroys.load() == 0);

    old_region.reset();
    CHECK(g_state_probe.destroys.load() == 0);
    old_bank.reset();
    CHECK(g_state_probe.destroys.load() == 1);
    CHECK(g_state_probe.destroyed_on_expected_thread.load());
}

TEST_CASE("Prepared sample region process and reset hooks are realtime safe",
          "[sample-region][runtime][rt-safety][reset]") {
    reset_state_probe();
    const auto plan = probe_plan();
    const std::array plans{plan};
    auto bank = SampleRegionStateBank::create_fresh(plans, kSampleRate, 64, 30);
    REQUIRE(bank);
    auto region = PreparedSampleRegion::create(plan, bank, nullptr);
    REQUIRE(region);
    std::size_t allocations = 0;
    std::size_t bytes = 0;
    float first = 0.0f;
    float after_reset = 0.0f;
    {
        test::ScopedRtProcessProbe probe;
        first = process_probe_region(*region, 0.5f);
        bank->reset();
        after_reset = process_probe_region(*region, 0.0f);
        allocations = probe.allocation_count();
        bytes = probe.allocated_bytes();
    }
    CHECK(first == 0.0f);
    CHECK(after_reset == 0.0f);
    CHECK(allocations == 0);
    CHECK(bytes == 0);
    CHECK(g_state_probe.resets.load() == 1);
    CHECK(g_state_probe.constructs.load() == 1);
    CHECK(g_state_probe.destroys.load() == 0);
}

TEST_CASE("Pinned sample region snapshots reset state exactly once without reinitializing",
          "[sample-region][runtime][rt-safety][reset][snapshot]") {
    reset_state_probe();
    SignalGraph graph;
    const auto graph_input = graph.add_input_node(1, "Input");
    const auto graph_output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(graph_input, 0, graph_output, 0));
    REQUIRE(graph.prepare(kSampleRate, 64));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->disconnect(graph_input, 0, graph_output, 0));
    REQUIRE(edit->register_custom_node_type(scalar_node_type("pulp.core.sample-region.input"),
                                            boundary_descriptor("pulp.core.sample-region.input")));
    REQUIRE(edit->register_custom_node_type(scalar_node_type("pulp.core.sample-region.output"),
                                            boundary_descriptor("pulp.core.sample-region.output")));
    REQUIRE(edit->register_custom_node_type(scalar_node_type("pulp.core.unit-delay"),
                                            instrumented_unit_delay_descriptor()));
    const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto delay = edit->add_custom_node("pulp.core.unit-delay");
    const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");
    REQUIRE(edit->connect(graph_input, 0, region_input, 0));
    REQUIRE(edit->connect(region_input, 0, delay, 0));
    REQUIRE(edit->connect(delay, 0, region_output, 0));
    REQUIRE(edit->connect(region_output, 0, graph_output, 0));

    SampleRegionDefinition definition;
    definition.region_id = kRegionId + 2;
    definition.members = {
        {region_input, "pulp.core.sample-region.input", 1, boundary()},
        {delay, "pulp.core.unit-delay", 1, none()},
        {region_output, "pulp.core.sample-region.output", 1, boundary()},
    };
    definition.input_boundaries = {region_input};
    definition.output_boundaries = {region_output};
    REQUIRE(edit->declare_sample_region(definition).accepted);
    REQUIRE(edit->prove_sample_region(definition.region_id).accepted);
    auto parameters =
        SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(parameters);
    REQUIRE(edit->bind_sample_region_parameters(parameters->binding()).accepted);
    edit->set_canonical_executor_routing_enabled(true);
    REQUIRE(edit->prepare(kSampleRate, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    REQUIRE(snapshot);

    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{-1.0f};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    audio::BufferView<const float> in(input_channels, 1, input.size());
    audio::BufferView<float> out(output_channels, 1, output.size());
    snapshot.process(out, in, 1);
    CHECK(output[0] == 0.0f);

    input[0] = 0.0f;
    output[0] = -1.0f;
    format::ProcessContext reset;
    reset.reset_requested = true;
    reset.num_samples = 1;
    reset.sample_rate = kSampleRate;
    std::size_t reset_allocations = 0;
    std::size_t reset_bytes = 0;
    {
        test::ScopedRtProcessProbe probe;
        snapshot.process(out, in, 1, reset);
        reset_allocations = probe.allocation_count();
        reset_bytes = probe.allocated_bytes();
    }
    CHECK(output[0] == 0.0f);
    CHECK(reset_allocations == 0);
    CHECK(reset_bytes == 0);
    CHECK(g_state_probe.resets.load() == 1);
    CHECK(g_state_probe.constructs.load() == 1);
    CHECK(g_state_probe.destroys.load() == 0);
}

TEST_CASE("Sample region quotient prepares commits and preserves authored readback",
          "[sample-region][runtime][quotient]") {
    AllpassFixture fixture;
    const auto region = fixture.graph.sample_region(kRegionId);
    REQUIRE(region);
    CHECK(region->members.size() == 11);
    CHECK(region->input_boundaries == std::vector<NodeId>{fixture.region_input});
    CHECK(region->output_boundaries == std::vector<NodeId>{fixture.region_output});
    CHECK(fixture.graph.prove_sample_region(kRegionId).accepted);

    // The public authoring model remains expanded while the compiled topology
    // contains graph input, the unrelated outer gain, one fused region anchor,
    // and graph output.
    CHECK(fixture.graph.nodes().size() == 14);
    CHECK(fixture.graph.prepared_stats().node_count == 4);
    CHECK(fixture.graph.prepared_stats().ordered_node_count == 4);
    CHECK(fixture.graph.prepared_stats().connection_count == 3);
    CHECK(fixture.graph.sample_region_binding_generation() > 0);
    const auto receipts = fixture.graph.sample_region_runtime_receipts();
    REQUIRE(receipts.size() == 1);
    CHECK(receipts[0].region_id == kRegionId);
    CHECK(receipts[0].resources.member_nodes == 11);
    CHECK(receipts[0].resources.delay_nodes == 2);
    CHECK(receipts[0].resources.state_bytes == 2 * sizeof(float));
    CHECK(receipts[0].resources.state_alignment == alignof(float));
    CHECK(receipts[0].resources.logical_boundary_bytes == 2 * kPreparedMaximum * sizeof(float));
    CHECK(receipts[0].resources.work_per_frame == 28);
    CHECK(receipts[0].resources.work_per_block == 28 * kPreparedMaximum);
    CHECK(receipts[0].physical_executor_bytes > 0);
    CHECK(receipts[0].private_boundary_copy_bytes == 0);
    CHECK(receipts[0].scalar_slots > 0);
    CHECK(receipts[0].fresh_state_cells == 2);
    CHECK(receipts[0].retained_state_cells == 0);

    const std::array<float, 4> impulse{1.0f, 0.0f, 0.0f, 0.0f};
    const auto output = render(fixture.graph, impulse, 4);
    REQUIRE(output[0] == Approx(0.5f));
    REQUIRE(output[1] == Approx(0.75f));
}

TEST_CASE("Sample region allpass matches its independent oracle and every partition",
          "[sample-region][runtime][parity][allpass]") {
    constexpr int kFrames = 4096;
    auto input = seeded_input(kFrames);
    input[0] = 1.0f;
    const auto expected = allpass_oracle(input, 0.5f);
    CHECK(expected[0] == 0.5f);
    CHECK(expected[1] == Approx(input[0] + 0.5f * input[1] - 0.5f * expected[0]));
    AllpassFixture one_sample_fixture;
    const auto one_sample_partition = render(one_sample_fixture.graph, input, 1);
    require_near(one_sample_partition, expected);

    for (const int block : {1, 7, 16, 63, 64, 127, 128, 257, 1024}) {
        CAPTURE(block);
        AllpassFixture fixture;
        const auto actual = render(fixture.graph, input, block);
        require_near(actual, expected);
        require_exact(actual, one_sample_partition);
    }

    const std::array irregular{1, 127, 3, 64, 17, 257, 5, 1024, 31};
    AllpassFixture fixture;
    const auto irregular_output = render(fixture.graph, input, irregular);
    require_near(irregular_output, expected);
    require_exact(irregular_output, one_sample_partition);
    std::array<const float*, 1> reference_channels{};
    std::array<const float*, 1> actual_channels{};
    const auto reference_view = mono_view(one_sample_partition, reference_channels);
    const auto actual_view = mono_view(irregular_output, actual_channels);
    const auto null_check = test::audio::assert_null_near(reference_view, actual_view, -180.0);
    INFO(null_check.message);
    CHECK(null_check.passed);

    audio::Buffer<float> scenario_input(1, input.size());
    std::copy(input.begin(), input.end(), scenario_input.channel(0).begin());
    const auto scenario = test::audio::RenderScenario(create_sample_region_allpass_processor)
                              .name("sample-region.allpass.partition")
                              .sample_rate(kSampleRate)
                              .block_size(128)
                              .channels(1, 1)
                              .input(std::move(scenario_input));
    const std::array partition_blocks{1, 7, 16, 63, 64, 127, 128, 257, 1024};
    const auto partition_check = test::audio::assert_block_partition_invariant(
        scenario, partition_blocks, test::audio::kExactPartitionToleranceDb);
    INFO(partition_check.message);
    CHECK(partition_check.passed);
}

TEST_CASE("Sample region allpass response phase and group delay match analysis oracles",
          "[sample-region][runtime][parity][allpass][doctor]") {
    constexpr int kFft = 16384;
    const std::array coefficients{-0.95f, -0.5f, 0.0f, 0.5f, 0.95f};
    const std::array sample_rates{44100.0, 48000.0, 96000.0, 192000.0};

    for (const double sample_rate : sample_rates) {
        const std::array checkpoints{50.0, 200.0, 1000.0, 5000.0,
                                     std::min(18000.0, 0.45 * sample_rate)};
        for (const float coefficient : coefficients) {
            CAPTURE(sample_rate, coefficient);
            auto noise_and_tail = seeded_input(2048);
            std::fill(noise_and_tail.begin() + 1024, noise_and_tail.end(), 0.0f);
            AllpassFixture stability_fixture(coefficient, sample_rate);
            const auto stability =
                render(stability_fixture.graph, noise_and_tail, std::array{1, 127, 3, 64, 17, 257});
            REQUIRE(std::all_of(stability.begin(), stability.end(),
                                [](float value) { return std::isfinite(value); }));

            std::vector<float> impulse(kFft, 0.0f);
            impulse[0] = 1.0f;
            AllpassFixture fixture(coefficient, sample_rate);
            const auto response = render(fixture.graph, impulse, 257);
            REQUIRE(std::all_of(response.begin(), response.end(),
                                [](float value) { return std::isfinite(value); }));

            std::array<const float*, 1> input_channels{};
            std::array<const float*, 1> output_channels{};
            const auto input_view = mono_view(impulse, input_channels);
            const auto output_view = mono_view(response, output_channels);
            test::audio::ResponseOptions response_options;
            response_options.fft_length = kFft;
            const auto magnitude = test::audio::response_relative_to_input(
                input_view, output_view, sample_rate, checkpoints, response_options);
            test::audio::GroupDelayOptions delay_options;
            delay_options.fft_length = kFft;
            const auto delay = test::audio::measure_group_delay(
                input_view, output_view, sample_rate, checkpoints, delay_options);

            for (std::size_t checkpoint = 0; checkpoint < checkpoints.size(); ++checkpoint) {
                const double frequency = checkpoints[checkpoint];
                const double measured_frequency =
                    std::round(frequency / delay.bin_hz) * delay.bin_hz;
                CAPTURE(frequency);
                CHECK(std::abs(magnitude.magnitude_db_at(frequency)) <= 0.02);
                REQUIRE(delay.defined_at(frequency));
                REQUIRE(delay.phase_defined_at(frequency));
                const double omega = 2.0 * std::numbers::pi * measured_frequency / sample_rate;
                const double a = coefficient;
                const double expected_delay =
                    (1.0 - a * a) / (1.0 + a * a + 2.0 * a * std::cos(omega));
                CHECK(delay.group_delay_samples_at(frequency) ==
                      Approx(expected_delay).margin(0.05));
                const std::complex<double> z_inv = std::polar(1.0, -omega);
                const auto transfer = (a + z_inv) / (1.0 + a * z_inv);
                const double phase_error = std::remainder(
                    delay.phase_radians_at(frequency) - std::arg(transfer), 2.0 * std::numbers::pi);
                CHECK(std::abs(phase_error) <= 0.01);
            }
        }
    }
}

TEST_CASE("Sample region reference serial parallel and offline paths agree",
          "[sample-region][runtime][parity]") {
    constexpr int kFrames = 4096;
    const auto input = seeded_input(kFrames);

    AllpassFixture reference(0.5f, kSampleRate, false, false);
    const auto expected = render(reference.graph, input, 127);

    AllpassFixture serial(0.5f, kSampleRate, true, false);
    REQUIRE(serial.graph.routed_execution_status(kPreparedMaximum).serial_selected);
    const auto serial_output = render(serial.graph, input, 127);
    require_exact(serial_output, expected);
    CHECK(serial.graph.routing_executor_stats().blocks_processed > 0);
    CHECK(serial.graph.routed_walk_fallbacks() == 0);

    AllpassFixture parallel(0.5f, kSampleRate, true, true);
    REQUIRE(parallel.graph.routed_execution_status(kPreparedMaximum).parallel_selected);
    const auto parallel_output = render(parallel.graph, input, 127);
    require_exact(parallel_output, expected);
    CHECK(parallel.graph.routing_executor_stats().blocks_processed > 0);
    CHECK(parallel.graph.routed_walk_fallbacks() == 0);

    AllpassFixture offline;
    const auto offline_output = render_offline(offline, input, 127);
    require_exact(offline_output, expected);
}

TEST_CASE("Sample region state survives adopted edits and resets on full prepare",
          "[sample-region][runtime][continuity][reset]") {
    const std::array impulse{1.0f};
    const std::array silence{0.0f};
    AllpassFixture fixture;
    CHECK(render(fixture.graph, impulse, 1)[0] == Approx(0.5f));

    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit->set_node_gain(fixture.outer_gain, 1.0f));
    REQUIRE(edit->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(render(fixture.graph, silence, 1)[0] == Approx(0.75f));

    REQUIRE(fixture.graph.prepare(kSampleRate, kPreparedMaximum));
    CHECK(render(fixture.graph, silence, 1)[0] == 0.0f);

    CHECK(render(fixture.graph, impulse, 1)[0] == Approx(0.5f));
    auto rejected = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(rejected->prepare(kSampleRate, 0) ==
            SignalGraph::PreparedTopologyEdit::Result::PreflightFailed);
    CHECK(render(fixture.graph, silence, 1)[0] == Approx(0.75f));
}

TEST_CASE("Clearing a committed sample region removes its authoring and prepared runtime state",
          "[sample-region][runtime][clear]") {
    AllpassFixture fixture;
    REQUIRE(fixture.graph.sample_region_parameter_binding() == &fixture.parameters->binding());
    REQUIRE(fixture.graph.sample_region_runtime_receipts().size() == 1);

    fixture.graph.clear();
    CHECK(fixture.graph.nodes().empty());
    CHECK(fixture.graph.connections().empty());
    CHECK(fixture.graph.sample_region_parameter_binding() == nullptr);
    CHECK(fixture.graph.sample_region_runtime_receipts().empty());
    CHECK(fixture.graph.sample_region_binding_generation() == 0);

    const auto input = fixture.graph.add_input_node(1);
    const auto output = fixture.graph.add_output_node(1);
    REQUIRE(fixture.graph.connect(input, 0, output, 0));
    REQUIRE(fixture.graph.prepare(kSampleRate, kPreparedMaximum));
}

TEST_CASE("Sample region quotient permits unrelated reinit-free live swaps",
          "[sample-region][runtime][continuity][prepared-swap]") {
    const std::array impulse{1.0f};
    const std::array silence{0.0f};
    AllpassFixture fixture;
    CHECK(render(fixture.graph, impulse, 1)[0] == Approx(0.5f));

    fixture.graph.begin_swap_edit();
    REQUIRE(fixture.graph.set_node_gain(fixture.outer_gain, 0.75f));
    REQUIRE(fixture.graph.prepare_swap(kSampleRate, kPreparedMaximum) ==
            SignalGraph::SwapResult::Swapped);

    // The allpass delay identities survive the outer gain edit, so the exact
    // next retained sample is unchanged rather than resetting to silence.
    CHECK(render(fixture.graph, silence, 1)[0] == Approx(0.75f));
}

TEST_CASE("Stale sample region edits preserve snapshot binding generation and state",
          "[sample-region][runtime][comp][continuity]") {
    const std::array impulse{1.0f};
    const std::array silence{0.0f};
    AllpassFixture fixture;
    CHECK(render(fixture.graph, impulse, 1)[0] == Approx(0.5f));

    auto winner = fixture.graph.begin_prepared_topology_edit();
    auto stale = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(winner->set_node_gain(fixture.outer_gain, 1.0f));
    REQUIRE(stale->set_node_gain(fixture.outer_gain, 0.75f));
    REQUIRE(winner->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(stale->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(winner->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto winner_snapshot = winner->committed_execution_snapshot();
    REQUIRE(winner_snapshot);
    CHECK(winner_snapshot.sample_region_binding_generation() > 0);
    REQUIRE(winner_snapshot.sample_region_runtime_receipts().size() == 1);
    REQUIRE(stale->commit() == SignalGraph::PreparedTopologyEdit::Result::StaleBase);
    CHECK_FALSE(stale->committed_execution_snapshot());

    CHECK(fixture.graph.node_gain(fixture.outer_gain) == 1.0f);
    CHECK(fixture.graph.sample_region_parameter_binding() == &fixture.parameters->binding());
    CHECK(fixture.graph.sample_region_binding_generation() ==
          winner_snapshot.sample_region_binding_generation());
    CHECK(render(fixture.graph, silence, 1)[0] == Approx(0.75f));
}

TEST_CASE("Old sample region execution snapshots fail closed after newer generation admission",
          "[sample-region][runtime][comp][continuity][negative]") {
    const std::array impulse{1.0f};
    const std::array silence{0.0f};
    AllpassFixture fixture;
    CHECK(render(fixture.graph, impulse, 1)[0] == Approx(0.5f));

    auto old_edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(old_edit->set_node_gain(fixture.outer_gain, 1.0f));
    REQUIRE(old_edit->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(old_edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto old_snapshot = old_edit->committed_execution_snapshot();
    REQUIRE(old_snapshot);
    const auto old_generation = old_snapshot.sample_region_binding_generation();
    REQUIRE(old_generation > 0);

    auto new_edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(new_edit->set_node_gain(fixture.outer_gain, 1.0f));
    REQUIRE(new_edit->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(new_edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto new_snapshot = new_edit->committed_execution_snapshot();
    REQUIRE(new_snapshot);
    const auto new_generation = new_snapshot.sample_region_binding_generation();
    REQUIRE(new_generation > old_generation);

    CHECK(render(fixture.graph, silence, 1)[0] == Approx(0.75f));

    std::array<float, 4> old_output{123.0f, 456.0f, 789.0f, 321.0f};
    const float* input_channels[] = {silence.data()};
    float* output_channels[] = {old_output.data()};
    audio::BufferView<const float> in(input_channels, 1, silence.size());
    audio::BufferView<float> out(output_channels, 1, old_output.size());
    const auto failures_before = fixture.graph.routed_only_execution_failures();
    old_snapshot.process(out, in, 1);
    CHECK(old_output[0] == 0.0f);
    CHECK(old_output[1] == 456.0f);
    CHECK(old_output[2] == 789.0f);
    CHECK(old_output[3] == 321.0f);
    CHECK(fixture.graph.routed_only_execution_failures() == failures_before + 1);
}

TEST_CASE("Sample region remains exterior while global anticipation is enabled",
          "[sample-region][runtime][anticipation][parity]") {
    constexpr int kBlocks = 12;
    constexpr int kFrames = 64;
    const std::array<float, kFrames> silence{};

    AllpassFixture live(0.5f, kSampleRate, true, false, false, false, kFrames);
    auto* live_source = attach_counting_source(live, false, kFrames);
    std::vector<float> expected;
    for (int block = 0; block < kBlocks; ++block) {
        const auto output = render(live.graph, silence, kFrames);
        expected.insert(expected.end(), output.begin(), output.end());
    }
    CHECK(live_source->blocks() == kBlocks);

    AllpassFixture anticipated(0.5f, kSampleRate, true, false, false, false, kFrames);
    auto* anticipated_source = attach_counting_source(anticipated, true, kFrames);
    std::vector<float> actual;
    std::uint64_t produced = 0;
    for (int block = 0; block < kBlocks; ++block) {
        produced += anticipated.graph.pump_anticipation(8);
        const auto output = render(anticipated.graph, silence, kFrames);
        actual.insert(actual.end(), output.begin(), output.end());
    }

    CHECK(produced >= kBlocks);
    CHECK(anticipated_source->blocks() >= kBlocks);
    CHECK(anticipated_source->blocks() <= kBlocks + 4);
    // Bit identity proves the source ran ahead while the stateful region anchor
    // ran exactly once on the live path; producer execution would advance the
    // allpass delays a second time and diverge.
    require_exact(actual, expected);
}

TEST_CASE("UnitDelay inside a sample region remains zero-PDC direct feedthrough",
          "[sample-region][runtime][pdc]") {
    AllpassFixture fixture(0.5f, kSampleRate, true, false, false, true);
    CHECK(fixture.graph.latency_samples() == 0);
    CHECK(fixture.graph.node_latency_samples(fixture.region_input) == 0);
    CHECK(fixture.graph.node_latency_samples(fixture.region_output) == 0);
    CHECK(fixture.graph.prepared_stats().delay_buffer_bytes == 0);

    const std::array<float, 4> impulse{1.0f, 0.0f, 0.0f, 0.0f};
    const auto output = render(fixture.graph, impulse, 4);
    // Dry path is present at frame zero and the allpass direct term is 0.5.
    CHECK(output[0] == Approx(1.5f));
    CHECK(output[1] == Approx(0.75f));

    SignalGraph delayed;
    const auto graph_input = delayed.add_input_node(1, "Input");
    const auto graph_output = delayed.add_output_node(1, "Output");
    REQUIRE(delayed.connect(graph_input, 0, graph_output, 0));
    REQUIRE(delayed.prepare(kSampleRate, kPreparedMaximum));

    auto edit = delayed.begin_prepared_topology_edit();
    REQUIRE(edit->disconnect(graph_input, 0, graph_output, 0));
    REQUIRE(register_builtin_sample_region_types(*edit));
    const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto unit_delay = edit->add_custom_node("pulp.core.unit-delay");
    const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");
    REQUIRE(edit->connect(graph_input, 0, region_input, 0));
    REQUIRE(edit->connect(region_input, 0, unit_delay, 0));
    REQUIRE(edit->connect(unit_delay, 0, region_output, 0));
    REQUIRE(edit->connect(region_output, 0, graph_output, 0));

    SampleRegionDefinition definition;
    definition.region_id = kRegionId + 1;
    definition.members = {
        {region_input, "pulp.core.sample-region.input", 1, boundary()},
        {unit_delay, "pulp.core.unit-delay", 1, none()},
        {region_output, "pulp.core.sample-region.output", 1, boundary()},
    };
    definition.input_boundaries = {region_input};
    definition.output_boundaries = {region_output};
    REQUIRE(edit->declare_sample_region(definition).accepted);
    REQUIRE(edit->prove_sample_region(definition.region_id).accepted);
    auto delay_parameters =
        SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(delay_parameters);
    REQUIRE(edit->bind_sample_region_parameters(delay_parameters->binding()).accepted);
    edit->set_canonical_executor_routing_enabled(true);
    REQUIRE(edit->prepare(kSampleRate, kPreparedMaximum) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    CHECK(delayed.latency_samples() == 0);
    CHECK(delayed.node_latency_samples(region_input) == 0);
    CHECK(delayed.node_latency_samples(unit_delay) == 0);
    CHECK(delayed.node_latency_samples(region_output) == 0);
    CHECK(delayed.prepared_stats().delay_buffer_bytes == 0);
    const auto shifted = render(delayed, impulse, 4);
    CHECK(shifted[0] == 0.0f);
    CHECK(shifted[1] == 1.0f);
    CHECK(shifted[2] == 0.0f);
}

TEST_CASE("Sample region ordinary and reset callbacks are allocation free",
          "[sample-region][runtime][rt-safety][reset]") {
    AllpassFixture fixture;
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input[0] = 1.0f;
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    audio::BufferView<const float> in(input_channels, 1, input.size());
    audio::BufferView<float> out(output_channels, 1, output.size());

    std::size_t allocations = 0;
    std::size_t bytes = 0;
    {
        test::ScopedRtProcessProbe probe;
        fixture.graph.process(out, in, static_cast<int>(input.size()));
        std::fill(input.begin(), input.end(), 0.0f);
        std::fill(output.begin(), output.end(), -1.0f);
        format::ProcessContext reset;
        reset.reset_requested = true;
        fixture.graph.process(out, in, static_cast<int>(input.size()), reset);
        allocations = probe.allocation_count();
        bytes = probe.allocated_bytes();
    }
    CHECK(allocations == 0);
    CHECK(bytes == 0);
    CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return sample == 0.0f; }));
}
