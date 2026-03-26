#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>
#include <cmath>

#include "pulp_effect.hpp"

using Catch::Matchers::WithinAbs;

// Helper: generate a sine wave buffer
static pulp::audio::Buffer<float> make_sine(int channels, int samples,
                                             float freq = 440.0f,
                                             double sample_rate = 48000.0) {
    pulp::audio::Buffer<float> buf(channels, samples);
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < samples; ++i) {
            buf.channel(ch)[i] = std::sin(2.0f * M_PI * freq * i / sample_rate);
        }
    }
    return buf;
}

// Helper: compute RMS of a buffer channel
static float rms(const pulp::audio::Buffer<float>& buf, int ch) {
    float sum = 0.0f;
    auto span = buf.channel(ch);
    for (std::size_t i = 0; i < buf.num_samples(); ++i) {
        sum += span[i] * span[i];
    }
    return std::sqrt(sum / static_cast<float>(buf.num_samples()));
}

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

    float in_rms = rms(high_freq, 0);
    float out_rms = rms(out, 0);

    // Lowpass at 200 Hz should heavily attenuate 8 kHz
    REQUIRE(out_rms < in_rms * 0.1f); // at least -20 dB attenuation
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

    float in_rms = rms(low_freq, 0);
    float out_rms = rms(out, 0);

    // Lowpass at 5 kHz should barely attenuate 100 Hz
    REQUIRE(out_rms > in_rms * 0.8f); // within ~2 dB
}
