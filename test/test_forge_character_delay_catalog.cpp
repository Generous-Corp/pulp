// CharacterDelay Forge catalog — stable bake-layer contract tests.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_character_delay_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pulp::host;
namespace catalog = pulp::host::character_delay;
using Character = pulp::signal::CharacterDelay::Character;
using TapeTier = pulp::signal::CharacterDelay::TapeTier;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr std::size_t kParamCount = 10;

struct ParamContract {
    pulp::state::ParamID id;
    float minimum;
    float maximum;
    float default_value;
};

constexpr std::array<ParamContract, kParamCount> kParams{{
    {catalog::kTimeMs, 1.0f, 2000.0f, 350.0f},
    {catalog::kTimeOffset, 0.5f, 1.5f, 1.0f},
    {catalog::kFeedback, 0.0f, 1.1f, 0.35f},
    {catalog::kCrossfeed, 0.0f, 1.0f, 0.0f},
    {catalog::kCharacter, 0.0f, 1.0f, 0.5f},
    {catalog::kModRate, 0.0f, 1.0f, 0.3f},
    {catalog::kModDepth, 0.0f, 1.0f, 0.0f},
    {catalog::kDuck, 0.0f, 1.0f, 0.0f},
    {catalog::kFreeze, 0.0f, 1.0f, 0.0f},
    {catalog::kReverse, 0.0f, 1.0f, 0.0f},
}};

struct Realization {
    Character character;
    TapeTier tier;
    const char* type_id;
};

constexpr std::array<Realization, 6> kRealizations{{
    {Character::clean, TapeTier::standard, "delay.clean"},
    {Character::vintage_digital, TapeTier::standard, "delay.vintage"},
    {Character::tape, TapeTier::standard, "delay.tape"},
    {Character::tape, TapeTier::physical, "delay.tape_physical"},
    {Character::bbd, TapeTier::standard, "delay.bbd"},
    {Character::diffusion, TapeTier::standard, "delay.diffusion"},
}};

pulp::state::ParameterEvent immediate(pulp::state::ParamID id, float value) {
    return {id, 0, value, 0};
}

struct StereoBlock {
    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
};

void process_block(pulp::format::Processor& processor, const StereoBlock& input,
                   StereoBlock& output) {
    const float* inputs[2] = {input.left.data(), input.right.data()};
    float* outputs[2] = {output.left.data(), output.right.data()};
    pulp::audio::BufferView<const float> input_view(inputs, 2, kFrames);
    pulp::audio::BufferView<float> output_view(outputs, 2, kFrames);
    pulp::midi::MidiBuffer midi_in;
    pulp::midi::MidiBuffer midi_out;
    pulp::format::ProcessContext context;
    context.sample_rate = kSr;
    context.num_samples = kFrames;
    processor.process(output_view, input_view, midi_in, midi_out, context);
}

struct BakedDelay {
    SignalGraph graph;
    LowerResult result;
    NodeId node = 0;

    explicit BakedDelay(const CustomNodeType& type) {
        REQUIRE(graph.register_custom_node_type(type));
        const auto input = graph.add_input_node(2, "In");
        node = graph.add_custom_node(type.type_id, 1, "Delay");
        const auto output = graph.add_output_node(2, "Out");
        for (PortIndex port = 0; port < 2; ++port) {
            REQUIRE(graph.connect(input, port, node, port));
            REQUIRE(graph.connect(node, port, output, port));
        }
        graph.set_canonical_executor_routing_enabled(true);
        REQUIRE(graph.prepare(kSr, kFrames));
        result = bake(graph);
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        REQUIRE(result.reason == LowerRejectReason::None);

        pulp::format::PrepareContext context;
        context.sample_rate = kSr;
        context.max_buffer_size = kFrames;
        context.input_channels = 2;
        context.output_channels = 2;
        result.processor->prepare(context);
    }

    BakedGraphProcessor& baked() {
        return *static_cast<BakedGraphProcessor*>(result.processor.get());
    }
};

struct ParamObservation {
    std::array<float, kParamCount> values{};
    std::array<bool, kParamCount> seen{};
};

std::size_t param_index(pulp::state::ParamID id) {
    const auto found = std::find_if(kParams.begin(), kParams.end(),
                                    [id](const auto& spec) { return spec.id == id; });
    assert(found != kParams.end());
    return static_cast<std::size_t>(std::distance(kParams.begin(), found));
}

class ObservedParamView final : public BakedParamView {
public:
    ObservedParamView(const BakedParamView& source, ParamObservation& observation)
        : source_(source), observation_(observation) {}

    float value_at(pulp::state::ParamID id, std::int32_t offset) const override {
        const float delivered = source_.value_at(id, offset);
        const auto index = param_index(id);
        if (offset == 0) {
            observation_.values[index] = delivered;
            observation_.seen[index] = true;
        }
        return delivered;
    }

    float value(pulp::state::ParamID id) const override { return source_.value(id); }

private:
    const BakedParamView& source_;
    ParamObservation& observation_;
};

CustomNodeType observed_node(ParamObservation& observation) {
    auto type = catalog::make_character_delay_node(Character::clean);
    const auto production_callback = type.process_instance_baked_param;
    type.process_instance_baked_param =
        [&observation, production_callback](void* instance, pulp::audio::BufferView<float>& out,
                                             const pulp::audio::BufferView<const float>& in,
                                             int frames, const BakedParamView& params) {
            ObservedParamView observed(params, observation);
            production_callback(instance, out, in, frames, observed);
        };
    return type;
}

class FixedParamView final : public BakedParamView {
public:
    FixedParamView() {
        for (std::size_t i = 0; i < kParams.size(); ++i) values_[i] = kParams[i].default_value;
    }

    void set(pulp::state::ParamID id, float value) { values_[index_of(id)] = value; }

    float value_at(pulp::state::ParamID id, std::int32_t) const override {
        seen_[index_of(id)] = true;
        return values_[index_of(id)];
    }

    float value(pulp::state::ParamID id) const override { return values_[index_of(id)]; }

    bool saw_every_param() const {
        return std::all_of(seen_.begin(), seen_.end(), [](bool seen) { return seen; });
    }

private:
    static std::size_t index_of(pulp::state::ParamID id) {
        const auto found = std::find_if(kParams.begin(), kParams.end(),
                                        [id](const auto& spec) { return spec.id == id; });
        assert(found != kParams.end());
        return static_cast<std::size_t>(std::distance(kParams.begin(), found));
    }

    std::array<float, kParamCount> values_{};
    mutable std::array<bool, kParamCount> seen_{};
};

struct DirectDelay {
    CustomNodeType type;
    void* instance = nullptr;

    explicit DirectDelay(const CustomNodeType& node_type) : type(node_type), instance(type.create()) {
        REQUIRE(instance != nullptr);
        type.prepare(instance, kSr, kFrames);
    }

    ~DirectDelay() { type.destroy(instance); }

    void process(const StereoBlock& input, StereoBlock& output, const BakedParamView& params) {
        const float* inputs[2] = {input.left.data(), input.right.data()};
        float* outputs[2] = {output.left.data(), output.right.data()};
        pulp::audio::BufferView<const float> input_view(inputs, 2, kFrames);
        pulp::audio::BufferView<float> output_view(outputs, 2, kFrames);
        type.process_instance_baked_param(instance, output_view, input_view, kFrames, params);
    }
};

std::vector<float> deterministic_render(DirectDelay& delay, const FixedParamView& params) {
    constexpr int kBlocks = 256;
    std::vector<float> rendered;
    rendered.reserve(kBlocks * kFrames * 2);
    for (int block = 0; block < kBlocks; ++block) {
        StereoBlock input;
        StereoBlock output;
        if (block == 0) {
            input.left[0] = 0.7f;
            input.right[0] = -0.35f;
        }
        delay.process(input, output, params);
        rendered.insert(rendered.end(), output.left.begin(), output.left.end());
        rendered.insert(rendered.end(), output.right.begin(), output.right.end());
    }
    return rendered;
}

}  // namespace

TEST_CASE("Forge CharacterDelay identities and baked parameter contract are stable",
          "[forge][character-delay][catalog][contract]") {
    SignalGraph graph;
    std::vector<std::string> ids;
    for (const auto& realization : kRealizations) {
        const auto type = catalog::make_character_delay_node(realization.character,
                                                              realization.tier);
        INFO("type " << realization.type_id);
        CHECK(type.type_id == realization.type_id);
        CHECK(catalog::character_delay_type_id(realization.character, realization.tier) ==
              std::string(realization.type_id));
        CHECK(type.version == 1);
        CHECK(type.num_input_ports == 2);
        CHECK(type.num_output_ports == 2);
        CHECK(type.lowerable);
        CHECK(static_cast<bool>(type.process_instance_baked_param));
        REQUIRE(type.baked_params.size() == kParams.size());
        for (std::size_t i = 0; i < kParams.size(); ++i) {
            CHECK(type.baked_params[i].id == kParams[i].id);
            CHECK(type.baked_params[i].min_value == kParams[i].minimum);
            CHECK(type.baked_params[i].max_value == kParams[i].maximum);
            CHECK(type.baked_params[i].default_value == kParams[i].default_value);
            CHECK(type.baked_params[i].id == static_cast<pulp::state::ParamID>(i + 1));
        }
        CHECK(graph.register_custom_node_type(type));
        ids.emplace_back(type.type_id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Physical tape speed changes construction-time filter geometry. It must
    // not create another runtime parameter or another serialized node identity.
    const auto fast_tape = catalog::make_character_delay_node(
        Character::tape, TapeTier::physical, 15.0f);
    CHECK(fast_tape.type_id == std::string(catalog::kTapePhysicalTypeId));
    CHECK(fast_tape.baked_params.size() == kParamCount);
}

TEST_CASE("Forge CharacterDelay lowers and accepts every runtime control",
          "[forge][character-delay][catalog][injection]") {
    ParamObservation observation;
    BakedDelay fixture(observed_node(observation));
    auto injector = fixture.baked().claim_param_injection(fixture.node);
    REQUIRE(injector.valid());

    pulp::state::ParameterEventQueue all;
    std::array<float, kParamCount> injected{};
    for (std::size_t i = 0; i < kParams.size(); ++i) {
        // A distinct interior value per parameter makes swapped IDs visible.
        const auto& param = kParams[i];
        injected[i] = param.minimum +
                      (param.maximum - param.minimum) * static_cast<float>(i + 1) / 11.0f;
        REQUIRE(all.push(immediate(param.id, injected[i])));
    }
    REQUIRE(injector.inject(all) == InjectStatus::Ok);
    StereoBlock silence;
    StereoBlock scratch;
    process_block(*fixture.result.processor, silence, scratch);
    for (std::size_t i = 0; i < kParams.size(); ++i) {
        INFO("ParamID " << kParams[i].id);
        CHECK(observation.seen[i]);
        CHECK(observation.values[i] == injected[i]);
    }
}

TEST_CASE("Forge CharacterDelay bake produces its default repeat",
          "[forge][character-delay][catalog][lowering]") {
    BakedDelay fixture(catalog::make_character_delay_node(Character::clean));

    std::vector<float> captured;
    for (int block = 0; block < 190; ++block) {
        StereoBlock input;
        StereoBlock output;
        if (block == 0) input.left[0] = 1.0f;
        process_block(*fixture.result.processor, input, output);
        captured.insert(captured.end(), output.left.begin(), output.left.end());
    }
    const auto peak = std::max_element(captured.begin() + 1, captured.end(),
                                       [](float a, float b) { return std::abs(a) < std::abs(b); });
    const auto repeat_sample = std::distance(captured.begin(), peak);
    INFO("repeat sample " << repeat_sample);
    CHECK(std::abs(repeat_sample - 16800) <= 96);
}

TEST_CASE("Forge CharacterDelay is deterministic after reset",
          "[forge][character-delay][catalog][determinism]") {
    DirectDelay delay(catalog::make_character_delay_node(Character::tape, TapeTier::physical));
    FixedParamView params;
    params.set(catalog::kTimeMs, 20.0f);
    params.set(catalog::kFeedback, 0.65f);
    params.set(catalog::kCharacter, 1.0f);
    params.set(catalog::kModDepth, 0.7f);
    // Let the callback publish the chosen targets once. reset() then snaps the
    // smoothers to those targets, giving both measured runs identical starts.
    StereoBlock warmup_input;
    StereoBlock warmup_output;
    delay.process(warmup_input, warmup_output, params);
    delay.type.reset(delay.instance);
    const auto first = deterministic_render(delay, params);
    delay.type.reset(delay.instance);
    const auto second = deterministic_render(delay, params);
    CHECK(first == second);
    CHECK(std::any_of(first.begin(), first.end(), [](float value) { return value != 0.0f; }));
}

TEST_CASE("Forge CharacterDelay contains non-finite control input",
          "[forge][character-delay][catalog][nonfinite]") {
    const std::array bad_values{std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()};
    for (float bad : bad_values) {
        CHECK_FALSE(catalog::detail::baked_switch_on(bad));
    }
    CHECK_FALSE(catalog::detail::baked_switch_on(0.49f));
    CHECK(catalog::detail::baked_switch_on(0.5f));

    for (const auto& realization : kRealizations) {
        DirectDelay delay(catalog::make_character_delay_node(realization.character,
                                                              realization.tier));
        for (const auto& param : kParams) {
            for (float bad : bad_values) {
                FixedParamView params;
                params.set(catalog::kTimeMs, 1.0f);
                params.set(param.id, bad);
                StereoBlock input;
                StereoBlock output;
                input.left.fill(0.25f);
                input.right.fill(-0.25f);
                delay.process(input, output, params);
                INFO("type " << realization.type_id << ", ParamID " << param.id);
                CHECK(std::all_of(output.left.begin(), output.left.end(),
                                  [](float value) { return std::isfinite(value); }));
                CHECK(std::all_of(output.right.begin(), output.right.end(),
                                  [](float value) { return std::isfinite(value); }));
            }
        }
    }
}

TEST_CASE("Forge CharacterDelay process and reset are allocation-free",
          "[forge][character-delay][catalog][rt-safety]") {
    for (const auto& realization : kRealizations) {
        BakedDelay fixture(catalog::make_character_delay_node(realization.character,
                                                               realization.tier));
        auto injector = fixture.baked().claim_param_injection(fixture.node);
        pulp::state::ParameterEventQueue controls;
        for (const auto& param : kParams) REQUIRE(controls.push(immediate(param.id, param.default_value)));
        StereoBlock input;
        StereoBlock output;
        input.left[0] = 1.0f;
        process_block(*fixture.result.processor, input, output);  // prime first-call paths
        INFO("type " << realization.type_id);
        {
            pulp::test::RtAllocationProbe probe;
            REQUIRE(injector.inject(controls) == InjectStatus::Ok);
            process_block(*fixture.result.processor, input, output);
            const auto count = probe.allocation_count();
            const auto bytes = probe.allocated_bytes();
            CHECK(count == 0);
            CHECK(bytes == 0);
        }

        // Reset is a separate custom-node lifecycle hook; it is not exposed by
        // format::Processor, so probe it directly as the bake step binds it.
        const auto type = catalog::make_character_delay_node(realization.character,
                                                              realization.tier);
        void* instance = type.create();
        REQUIRE(instance != nullptr);
        type.prepare(instance, kSr, kFrames);
        {
            pulp::test::RtAllocationProbe probe;
            type.reset(instance);
            const auto count = probe.allocation_count();
            const auto bytes = probe.allocated_bytes();
            CHECK(count == 0);
            CHECK(bytes == 0);
        }
        type.destroy(instance);
    }
}
