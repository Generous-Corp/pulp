// S4 proof: authored ProcessorNode instances must be reachable through the
// same Timeline automation and graph modulation routes as hosted plugins.

#include "support/timeline_graph_binding_test_support.hpp"

#include "allpass_processor.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace {

using pulp::format::BusDirection;
using pulp::format::BusRole;
using pulp::format::PluginCategory;
using pulp::format::PluginDescriptor;
using pulp::format::PrepareContext;
using pulp::format::ProcessBlock;
using pulp::format::ProcessContext;
using pulp::format::Processor;
using pulp::host::NodeId;
using pulp::host::SignalGraph;
using pulp::host::TimelineDeviceGraphRoute;
using pulp::host::TimelineGraphPlaybackBinding;
using pulp::host::TimelineTrackGraphRoute;
using pulp::state::ParamInfo;
using pulp::state::ParamRate;
using pulp::state::StateStore;

constexpr pulp::state::ParamID kSequencerParam = 2901;
constexpr float kAllpassCoefficient = 0.5f;

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

pulp::timeline::Project modulation_route_project() {
    pulp::timeline::TrackInput track_input;
    track_input.id = {3};
    track_input.name = "sample-region route";
    track_input.device_chain = {pulp::timeline::DevicePlacement{
        {20},
        pulp::timeline::DeviceConfiguration{.device_kind = pulp::timeline::DeviceKind::BuiltIn,
                                            .binding_key = "com.pulp.sample-region-allpass"}}};
    track_input.modulators = {
        pulp::timeline::Modulator{{10}, pulp::timeline::ModulatorKind::Lfo, "coefficient lfo"}};
    track_input.macros = {pulp::timeline::MacroControl{{11}, "coefficient macro", 0.5f}};
    auto track = take(pulp::timeline::Track::create(std::move(track_input)));
    auto sequence = take(pulp::timeline::Sequence::create(
        {2}, "root", std::nullopt, std::nullopt, std::vector<pulp::timeline::Track>{track}));
    return take(pulp::timeline::Project::create(
        pulp::timeline::ProjectInput{{1}, "sample-region routes", 1'000, {2}, {}, {sequence}}));
}

pulp::timeline::Transaction one_timeline_command(pulp::timeline::Command command) {
    pulp::timeline::Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, std::move(command)});
    return transaction;
}

pulp::timeline::ModulationRoute coefficient_route(pulp::timeline::ItemId id,
                                                  pulp::timeline::ItemId source,
                                                  pulp::timeline::ModulationSourceKind source_kind,
                                                  float depth, bool enabled) {
    return {id,
            {source, source_kind},
            pulp::timeline::DeviceParameterTarget{{20}, kSequencerParam},
            depth,
            enabled};
}

std::vector<float> allpass_oracle(std::span<const float> input, float coefficient) {
    std::vector<float> output(input.size());
    float previous_input = 0.0f;
    float previous_output = 0.0f;
    for (std::size_t index = 0; index < input.size(); ++index) {
        output[index] = coefficient * input[index] + previous_input - coefficient * previous_output;
        previous_input = input[index];
        previous_output = output[index];
    }
    return output;
}

std::vector<float> piecewise_allpass_oracle(std::span<const float> input,
                                            std::span<const float> coefficients,
                                            float fallback_coefficient) {
    std::vector<float> output(input.size());
    float previous_input = 0.0f;
    float previous_output = 0.0f;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const float coefficient =
            index < coefficients.size() ? coefficients[index] : fallback_coefficient;
        output[index] = coefficient * input[index] + previous_input - coefficient * previous_output;
        previous_input = input[index];
        previous_output = output[index];
    }
    return output;
}

std::vector<float> render_allpass(const std::vector<std::size_t>& partitions,
                                  std::span<const float> input) {
    auto processor = std::make_unique<pulp::examples::SampleRegionAllpassProcessor>();
    REQUIRE(processor->error().empty());
    StateStore state;
    processor->define_parameters(state);
    REQUIRE(processor->error().empty());
    PrepareContext prepare;
    prepare.sample_rate = 48'000.0;
    prepare.max_buffer_size = 64;
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    processor->prepare(prepare);
    REQUIRE(processor->error().empty());

    std::vector<float> rendered(input.size(), 0.0f);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    std::size_t offset = 0;
    for (const auto frames : partitions) {
        REQUIRE(frames > 0);
        REQUIRE(offset + frames <= input.size());
        Buffer in(1, frames);
        Buffer out(1, frames);
        std::copy_n(input.data() + offset, frames, in.storage[0].data());
        auto output_view = out.view();
        auto input_view = in.const_view();
        ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = static_cast<int>(frames);
        processor->process(output_view, input_view, midi_in, midi_out, context);
        std::copy_n(out.storage[0].data(), frames, rendered.data() + offset);
        offset += frames;
    }
    REQUIRE(offset == input.size());
    return rendered;
}

class ModulatedAllpassProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "S4 Modulated Allpass Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.s4-modulated-allpass";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(StateStore& store) override {
        ParamInfo p;
        p.id = kSequencerParam;
        p.name = "Allpass Coefficient";
        p.range = {-0.99f, 0.99f, kAllpassCoefficient};
        p.rate = ParamRate::AudioRate;
        store.add_parameter(p);
    }

    void prepare(const PrepareContext&) override {
        previous_input = 0.0f;
        previous_output = 0.0f;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        if (output.num_channels() == 0)
            return;
        const auto* source = input.num_channels() == 0 ? nullptr : input.channel_ptr(0);
        auto* destination = output.channel_ptr(0);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            const float sample = source == nullptr ? 0.0f : source[frame];
            destination[frame] = kAllpassCoefficient * sample + previous_input -
                                 kAllpassCoefficient * previous_output;
            previous_input = sample;
            previous_output = destination[frame];
        }
    }

    bool process_block(ProcessBlock& block) override {
        const auto modulation = block.events == nullptr ? std::span<const float>{} : [&] {
            for (const auto& lane : block.events->audio_rate_modulations) {
                if (lane.param_id == kSequencerParam)
                    return lane.values;
            }
            return std::span<const float>{};
        }();
        auto* input = block.buses == nullptr
                          ? nullptr
                          : block.buses->first(BusDirection::Input, BusRole::Main);
        auto* output = block.buses == nullptr
                           ? nullptr
                           : block.buses->first(BusDirection::Output, BusRole::Main);
        if (output == nullptr)
            return false;
        const auto* source = input == nullptr ? nullptr : input->input.channel_ptr(0);
        auto* destination = output->output.channel_ptr(0);
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame) {
            const float sample = source == nullptr ? 0.0f : source[frame];
            const float coefficient =
                frame < modulation.size() ? modulation[frame] : kAllpassCoefficient;
            destination[frame] =
                coefficient * sample + previous_input - coefficient * previous_output;
            previous_input = sample;
            previous_output = destination[frame];
        }
        return true;
    }

  private:
    float previous_input = 0.0f;
    float previous_output = 0.0f;
};

std::vector<float> render_piecewise_allpass(const std::vector<std::size_t>& partitions,
                                            std::span<const float> input,
                                            std::span<const float> coefficients,
                                            bool modulation_connected) {
    if (modulation_connected)
        REQUIRE(coefficients.size() >= input.size());
    SignalGraph graph;
    const auto input_node = graph.add_input_node(2);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<ModulatedAllpassProcessor>();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    if (modulation_connected)
        REQUIRE(
            graph.connect_audio_rate_modulation(input_node, 1, node, kSequencerParam, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48'000.0, 64));

    std::vector<float> rendered(input.size(), 0.0f);
    std::size_t offset = 0;
    for (const auto frames : partitions) {
        REQUIRE(frames > 0);
        REQUIRE(offset + frames <= input.size());
        Buffer in(2, frames);
        Buffer out(1, frames);
        std::copy_n(input.data() + offset, frames, in.storage[0].data());
        if (modulation_connected) {
            std::copy_n(coefficients.data() + offset, frames, in.storage[1].data());
        } else {
            std::fill(in.storage[1].begin(), in.storage[1].end(), 0.0f);
        }
        auto output_view = out.view();
        const auto input_view = in.const_view();
        graph.process(output_view, input_view, frames);
        std::copy_n(out.storage[0].data(), frames, rendered.data() + offset);
        offset += frames;
    }
    REQUIRE(offset == input.size());
    return rendered;
}

void require_close_to(std::span<const float> actual, std::span<const float> expected,
                      float margin = 1.0e-6f) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index)
        CHECK(actual[index] == Catch::Approx(expected[index]).margin(margin));
}

class SequencerProbeProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "S4 Sequencer Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.s4-sequencer-probe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(StateStore& store) override {
        ParamInfo p;
        p.id = kSequencerParam;
        p.name = "Sequencer Amount";
        p.range = {0.0f, 1.0f, 0.25f};
        p.rate = ParamRate::AudioRate;
        store.add_parameter(p);
    }

    void prepare(const PrepareContext&) override {
        sparse_events.clear();
        dense_values.clear();
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        if (output.num_channels() == 0)
            return;
        const auto* source = input.num_channels() == 0 ? nullptr : input.channel_ptr(0);
        auto* destination = output.channel_ptr(0);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            destination[frame] = source == nullptr ? 0.0f : source[frame];
    }

    bool process_block(ProcessBlock& block) override {
        if (block.events != nullptr) {
            for (const auto& event : block.events->parameters())
                sparse_events.push_back(event);
            if (!block.events->audio_rate_modulations.empty()) {
                const auto& lane = block.events->audio_rate_modulations.front();
                dense_values.assign(lane.values.begin(), lane.values.end());
            }
        }
        auto* input = block.buses == nullptr
                          ? nullptr
                          : block.buses->first(BusDirection::Input, BusRole::Main);
        auto* output = block.buses == nullptr
                           ? nullptr
                           : block.buses->first(BusDirection::Output, BusRole::Main);
        if (output == nullptr)
            return false;
        const auto* source = input == nullptr ? nullptr : input->input.channel_ptr(0);
        auto* destination = output->output.channel_ptr(0);
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame)
            destination[frame] = source == nullptr ? 0.0f : source[frame];
        return true;
    }

    std::vector<pulp::state::ParameterEvent> sparse_events;
    std::vector<float> dense_values;
};

class LifecycleProbeProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "S4 Lifecycle Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.s4-lifecycle-probe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        return d;
    }

    void define_parameters(StateStore&) override {}

    void prepare(const PrepareContext&) override {
        previous = 0.0f;
        observations.clear();
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext& context) override {
        if (context.reset_requested || context.transport_jump)
            previous = 0.0f;
        observations.push_back({context.reset_requested, context.transport_jump, context.is_playing,
                                context.transport_started,
                                context.process_mode == pulp::format::ProcessMode::Offline});
        if (output.num_channels() == 0)
            return;
        const auto* source = input.num_channels() == 0 ? nullptr : input.channel_ptr(0);
        auto* destination = output.channel_ptr(0);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            const float sample = source == nullptr ? 0.0f : source[frame];
            destination[frame] = sample + previous;
            previous = destination[frame];
        }
    }

    struct Observation {
        bool reset_requested = false;
        bool transport_jump = false;
        bool is_playing = false;
        bool transport_started = false;
        bool offline = false;
    };

    float previous = 0.0f;
    std::vector<Observation> observations;
};

} // namespace

TEST_CASE("S4 installed allpass arrangement follows an independent oracle") {
    const std::vector<float> input{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const auto expected = allpass_oracle(input, kAllpassCoefficient);
    const auto one_block = render_allpass({input.size()}, input);
    const auto split_blocks = render_allpass({1, 3, 2, 2}, input);

    REQUIRE(one_block.size() == expected.size());
    REQUIRE(split_blocks.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(one_block[index] == Catch::Approx(expected[index]).margin(1.0e-6f));
        CHECK(split_blocks[index] == Catch::Approx(expected[index]).margin(1.0e-6f));
        CHECK(split_blocks[index] == Catch::Approx(one_block[index]).margin(1.0e-6f));
    }
    CHECK(one_block[0] == Catch::Approx(0.5f));
    CHECK(one_block[1] == Catch::Approx(0.75f));
    CHECK(one_block[2] == Catch::Approx(-0.375f));
}

TEST_CASE("S4 typed modulation route lifecycle gates the promoted coefficient") {
    const auto project = modulation_route_project();
    const auto inserted_route =
        coefficient_route({1000}, {11}, pulp::timeline::ModulationSourceKind::Macro, 0.25f, true);
    auto inserted_result = pulp::timeline::reduce_transaction(
        project,
        one_timeline_command(pulp::timeline::InsertModulationRoute{{2}, {3}, inserted_route}));
    REQUIRE(inserted_result);
    const auto inserted = std::move(inserted_result).value();
    const auto* inserted_track = inserted.project.find_sequence({2})->find_track({3});
    REQUIRE(inserted_track != nullptr);
    REQUIRE(inserted_track->modulation_routes().size() == 1);
    CHECK(*inserted_track->find_modulation_route({1000}) == inserted_route);

    const auto replacement = coefficient_route(
        {1000}, {10}, pulp::timeline::ModulationSourceKind::Modulator, -0.5f, false);
    const auto changed = take(pulp::timeline::reduce_transaction(
        inserted.project, one_timeline_command(pulp::timeline::SetModulationRoute{
                              {2}, {3}, {1000}, inserted_route, replacement})));
    const auto* changed_track = changed.project.find_sequence({2})->find_track({3});
    REQUIRE(changed_track != nullptr);
    CHECK(*changed_track->find_modulation_route({1000}) == replacement);
    REQUIRE(changed.inverses.size() == 1);
    const auto inverse = std::get<pulp::timeline::SetModulationRoute>(changed.inverses[0]);
    CHECK(inverse.expected == replacement);
    CHECK(inverse.replacement == inserted_route);

    const auto removed = take(pulp::timeline::reduce_transaction(
        changed.project,
        one_timeline_command(pulp::timeline::RemoveModulationRoute{{2}, {3}, {1000}})));
    const auto* removed_track = removed.project.find_sequence({2})->find_track({3});
    REQUIRE(removed_track != nullptr);
    CHECK(removed_track->modulation_routes().empty());
    REQUIRE(removed.inverses.size() == 1);
    const auto remove_inverse =
        std::get<pulp::timeline::InsertModulationRoute>(removed.inverses[0]);
    CHECK(remove_inverse.route == replacement);

    const auto invalid_source =
        coefficient_route({1001}, {99}, pulp::timeline::ModulationSourceKind::Macro, 0.5f, true);
    const auto refused = pulp::timeline::reduce_transaction(
        project,
        one_timeline_command(pulp::timeline::InsertModulationRoute{{2}, {3}, invalid_source}));
    REQUIRE_FALSE(refused);
    const auto* unchanged_track = project.find_sequence({2})->find_track({3});
    REQUIRE(unchanged_track != nullptr);
    CHECK(unchanged_track->modulation_routes().empty());
}

TEST_CASE(
    "S4 audio-rate modulation follows a piecewise coefficient oracle and removal restores base") {
    const std::vector<float> input{1.0f, 0.25f, -0.5f, 0.75f, 0.0f, -0.25f, 0.5f, 0.125f};
    const std::vector<float> coefficients{0.25f, 0.25f, 0.5f, 0.5f, 0.9f, 0.9f, 0.2f, 0.2f};
    const auto expected_modulated =
        piecewise_allpass_oracle(input, coefficients, kAllpassCoefficient);
    const auto one_block = render_piecewise_allpass({input.size()}, input, coefficients, true);
    const auto split_blocks = render_piecewise_allpass({2, 1, 3, 2}, input, coefficients, true);
    require_close_to(one_block, expected_modulated);
    require_close_to(split_blocks, expected_modulated);
    require_close_to(split_blocks, one_block);

    const auto expected_unmodulated = allpass_oracle(input, kAllpassCoefficient);
    const auto disconnected = render_piecewise_allpass({2, 1, 3, 2}, input, coefficients, false);
    require_close_to(disconnected, expected_unmodulated);
    CHECK(disconnected != expected_modulated);
}

TEST_CASE("S4 reloaded route projects through the existing timeline and graph authorities") {
    const auto project = modulation_route_project();
    const auto route =
        coefficient_route({1000}, {11}, pulp::timeline::ModulationSourceKind::Macro, 0.25f, true);
    auto authored_result = pulp::timeline::reduce_transaction(
        project, one_timeline_command(pulp::timeline::InsertModulationRoute{{2}, {3}, route}));
    REQUIRE(authored_result);
    const auto authored = std::move(authored_result).value();

    const auto registry = take(pulp::timeline::make_builtin_timeline_registry());
    const auto encoded = take(pulp::timeline::serialize_project(authored.project, registry));
    const auto decoded = take(pulp::timeline::deserialize_project(encoded.json, registry));
    const auto reencoded = take(pulp::timeline::serialize_project(decoded, registry));
    REQUIRE(reencoded.json == encoded.json);

    const auto* reloaded_track = decoded.find_sequence({2})->find_track({3});
    REQUIRE(reloaded_track != nullptr);
    REQUIRE(reloaded_track->modulation_routes().size() == 1);
    const auto* reloaded_route = reloaded_track->find_modulation_route({1000});
    REQUIRE(reloaded_route != nullptr);
    CHECK(*reloaded_route == route);
    REQUIRE(std::holds_alternative<pulp::timeline::DeviceParameterTarget>(reloaded_route->target));
    CHECK(std::get<pulp::timeline::DeviceParameterTarget>(reloaded_route->target)
              .device_placement_id == pulp::timeline::ItemId{20});
    CHECK(std::get<pulp::timeline::DeviceParameterTarget>(reloaded_route->target).param_id ==
          kSequencerParam);

    auto original = std::make_unique<pulp::examples::SampleRegionAllpassProcessor>();
    REQUIRE(original->error().empty());
    StateStore original_state;
    original->define_parameters(original_state);
    PrepareContext original_prepare;
    original_prepare.sample_rate = 48'000.0;
    original_prepare.max_buffer_size = 64;
    original_prepare.input_channels = 1;
    original_prepare.output_channels = 1;
    original->prepare(original_prepare);
    REQUIRE(original->error().empty());
    const auto graph_json = pulp::host::GraphSerializer::to_json(original->graph());
    REQUIRE_FALSE(graph_json.empty());
    pulp::examples::SampleRegionAllpassProcessor reloaded_processor(graph_json);
    REQUIRE(reloaded_processor.error().empty());
    const auto original_parameters = original->parameter_contract().parameters();
    const auto reloaded_parameters = reloaded_processor.parameter_contract().parameters();
    REQUIRE(reloaded_parameters.size() == original_parameters.size());
    for (std::size_t index = 0; index < original_parameters.size(); ++index)
        CHECK(reloaded_parameters[index].id == original_parameters[index].id);
    CHECK(reloaded_processor.parameter_contract().frozen());
}

TEST_CASE("S4 authored ProcessorNode receives the complete lifecycle matrix") {
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<LifecycleProbeProcessor>();
    auto* observed = processor.get();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 8));

    Buffer input(1, 2, 1.0f);
    Buffer output(1, 2);
    auto render = [&](pulp::format::ProcessContext context) {
        context.sample_rate = 48'000.0;
        context.num_samples = 2;
        auto output_view = output.view();
        const auto input_view = input.const_view();
        graph.process(output_view, input_view, 2, context);
        return output.storage[0];
    };

    auto first = render({.reset_requested = true, .is_playing = true, .transport_started = true});
    CHECK(first[0] == Catch::Approx(1.0f));
    CHECK(first[1] == Catch::Approx(2.0f));

    auto contiguous = render({.is_playing = true});
    CHECK(contiguous[0] == Catch::Approx(3.0f));
    CHECK(contiguous[1] == Catch::Approx(4.0f));

    auto seek = render({.reset_requested = true, .transport_jump = true, .is_playing = true});
    CHECK(seek[0] == Catch::Approx(1.0f));
    CHECK(seek[1] == Catch::Approx(2.0f));

    auto stopped = render({});
    CHECK(stopped[0] == Catch::Approx(3.0f));
    CHECK(stopped[1] == Catch::Approx(4.0f));

    auto restarted = render({.is_playing = true, .transport_started = true});
    CHECK(restarted[0] == Catch::Approx(5.0f));
    CHECK(restarted[1] == Catch::Approx(6.0f));

    auto offline = render({.process_mode = pulp::format::ProcessMode::Offline, .is_playing = true});
    CHECK(offline[0] == Catch::Approx(7.0f));
    CHECK(offline[1] == Catch::Approx(8.0f));

    REQUIRE(observed->observations.size() == 6);
    CHECK(observed->observations[0].reset_requested);
    CHECK_FALSE(observed->observations[0].transport_jump);
    CHECK(observed->observations[0].is_playing);
    CHECK(observed->observations[0].transport_started);
    CHECK_FALSE(observed->observations[0].offline);
    CHECK_FALSE(observed->observations[1].reset_requested);
    CHECK_FALSE(observed->observations[1].transport_jump);
    CHECK(observed->observations[2].reset_requested);
    CHECK(observed->observations[2].transport_jump);
    CHECK_FALSE(observed->observations[3].is_playing);
    CHECK(observed->observations[4].transport_started);
    CHECK(observed->observations[5].offline);
}

TEST_CASE("S4 prepared topology adoption retains the authored allpass instance state") {
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<pulp::examples::SampleRegionAllpassProcessor>();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 8));

    Buffer input(1, 2, 1.0f);
    Buffer output(1, 2);
    pulp::format::ProcessContext first_context;
    first_context.sample_rate = 48'000.0;
    first_context.num_samples = 2;
    first_context.reset_requested = true;
    auto output_view = output.view();
    const auto input_view = input.const_view();
    graph.process(output_view, input_view, 2, first_context);

    // Keep a same-generation control graph.  The independent oracle above
    // proves the DSP recurrence; this paired graph proves the stronger
    // adoption property without assuming how the executor partitions its
    // retained cells internally.
    SignalGraph control;
    const auto control_input_node = control.add_input_node(1);
    const auto control_output_node = control.add_output_node(1);
    auto control_processor = std::make_unique<pulp::examples::SampleRegionAllpassProcessor>();
    const auto control_node = control.add_processor_node(std::move(control_processor));
    REQUIRE(control_node != 0);
    REQUIRE(control.connect(control_input_node, 0, control_node, 0));
    REQUIRE(control.connect(control_node, 0, control_output_node, 0));
    REQUIRE(control.prepare(48'000.0, 8));
    Buffer control_first(1, 2);
    auto control_first_view = control_first.view();
    control.process(control_first_view, input_view, 2, first_context);
    CHECK(output.storage == control_first.storage);
    auto control_edit = control.begin_prepared_topology_edit();
    REQUIRE(control_edit);
    REQUIRE(control_edit->prepare(48'000.0, 8) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(control_edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    Buffer control_second(1, 2);
    auto control_second_view = control_second.view();
    pulp::format::ProcessContext second_context;
    second_context.sample_rate = 48'000.0;
    second_context.num_samples = 2;
    control.process(control_second_view, input_view, 2, second_context);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto gain = edit->add_gain_node("retained-state-adoption-gain");
    REQUIRE(gain != 0);
    REQUIRE(edit->disconnect(node, 0, output_node, 0));
    REQUIRE(edit->connect(node, 0, gain, 0));
    REQUIRE(edit->connect(gain, 0, output_node, 0));
    REQUIRE(edit->prepare(48'000.0, 8) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    std::fill(output.storage[0].begin(), output.storage[0].end(), 0.0f);
    output_view = output.view();
    graph.process(output_view, input_view, 2, second_context);
    CHECK(output.storage == control_second.storage);
}

TEST_CASE("S4 authored ProcessorNode replacement is refused with the typed diagnostic") {
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<LifecycleProbeProcessor>();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 8));

    pulp::host::PluginInfo replacement;
    replacement.name = "S4 replacement control";
    replacement.manufacturer = "Pulp";
    replacement.version = "1.0.0";
    replacement.unique_id = "com.pulp.test.s4-replacement-control";
    replacement.format = pulp::host::PluginFormat::CLAP;
    replacement.num_inputs = 1;
    replacement.num_outputs = 1;
    const auto token = graph.register_scanned_plugin(replacement);
    REQUIRE(static_cast<bool>(token));

    graph.begin_swap_edit();
    CHECK(graph.stage_plugin_replacement(node, token) ==
          SignalGraph::SwapResult::NeedsEagerPrepare);
    const auto diagnostics = graph.last_swap_diagnostics();
    CHECK(diagnostics.reason == pulp::host::LiveSwapFallbackReason::PredicateExcluded);
    CHECK(diagnostics.offending_node == node);
    CHECK(diagnostics.message == "node is not a prepared plugin node");
}

TEST_CASE("S4 timeline automation reaches an authored ProcessorNode") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map, 0.25f, 0.75f, kSequencerParam), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    const auto program = programs.store.read();
    REQUIRE(program);

    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    auto processor = std::make_unique<SequencerProbeProcessor>();
    auto* observed = processor.get();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.is_processor_node(node));
    REQUIRE(graph.connect(node, 0, output, 0));
    REQUIRE(graph.prepare(48'000.0, 64));

    const std::array devices{TimelineDeviceGraphRoute{{20}, node}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.prepare_quiesced(*program, routes, config(1), 48'000.0, 64));

    Buffer input(1, 32, 1.0f);
    Buffer rendered(1, 32);
    auto input_view = input.const_view();
    auto rendered_view = rendered.view();
    auto result = binding.process(rendered_view, input_view, snapshot(*program, 32));
    REQUIRE(result);
    REQUIRE(result.emitted_automation_events > 0);
    REQUIRE_FALSE(observed->sparse_events.empty());
    REQUIRE(std::all_of(observed->sparse_events.begin(), observed->sparse_events.end(),
                        [](const auto& event) { return event.param_id == kSequencerParam; }));
    REQUIRE(std::any_of(observed->sparse_events.begin(), observed->sparse_events.end(),
                        [](const auto& event) {
                            return event.value == 0.75f && event.ramp_duration_sample_frames > 0;
                        }));
}

TEST_CASE("S4 graph modulation reaches the same authored ProcessorNode parameter") {
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<SequencerProbeProcessor>();
    auto* observed = processor.get();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    REQUIRE(graph.connect_audio_rate_modulation(input_node, 0, node, kSequencerParam, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48'000.0, 32));

    Buffer input(1, 32, 0.5f);
    Buffer output(1, 32);
    auto input_view = input.const_view();
    auto output_view = output.view();
    graph.process(output_view, input_view, 32);
    REQUIRE(observed->dense_values.size() == 32);
    REQUIRE(std::all_of(observed->dense_values.begin(), observed->dense_values.end(),
                        [](float value) { return value == 0.5f; }));
}

TEST_CASE("S4 exposure row and example retain the existing authority boundaries") {
    const auto source_root = std::filesystem::path(PULP_SOURCE_DIR);
    const auto row = read_text(source_root / "docs/status/sequencer-exposure/rows/"
                                             "sample-region-sequencer-interoperability.json");
    const auto example = read_text(source_root / "examples/sample-region-allpass/README.md");
    const auto automation =
        read_text(source_root / "core/host/src/timeline_automation_delivery.cpp");
    const auto graph = read_text(source_root / "core/host/src/signal_graph.cpp");
    REQUIRE_FALSE(row.empty());
    REQUIRE_FALSE(example.empty());
    REQUIRE_FALSE(automation.empty());
    REQUIRE_FALSE(graph.empty());

    CHECK(row.find("S4 timeline automation reaches an authored ProcessorNode") !=
          std::string::npos);
    CHECK(row.find("S4 audio-rate modulation follows a piecewise coefficient oracle") !=
          std::string::npos);
    CHECK(row.find(
              "S4 reloaded route projects through the existing timeline and graph authorities") !=
          std::string::npos);
    CHECK(example.find("ordinary host-owned `StateStore`") != std::string::npos);
    CHECK(example.find("parameter 2901") != std::string::npos);
    CHECK(example.find("same Processor rather than") != std::string::npos);
    CHECK(automation.find("validate_timeline_automation_routes") != std::string::npos);
    CHECK(graph.find("connect_audio_rate_modulation") != std::string::npos);
    CHECK(graph.find("serialize_project") == std::string::npos);
    CHECK(graph.find("StateStore") == std::string::npos);
    CHECK(automation.find("serialize_project") == std::string::npos);
    CHECK(automation.find("StateStore") == std::string::npos);
}
