#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>
#include <cmath>

// Simple test processor for headless testing
namespace {

static pulp::format::ProcessContext last_context{};

class TestGainProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "TestGain",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.gain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext& context) override
    {
        last_context = context;
        float db = state().get_value(1);
        float gain = std::pow(10.0f, db / 20.0f);
        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                out[i] = in[i] * gain;
        }
    }
};

std::unique_ptr<pulp::format::Processor> create_test_gain() {
    return std::make_unique<TestGainProcessor>();
}

} // anonymous namespace

using Catch::Matchers::WithinAbs;

TEST_CASE("HeadlessHost creates processor", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    REQUIRE(host.descriptor().name == "TestGain");
    REQUIRE(host.state().param_count() == 1);
}

TEST_CASE("HeadlessHost processes audio at unity gain", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 128; ++i)
            in.channel(ch)[i] = 0.5f;

    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    host.process(out_view, in_view);

    // 0 dB = unity gain
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.5, 0.001));
}

TEST_CASE("HeadlessHost applies parameter changes", "[headless]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    host.state().set_value(1, 6.0f); // +6 dB ≈ 2x

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 128; ++i)
            in.channel(ch)[i] = 0.5f;

    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    host.process(out_view, in_view);

    float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(out.channel(0)[0]),
                 Catch::Matchers::WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("HeadlessHost accepts explicit transport context for offline stepping", "[headless][phase12]") {
    pulp::format::HeadlessHost host(create_test_gain);
    host.prepare(48000.0, 256);

    pulp::audio::Buffer<float> in(2, 128), out(2, 128);
    const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 128);
    auto out_view = out.view();

    last_context = {};
    pulp::format::ProcessContext ctx;
    ctx.is_playing = true;
    ctx.tempo_bpm = 98.0;
    ctx.position_beats = 12.5;
    ctx.position_samples = 2048;
    ctx.time_sig_numerator = 7;
    ctx.time_sig_denominator = 8;

    host.process(out_view, in_view, ctx);

    REQUIRE_THAT(last_context.sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(last_context.num_samples == 128);
    REQUIRE(last_context.is_playing);
    REQUIRE_THAT(last_context.tempo_bpm, WithinAbs(98.0, 0.001));
    REQUIRE_THAT(last_context.position_beats, WithinAbs(12.5, 0.001));
    REQUIRE(last_context.position_samples == 2048);
    REQUIRE(last_context.time_sig_numerator == 7);
    REQUIRE(last_context.time_sig_denominator == 8);
}

TEST_CASE("HeadlessHost state round-trip", "[headless]") {
    pulp::format::HeadlessHost host1(create_test_gain);
    host1.state().set_value(1, -12.5f);

    auto saved = host1.save_state();

    pulp::format::HeadlessHost host2(create_test_gain);
    REQUIRE(host2.load_state(saved));
    REQUIRE_THAT(host2.state().get_value(1), WithinAbs(-12.5, 0.01));
}
