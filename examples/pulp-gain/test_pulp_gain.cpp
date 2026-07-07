#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_gain.hpp"
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/validation_assertions.hpp>
#include <array>
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
        processor->prepare({
            .sample_rate = 48000.0,
            .max_buffer_size = 512,
            .input_channels = 2,
            .output_channels = 2,
        });
    }

    void process(audio::Buffer<float>& in, audio::Buffer<float>& out) {
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

    void process_f64(audio::Buffer<double>& in, audio::Buffer<double>& out,
                     double sample_rate = 48000.0) {
        auto out_view = out.view();
        const double* in_ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
        audio::BufferView<const double> const_in(in_ptrs, 2, in.num_samples());

        midi::MidiBuffer midi_in, midi_out;
        format::ProcessContext ctx;
        ctx.sample_rate = sample_rate;
        ctx.num_samples = static_cast<int>(in.num_samples());
        processor->process_f64(out_view, const_in, midi_in, midi_out, ctx);
    }
};

struct ClapGainFixture {
    static constexpr uint32_t kFrames = 64;
    static constexpr int kChannels = 2;

    format::clap_adapter::PulpClapPlugin* plugin = nullptr;
    bool active = false;

    ClapGainFixture() {
        plugin = new format::clap_adapter::PulpClapPlugin();
        plugin->factory = create_pulp_gain;
        plugin->plugin.plugin_data = plugin;
        REQUIRE(format::clap_adapter::clap_init(&plugin->plugin));
        REQUIRE(format::clap_adapter::clap_activate(&plugin->plugin, 48000.0, 32, kFrames));
        active = true;
    }

    ~ClapGainFixture() {
        if (active) format::clap_adapter::clap_deactivate(&plugin->plugin);
        if (plugin) format::clap_adapter::clap_destroy(&plugin->plugin);
    }

    clap_process_status run(clap_audio_buffer_t& audio_in, clap_audio_buffer_t& audio_out) {
        clap_process_t proc{};
        proc.frames_count = kFrames;
        proc.audio_inputs = &audio_in;
        proc.audio_inputs_count = 1;
        proc.audio_outputs = &audio_out;
        proc.audio_outputs_count = 1;
        return format::clap_adapter::clap_process(&plugin->plugin, &proc);
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
    REQUIRE(desc.effective_capabilities().supports_f64_audio);
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

TEST_CASE("PulpGain native f64 preserves samples that the fallback would round",
          "[pulpgain][precision][f64]") {
    GainFixture fx;

    audio::Buffer<double> in(2, 16), out(2, 16);
    const double double_only = 1.0 + std::ldexp(1.0, -40);
    REQUIRE(static_cast<double>(static_cast<float>(double_only)) != double_only);

    for (std::size_t ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < 16; ++i)
            in.channel(ch)[i] = double_only + static_cast<double>(i) * std::ldexp(1.0, -42);

    fx.process_f64(in, out);

    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < 16; ++i) {
            REQUIRE(out.channel(ch)[i] == in.channel(ch)[i]);
            REQUIRE(out.channel(ch)[i] != static_cast<double>(static_cast<float>(in.channel(ch)[i])));
        }
    }
}

TEST_CASE("PulpGain native f64 covers the sample-rate and block-size matrix",
          "[pulpgain][precision][f64]") {
    constexpr std::array<double, 4> sample_rates{44100.0, 48000.0, 96000.0, 192000.0};
    constexpr std::array<std::size_t, 5> block_sizes{32, 64, 256, 1024, 4096};

    for (double sr : sample_rates) {
        for (std::size_t block : block_sizes) {
            GainFixture fx;
            fx.processor->prepare({
                .sample_rate = sr,
                .max_buffer_size = static_cast<int>(block),
                .input_channels = 2,
                .output_channels = 2,
            });
            fx.store.set_value(kInputGain, 6.0f);

            audio::Buffer<double> in(2, block), out(2, block);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                for (std::size_t i = 0; i < block; ++i) {
                    in.channel(ch)[i] = 0.125 + static_cast<double>(ch + 1) * 0.01 +
                                        static_cast<double>(i) * 1.0e-9;
                }
            }

            fx.process_f64(in, out, sr);

            const double expected_gain = std::pow(10.0, 6.0 / 20.0);
            REQUIRE_THAT(out.channel(0)[0], WithinRel(in.channel(0)[0] * expected_gain, 1.0e-12));
            REQUIRE_THAT(out.channel(1)[block - 1],
                         WithinRel(in.channel(1)[block - 1] * expected_gain, 1.0e-12));
        }
    }
}

TEST_CASE("PulpGain CLAP data64 host path runs native f64",
          "[pulpgain][clap][precision][f64]") {
    ClapGainFixture fx;

    std::array<double, ClapGainFixture::kFrames> in_l{};
    std::array<double, ClapGainFixture::kFrames> in_r{};
    std::array<double, ClapGainFixture::kFrames> out_l{};
    std::array<double, ClapGainFixture::kFrames> out_r{};
    const double double_only = 1.0 + std::ldexp(1.0, -40);
    REQUIRE(static_cast<double>(static_cast<float>(double_only)) != double_only);

    for (std::size_t i = 0; i < ClapGainFixture::kFrames; ++i) {
        in_l[i] = double_only + static_cast<double>(i) * std::ldexp(1.0, -42);
        in_r[i] = -double_only - static_cast<double>(i) * std::ldexp(1.0, -42);
    }

    double* in_ptrs[ClapGainFixture::kChannels] = {in_l.data(), in_r.data()};
    double* out_ptrs[ClapGainFixture::kChannels] = {out_l.data(), out_r.data()};
    clap_audio_buffer_t audio_in{};
    audio_in.data64 = in_ptrs;
    audio_in.channel_count = ClapGainFixture::kChannels;
    clap_audio_buffer_t audio_out{};
    audio_out.data64 = out_ptrs;
    audio_out.channel_count = ClapGainFixture::kChannels;

    REQUIRE(fx.run(audio_in, audio_out) == CLAP_PROCESS_CONTINUE);

    for (std::size_t i = 0; i < ClapGainFixture::kFrames; ++i) {
        REQUIRE(out_l[i] == in_l[i]);
        REQUIRE(out_r[i] == in_r[i]);
        REQUIRE(out_l[i] != static_cast<double>(static_cast<float>(in_l[i])));
        REQUIRE(out_r[i] != static_cast<double>(static_cast<float>(in_r[i])));
    }
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
