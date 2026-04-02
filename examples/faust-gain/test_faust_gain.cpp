#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "faust_gain.hpp"
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

struct FaustGainFixture {
    state::StateStore store;
    std::unique_ptr<format::Processor> processor;

    FaustGainFixture() {
        processor = create_faust_gain();
        processor->set_state_store(&store);
        processor->define_parameters(store);
        processor->prepare({48000.0, 512, 2, 2});
    }

    void process(audio::Buffer<float>& in, audio::Buffer<float>& out) {
        auto out_view = out.view();
        const float* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const float> const_in(in_ptrs, 2, in.num_samples());
        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.num_samples = static_cast<int>(in.num_samples());
        processor->process(out_view, const_in, midi_in, midi_out, ctx);
    }
};

TEST_CASE("FaustGain descriptor from metadata", "[faust][gain]") {
    FaustGainFixture fx;
    auto desc = fx.processor->descriptor();
    REQUIRE(desc.name == "FaustGain");
    REQUIRE(desc.manufacturer == "Pulp");
    REQUIRE(desc.version == "1.0.0");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    REQUIRE(desc.default_input_channels() == 2);
    REQUIRE(desc.default_output_channels() == 2);
    REQUIRE_FALSE(desc.accepts_midi);
}

TEST_CASE("FaustGain parameter count and names match FAUST metadata", "[faust][gain]") {
    FaustGainFixture fx;
    REQUIRE(fx.store.param_count() == 1);

    auto* info = fx.store.info(1);
    REQUIRE(info != nullptr);
    REQUIRE(info->name == "Gain");
    REQUIRE(info->unit == "dB");
    REQUIRE_THAT(info->range.min, WithinAbs(-60.0, 0.01));
    REQUIRE_THAT(info->range.max, WithinAbs(24.0, 0.01));
    REQUIRE_THAT(info->range.default_value, WithinAbs(0.0, 0.01));
}

TEST_CASE("FaustGain unity gain (0 dB)", "[faust][gain]") {
    FaustGainFixture fx;
    // Default is 0 dB = unity

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 1.0f;

    fx.process(in, out);

    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            REQUIRE_THAT(out.channel(ch)[i], WithinAbs(1.0, 0.001));
}

TEST_CASE("FaustGain applies +6 dB", "[faust][gain]") {
    FaustGainFixture fx;
    fx.store.set_value(1, 6.0f);

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 0.5f;

    fx.process(in, out);

    float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(out.channel(0)[0]),
                 WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("FaustGain applies -inf (silence)", "[faust][gain]") {
    FaustGainFixture fx;
    fx.store.set_value(1, -60.0f);

    audio::Buffer<float> in(2, 256), out(2, 256);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 256; ++i)
            in.channel(ch)[i] = 1.0f;

    fx.process(in, out);

    // -60 dB = 0.001, so output should be ~0.001
    float expected = std::pow(10.0f, -60.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(out.channel(0)[0]),
                 WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("FaustGain state serialization round-trip", "[faust][gain]") {
    FaustGainFixture fx;
    fx.store.set_value(1, -12.5f);

    auto data = fx.store.serialize();

    FaustGainFixture fx2;
    REQUIRE(fx2.store.deserialize(data));
    REQUIRE_THAT(fx2.store.get_value(1), WithinAbs(-12.5, 0.01));
}

TEST_CASE("FaustGain DSL reflection", "[faust][gain]") {
    auto proc = create_faust_gain();
    auto* dsl = dynamic_cast<dsl::DslProcessor*>(proc.get());
    REQUIRE(dsl != nullptr);
    REQUIRE(dsl->dsl_name() == "faust");
    REQUIRE(dsl->dsl_params().size() == 1);
    REQUIRE(dsl->dsl_params()[0].name == "Gain");
    REQUIRE(dsl->bus_layout().num_inputs == 2);
    REQUIRE(dsl->bus_layout().num_outputs == 2);
}
