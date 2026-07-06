#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>

// Signal generation and RMS measurement use the shared harness helpers
// (test/support/).
#include <pulp/audio/analysis/audio_metrics.hpp>
#include "support/audio_test_signals.hpp"

#include "pulp_effect.hpp"

using Catch::Matchers::WithinAbs;
using pulp::test::audio::analyze;
using pulp::test::audio::make_sine;

// Helper: process through headless host
static void process_headless(pulp::format::HeadlessHost& host,
                             const pulp::audio::Buffer<float>& in,
                             pulp::audio::Buffer<float>& out) {
    const float* in_ptrs[2] = {in.channel(0).data(),
                                in.num_channels() > 1 ? in.channel(1).data() : in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, in.num_channels(), in.num_samples());
    auto out_view = out.view();
    host.process(out_view, in_view);
}

using namespace pulp::examples;

TEST_CASE("Golden: PulpEffect lowpass attenuates high frequencies", "[golden][pulpeffect]") {
    pulp::format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 4096);

    host.state().set_value(kFrequency, 200.0f);
    host.state().set_value(kResonance, 0.707f);
    host.state().set_value(kFilterType, 0.0f);  // lowpass
    host.state().set_value(kMix, 100.0f);         // 100% wet
    host.state().set_value(kBypass, 0.0f);

    // Generate a high-frequency sine (8000 Hz) — should be attenuated
    auto high_freq = make_sine(2, 4096, 8000.0f);
    pulp::audio::Buffer<float> out(2, 4096);

    // Process a few blocks to let filter settle
    for (int b = 0; b < 3; ++b) {
        process_headless(host, high_freq, out);
    }

    const double in_rms = analyze(high_freq, 48000.0).channels[0].rms;
    const double out_rms = analyze(out, 48000.0).channels[0].rms;

    // Lowpass at 200 Hz should heavily attenuate 8 kHz
    REQUIRE(out_rms < in_rms * 0.1); // at least -20 dB attenuation
}

TEST_CASE("Golden: PulpEffect bypass passes through unchanged", "[golden][pulpeffect]") {
    pulp::format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 1024);

    host.state().set_value(kBypass, 1.0f); // bypass ON

    auto in = make_sine(2, 1024);
    pulp::audio::Buffer<float> out(2, 1024);
    process_headless(host, in, out);

    // Bypass: output should match input
    for (std::size_t i = 0; i < 1024; ++i) {
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(in.channel(0)[i], 1e-6));
    }
}

TEST_CASE("Golden: PulpEffect lowpass preserves low frequencies", "[golden][pulpeffect]") {
    pulp::format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 4096);

    host.state().set_value(kFrequency, 5000.0f);
    host.state().set_value(kResonance, 0.707f);
    host.state().set_value(kFilterType, 0.0f);
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kBypass, 0.0f);

    // Generate a low-frequency sine (100 Hz) — should pass through
    auto low_freq = make_sine(2, 4096, 100.0f);
    pulp::audio::Buffer<float> out(2, 4096);

    // Process a few blocks to let filter settle
    for (int b = 0; b < 3; ++b) {
        process_headless(host, low_freq, out);
    }

    const double in_rms = analyze(low_freq, 48000.0).channels[0].rms;
    const double out_rms = analyze(out, 48000.0).channels[0].rms;

    // Lowpass at 5 kHz should barely attenuate 100 Hz
    REQUIRE(out_rms > in_rms * 0.8); // within ~2 dB
}
