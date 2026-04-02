#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "faust_filter.hpp"
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

struct FaustFilterFixture {
    state::StateStore store;
    std::unique_ptr<format::Processor> processor;

    FaustFilterFixture() {
        processor = create_faust_filter();
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

TEST_CASE("FaustFilter descriptor", "[faust][filter]") {
    FaustFilterFixture fx;
    auto desc = fx.processor->descriptor();
    REQUIRE(desc.name == "FaustFilter");
    REQUIRE(desc.category == format::PluginCategory::Effect);
}

TEST_CASE("FaustFilter parameter count and names", "[faust][filter]") {
    FaustFilterFixture fx;
    REQUIRE(fx.store.param_count() == 2);

    auto* freq_info = fx.store.info(1);
    REQUIRE(freq_info != nullptr);
    REQUIRE(freq_info->name == "Frequency");
    REQUIRE(freq_info->unit == "Hz");
    REQUIRE_THAT(freq_info->range.default_value, WithinAbs(1000.0, 0.01));

    auto* q_info = fx.store.info(2);
    REQUIRE(q_info != nullptr);
    REQUIRE(q_info->name == "Resonance");
    REQUIRE_THAT(q_info->range.default_value, WithinAbs(0.707, 0.01));
}

TEST_CASE("FaustFilter attenuates high frequencies", "[faust][filter]") {
    FaustFilterFixture fx;
    fx.store.set_value(1, 200.0f);  // Low cutoff: 200 Hz
    fx.store.set_value(2, 0.707f);

    // Generate a high-frequency signal (10 kHz at 48 kHz SR)
    audio::Buffer<float> in(2, 1024), out(2, 1024);
    for (std::size_t i = 0; i < 1024; ++i) {
        float sample = std::sin(2.0f * 3.14159265f * 10000.0f * static_cast<float>(i) / 48000.0f);
        in.channel(0)[i] = sample;
        in.channel(1)[i] = sample;
    }

    fx.process(in, out);

    // After settling, output should be much quieter than input
    // Check RMS of last 256 samples
    float rms_in = 0.0f, rms_out = 0.0f;
    for (std::size_t i = 768; i < 1024; ++i) {
        rms_in += in.channel(0)[i] * in.channel(0)[i];
        rms_out += out.channel(0)[i] * out.channel(0)[i];
    }
    rms_in = std::sqrt(rms_in / 256.0f);
    rms_out = std::sqrt(rms_out / 256.0f);

    // 10 kHz through 200 Hz LP should be heavily attenuated
    REQUIRE(rms_out < rms_in * 0.1f);
}

TEST_CASE("FaustFilter passes low frequencies", "[faust][filter]") {
    FaustFilterFixture fx;
    fx.store.set_value(1, 10000.0f);  // High cutoff
    fx.store.set_value(2, 0.707f);

    // Generate a low-frequency signal (100 Hz)
    audio::Buffer<float> in(2, 2048), out(2, 2048);
    for (std::size_t i = 0; i < 2048; ++i) {
        float sample = std::sin(2.0f * 3.14159265f * 100.0f * static_cast<float>(i) / 48000.0f);
        in.channel(0)[i] = sample;
        in.channel(1)[i] = sample;
    }

    fx.process(in, out);

    // After settling, output should be close to input amplitude
    float rms_in = 0.0f, rms_out = 0.0f;
    for (std::size_t i = 1024; i < 2048; ++i) {
        rms_in += in.channel(0)[i] * in.channel(0)[i];
        rms_out += out.channel(0)[i] * out.channel(0)[i];
    }
    rms_in = std::sqrt(rms_in / 1024.0f);
    rms_out = std::sqrt(rms_out / 1024.0f);

    // 100 Hz through 10 kHz LP should pass with minimal attenuation
    REQUIRE(rms_out > rms_in * 0.8f);
}

TEST_CASE("FaustFilter state round-trip", "[faust][filter]") {
    FaustFilterFixture fx;
    fx.store.set_value(1, 440.0f);
    fx.store.set_value(2, 2.5f);

    auto data = fx.store.serialize();

    FaustFilterFixture fx2;
    REQUIRE(fx2.store.deserialize(data));
    REQUIRE_THAT(fx2.store.get_value(1), WithinAbs(440.0, 0.01));
    REQUIRE_THAT(fx2.store.get_value(2), WithinAbs(2.5, 0.01));
}
