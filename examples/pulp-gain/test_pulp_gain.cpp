#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_gain.hpp"
#include <pulp/format/validation_assertions.hpp>
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Helper: create a processor with state store wired up
struct GainFixture {
    state::StateStore store;
    std::unique_ptr<format::Processor> processor;

    GainFixture() {
        processor = create_pulp_gain();
        processor->set_state_store(&store);
        processor->define_parameters(store);
        processor->prepare({48000.0, 512, 2, 2});
    }

    void process(audio::Buffer<float>& in, audio::Buffer<float>& out) {
        auto in_view = in.view();
        auto out_view = out.view();
        // Need const input view
        const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> const_in(in_ptrs, 2, in.num_samples());

        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = static_cast<int>(in.num_samples());
        processor->process(out_view, const_in, midi_in, midi_out, ctx);
    }
};

TEST_CASE("PulpGain descriptor", "[pulpgain]") {
    PulpGainProcessor proc;
    auto desc = proc.descriptor();
    REQUIRE(desc.name == "PulpGain");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    REQUIRE(desc.default_input_channels() == 2);
    REQUIRE(desc.default_output_channels() == 2);
    REQUIRE_FALSE(desc.accepts_midi);
}

TEST_CASE("PulpGain parameters", "[pulpgain]") {
    GainFixture fx;
    REQUIRE(fx.store.param_count() == 3);

    auto* input_info = fx.store.info(kInputGain);
    REQUIRE(input_info != nullptr);
    REQUIRE(input_info->name == "Input Gain");

    // Defaults
    REQUIRE_THAT(fx.store.get_value(kInputGain), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(fx.store.get_value(kOutputGain), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(fx.store.get_value(kBypass), WithinAbs(0.0, 0.01));
}

TEST_CASE("PulpGain unity gain (0 dB)", "[pulpgain]") {
    GainFixture fx;

    audio::Buffer<float> in(2, 256), out(2, 256);
    // Fill input with 1.0
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 1.0f;

    fx.process(in, out);

    // At 0 dB input + 0 dB output = unity gain
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            REQUIRE_THAT(out.channel(ch)[i], WithinAbs(1.0, 0.001));
}

TEST_CASE("PulpGain applies gain", "[pulpgain]") {
    GainFixture fx;
    fx.store.set_value(kInputGain, 6.0f); // +6 dB ≈ 2x

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 0.5f;

    fx.process(in, out);

    float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(out.channel(0)[0]), WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("PulpGain bypass", "[pulpgain]") {
    GainFixture fx;
    fx.store.set_value(kBypass, 1.0f);
    fx.store.set_value(kInputGain, 12.0f); // Should be ignored

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 0.7f;

    fx.process(in, out);

    // Bypass = pass-through regardless of gain settings
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.7, 0.001));
}

TEST_CASE("PulpGain passes shared validation assertions", "[pulpgain]") {
    namespace v = pulp::format::validation;

    GainFixture fx;
    fx.store.set_value(kInputGain, 6.0f); // +6 dB

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 0.25f;

    fx.process(in, out);
    auto out_view = out.view();

    // Reusable SDK assertions (validation_assertions.hpp): no NaN/Inf, the
    // stage actually produced signal, and +6 dB on a 0.25 input stays below
    // unity so nothing clips.
    auto finite = v::check_finite(out_view);
    REQUIRE(finite.ok);
    REQUIRE(v::check_any_nonzero(out.channel(0)).ok);
    REQUIRE(v::check_peak_below(out.channel(0), 1.0f).ok);

    // Every declared parameter's host-automation round trip is stable.
    auto* input_info = fx.store.info(kInputGain);
    REQUIRE(input_info != nullptr);
    REQUIRE(v::check_param_round_trip(input_info->range, 6.0f).ok);
    REQUIRE(v::check_param_round_trip(input_info->range, input_info->range.min).ok);
    REQUIRE(v::check_param_round_trip(input_info->range, input_info->range.max).ok);
}

TEST_CASE("PulpGain state round-trip", "[pulpgain]") {
    GainFixture fx;
    fx.store.set_value(kInputGain, -12.5f);
    fx.store.set_value(kOutputGain, 3.0f);
    fx.store.set_value(kBypass, 1.0f);

    auto data = fx.store.serialize();

    // Load into a fresh instance
    GainFixture fx2;
    REQUIRE(fx2.store.deserialize(data));
    REQUIRE_THAT(fx2.store.get_value(kInputGain), WithinAbs(-12.5, 0.01));
    REQUIRE_THAT(fx2.store.get_value(kOutputGain), WithinAbs(3.0, 0.01));
    REQUIRE_THAT(fx2.store.get_value(kBypass), WithinAbs(1.0, 0.01));
}
