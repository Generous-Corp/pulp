#include <catch2/catch_test_macros.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "support/render_scenario.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/unit_delay.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {

using pulp::signal::UnitDelay;
using pulp::test::audio::RenderScenario;

class UnitDelayProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "UnitDelayFixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.unit-delay",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 1}},
            .output_buses = {{"Audio Out", 1}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {
        delay_.reset();
    }
    int latency_samples() const override {
        return 0;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext& context) override {
        if (context.reset_requested)
            delay_.reset();

        const auto frames = std::min(output.num_samples(), input.num_samples());
        for (std::size_t frame = 0; frame < frames; ++frame)
            output.channel(0)[frame] = delay_.process(input.channel(0)[frame]);
    }

  private:
    UnitDelay delay_;
};

std::unique_ptr<pulp::format::Processor> create_unit_delay_processor() {
    return std::make_unique<UnitDelayProcessor>();
}

template <typename Delay> std::array<float, 5> render_scalar(Delay& delay) {
    constexpr std::array<float, 5> input{1.0f, -0.25f, 0.5f, 0.0f, 0.75f};
    std::array<float, input.size()> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        output[i] = delay.process(input[i]);
    return output;
}

struct PublishBeforeReadDelay {
    float state = 0.0f;

    float process(float input) {
        state = input;
        return state;
    }
};

} // namespace

TEST_CASE("UnitDelay returns the independent scalar one-sample oracle",
          "[signal][unit-delay][scalar]") {
    UnitDelay delay;
    constexpr std::array<float, 5> expected{0.0f, 1.0f, -0.25f, 0.5f, 0.0f};

    REQUIRE(render_scalar(delay) == expected);
}

TEST_CASE("UnitDelay publishes old state until the next state is committed",
          "[signal][unit-delay][causal]") {
    UnitDelay delay;

    REQUIRE(delay.publish() == 0.0f);
    delay.commit(0.25f);
    REQUIRE(delay.publish() == 0.25f);
    REQUIRE(delay.publish() == 0.25f);
    delay.commit(-0.5f);
    REQUIRE(delay.publish() == -0.5f);
}

TEST_CASE("UnitDelay impulse and reset are exact through RenderScenario",
          "[signal][unit-delay][audio-harness][i2][DSP-01]") {
    auto result = RenderScenario(create_unit_delay_processor)
                      .name("unit-delay.impulse")
                      .sample_rate(48'000.0)
                      .block_size(3)
                      .channels(1, 1)
                      .input(pulp::test::audio::make_impulse(1, 8))
                      .render();
    constexpr std::array<float, 8> expected{0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    REQUIRE(std::equal(result.output.channel(0).begin(), result.output.channel(0).end(),
                       expected.begin()));
    REQUIRE(result.latency.reported_samples == 0);

    UnitDelay delay;
    REQUIRE(delay.process(1.0f) == 0.0f);
    REQUIRE(delay.process(0.0f) == 1.0f);
    delay.reset();
    REQUIRE_FALSE(std::signbit(delay.publish()));
    REQUIRE(delay.process(1.0f) == 0.0f);
    REQUIRE(delay.process(0.0f) == 1.0f);
}

TEST_CASE("UnitDelay process and reset are realtime safe", "[signal][unit-delay][rt-safety]") {
    UnitDelay delay;
    std::array<float, 64> output{};
    std::size_t allocation_count = 0;

    {
        pulp::test::ScopedRtProcessProbe probe;
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = delay.process(static_cast<float>(i));
        delay.reset();
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);
    REQUIRE(output[0] == 0.0f);
    REQUIRE(output[1] == 0.0f);
    REQUIRE(output[63] == 62.0f);
    REQUIRE(delay.publish() == 0.0f);
}

TEST_CASE("UnitDelay scalar oracle rejects publish-before-read ordering",
          "[signal][unit-delay][negative-control][i2][NEG-03]") {
    PublishBeforeReadDelay broken;
    constexpr std::array<float, 5> expected{0.0f, 1.0f, -0.25f, 0.5f, 0.0f};

    REQUIRE_FALSE(render_scalar(broken) == expected);
}
