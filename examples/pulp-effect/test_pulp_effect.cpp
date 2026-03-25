#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_effect.hpp"
#include <pulp/format/headless.hpp>
#include <cmath>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

static float rms(const audio::Buffer<float>& buf, int ch) {
    float sum = 0;
    for (std::size_t i = 0; i < buf.num_samples(); ++i) {
        float v = buf.channel(ch)[i];
        sum += v * v;
    }
    return std::sqrt(sum / buf.num_samples());
}

static void process_headless(format::HeadlessHost& host,
                             const audio::Buffer<float>& in,
                             audio::Buffer<float>& out) {
    const float* ptrs[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ptrs, 2, in.num_samples());
    auto ov = out.view();
    host.process(ov, iv);
}

TEST_CASE("PulpEffect descriptor", "[pulpeffect]") {
    auto proc = create_pulp_effect();
    auto desc = proc->descriptor();
    REQUIRE(desc.name == "PulpEffect");
    REQUIRE(desc.category == format::PluginCategory::Effect);
}

TEST_CASE("PulpEffect has 5 parameters", "[pulpeffect]") {
    format::HeadlessHost host(create_pulp_effect);
    REQUIRE(host.state().param_count() == 5);
    REQUIRE(host.state().info(kFrequency)->name == "Frequency");
    REQUIRE(host.state().info(kFilterType)->name == "Type");
}

TEST_CASE("PulpEffect lowpass attenuates high frequencies", "[pulpeffect]") {
    format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 4096);
    host.state().set_value(kFrequency, 200.0f);  // Low cutoff
    host.state().set_value(kResonance, 0.707f);
    host.state().set_value(kFilterType, 0.0f);    // Lowpass
    host.state().set_value(kMix, 100.0f);

    // Generate 10kHz sine (should be attenuated by 200Hz lowpass)
    audio::Buffer<float> in(2, 4096), out(2, 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        float v = std::sin(2.0f * M_PI * 10000.0f * i / 48000.0f);
        in.channel(0)[i] = v;
        in.channel(1)[i] = v;
    }

    process_headless(host, in, out);

    float in_rms = rms(in, 0);
    float out_rms = rms(out, 0);

    // 10kHz through 200Hz lowpass should be heavily attenuated
    REQUIRE(out_rms < in_rms * 0.1f);
}

TEST_CASE("PulpEffect bypass passes through unmodified", "[pulpeffect]") {
    format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 512);
    host.state().set_value(kBypass, 1.0f);
    host.state().set_value(kFrequency, 100.0f); // Should be ignored

    audio::Buffer<float> in(2, 512), out(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        in.channel(0)[i] = std::sin(2.0f * M_PI * 1000.0f * i / 48000.0f);
        in.channel(1)[i] = in.channel(0)[i];
    }

    process_headless(host, in, out);

    for (std::size_t i = 0; i < 512; ++i)
        REQUIRE_THAT(out.channel(0)[i], WithinAbs(in.channel(0)[i], 1e-6));
}

TEST_CASE("PulpEffect mix blends dry and wet", "[pulpeffect]") {
    format::HeadlessHost host(create_pulp_effect);
    host.prepare(48000.0, 4096);
    host.state().set_value(kFrequency, 200.0f);
    host.state().set_value(kFilterType, 0.0f); // Lowpass
    host.state().set_value(kMix, 50.0f);        // 50% wet

    // 10kHz sine
    audio::Buffer<float> in(2, 4096), out(2, 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        in.channel(0)[i] = std::sin(2.0f * M_PI * 10000.0f * i / 48000.0f);
        in.channel(1)[i] = in.channel(0)[i];
    }

    process_headless(host, in, out);

    float in_rms = rms(in, 0);
    float out_rms = rms(out, 0);

    // 50% mix: output should be between full dry and full wet
    REQUIRE(out_rms > in_rms * 0.3f);  // More than full wet (which was < 0.1)
    REQUIRE(out_rms < in_rms * 0.8f);  // Less than full dry
}

TEST_CASE("PulpEffect state round-trip", "[pulpeffect]") {
    format::HeadlessHost h1(create_pulp_effect);
    h1.state().set_value(kFrequency, 2500.0f);
    h1.state().set_value(kResonance, 3.0f);
    h1.state().set_value(kFilterType, 1.0f);
    h1.state().set_value(kMix, 75.0f);

    auto saved = h1.save_state();

    format::HeadlessHost h2(create_pulp_effect);
    REQUIRE(h2.load_state(saved));
    REQUIRE_THAT(h2.state().get_value(kFrequency), WithinAbs(2500.0, 0.1));
    REQUIRE_THAT(h2.state().get_value(kResonance), WithinAbs(3.0, 0.01));
    REQUIRE_THAT(h2.state().get_value(kFilterType), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(h2.state().get_value(kMix), WithinAbs(75.0, 0.1));
}
