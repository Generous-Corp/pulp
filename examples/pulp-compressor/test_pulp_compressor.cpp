#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_compressor.hpp"
#include <pulp/format/headless.hpp>
#include <cmath>
#include <numbers>

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

TEST_CASE("PulpCompressor descriptor has sidechain bus", "[compressor]") {
    auto proc = create_pulp_compressor();
    auto desc = proc->descriptor();
    REQUIRE(desc.name == "PulpCompressor");
    REQUIRE(desc.input_buses.size() == 2);
    REQUIRE(desc.input_buses[0].name == "Main In");
    REQUIRE(desc.input_buses[0].optional == false);
    REQUIRE(desc.input_buses[1].name == "Sidechain");
    REQUIRE(desc.input_buses[1].optional == true);
    REQUIRE(desc.output_buses.size() == 1);
}

TEST_CASE("PulpCompressor has 6 parameters", "[compressor]") {
    format::HeadlessHost host(create_pulp_compressor);
    REQUIRE(host.state().param_count() == 6);
    REQUIRE(host.state().info(kThreshold)->name == "Threshold");
    REQUIRE(host.state().info(kRatio)->name == "Ratio");
}

TEST_CASE("PulpCompressor reduces loud signals", "[compressor]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 4096);
    host.state().set_value(kThreshold, -20.0f);
    host.state().set_value(kRatio, 4.0f);
    host.state().set_value(kAttack, 0.1f);  // Fast attack

    // Loud sine wave (0 dBFS peak)
    audio::Buffer<float> in(2, 4096), out(2, 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        float v = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * i / 48000.0f);
        in.channel(0)[i] = v;
        in.channel(1)[i] = v;
    }

    process_headless(host, in, out);

    float in_rms = rms(in, 0);
    float out_rms = rms(out, 0);

    // With threshold at -20dB and 4:1 ratio, output should be quieter
    REQUIRE(out_rms < in_rms * 0.8f);
}

TEST_CASE("PulpCompressor bypass passes through", "[compressor]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 512);
    host.state().set_value(kBypass, 1.0f);

    audio::Buffer<float> in(2, 512), out(2, 512);
    for (std::size_t i = 0; i < 512; ++i) {
        in.channel(0)[i] = 0.5f;
        in.channel(1)[i] = 0.5f;
    }

    process_headless(host, in, out);
    REQUIRE_THAT(out.channel(0)[100], WithinAbs(0.5, 0.001));
}

TEST_CASE("PulpCompressor state round-trip", "[compressor]") {
    format::HeadlessHost h1(create_pulp_compressor);
    h1.state().set_value(kThreshold, -30.0f);
    h1.state().set_value(kRatio, 8.0f);
    h1.state().set_value(kMakeupGain, 6.0f);

    auto saved = h1.save_state();

    format::HeadlessHost h2(create_pulp_compressor);
    REQUIRE(h2.load_state(saved));
    REQUIRE_THAT(h2.state().get_value(kThreshold), WithinAbs(-30.0, 0.1));
    REQUIRE_THAT(h2.state().get_value(kRatio), WithinAbs(8.0, 0.1));
    REQUIRE_THAT(h2.state().get_value(kMakeupGain), WithinAbs(6.0, 0.1));
}

// ── Deeper golden cases (#356 golden breadth) ─────────────────────────

// Below threshold the compressor is a no-op: gain == 1.0, so a quiet
// signal should come out numerically identical to the input. If this
// ever regresses we've either broken the `envelope_ > threshold_lin`
// short-circuit or accidentally applied makeup gain to bypass.
TEST_CASE("PulpCompressor golden: below-threshold is unity pass-through",
          "[compressor][golden][issue-356]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 1024);
    host.state().set_value(kThreshold, -10.0f);  // -10 dBFS
    host.state().set_value(kRatio, 8.0f);
    host.state().set_value(kMakeupGain, 0.0f);

    // Peak amplitude ≈ 0.01 ⇒ ~-40 dBFS, well below threshold.
    audio::Buffer<float> in(2, 1024), out(2, 1024);
    for (std::size_t i = 0; i < 1024; ++i) {
        float v = 0.01f * std::sin(2.0f * std::numbers::pi_v<float>
                                   * 440.0f * i / 48000.0f);
        in.channel(0)[i] = v;
        in.channel(1)[i] = v;
    }

    process_headless(host, in, out);

    for (std::size_t i = 0; i < 1024; ++i) {
        REQUIRE_THAT(out.channel(0)[i],
                     WithinAbs(in.channel(0)[i], 1e-6));
        REQUIRE_THAT(out.channel(1)[i],
                     WithinAbs(in.channel(1)[i], 1e-6));
    }
}

// Makeup gain below threshold is pure linear scaling. A +6 dB makeup
// with a sub-threshold input ⇒ output RMS ≈ 2× input RMS.
TEST_CASE("PulpCompressor golden: makeup gain is linear below threshold",
          "[compressor][golden][issue-356]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 1024);
    host.state().set_value(kThreshold, -10.0f);
    host.state().set_value(kRatio, 4.0f);
    host.state().set_value(kMakeupGain, 6.0f);

    audio::Buffer<float> in(2, 1024), out(2, 1024);
    for (std::size_t i = 0; i < 1024; ++i) {
        float v = 0.05f * std::sin(2.0f * std::numbers::pi_v<float>
                                   * 440.0f * i / 48000.0f);
        in.channel(0)[i] = v;
        in.channel(1)[i] = v;
    }

    process_headless(host, in, out);

    const float expected_ratio = std::pow(10.0f, 6.0f / 20.0f);  // ≈1.995
    const float in_rms = rms(in, 0);
    const float out_rms = rms(out, 0);

    // Makeup below threshold is a pure gain, so ratio is tight.
    REQUIRE(out_rms > in_rms * (expected_ratio - 0.05f));
    REQUIRE(out_rms < in_rms * (expected_ratio + 0.05f));
}

// A 20:1 ratio approximates a brick-wall limiter. With a fast attack
// and a loud sine far over threshold, the post-attack output peak
// should sit within a few dB of threshold — definitely <<< input peak.
TEST_CASE("PulpCompressor golden: high-ratio limiter tames peaks",
          "[compressor][golden][issue-356]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 4096);
    host.state().set_value(kThreshold, -20.0f);
    host.state().set_value(kRatio, 20.0f);
    host.state().set_value(kAttack, 0.1f);
    host.state().set_value(kRelease, 50.0f);
    host.state().set_value(kMakeupGain, 0.0f);

    audio::Buffer<float> in(2, 4096), out(2, 4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        float v = std::sin(2.0f * std::numbers::pi_v<float> * 440.0f
                           * i / 48000.0f);
        in.channel(0)[i] = v;
        in.channel(1)[i] = v;
    }
    process_headless(host, in, out);

    // Discard the first 1024 samples: attack envelope is still ramping.
    float out_peak = 0.0f;
    for (std::size_t i = 1024; i < 4096; ++i) {
        out_peak = std::max(out_peak, std::abs(out.channel(0)[i]));
    }

    // Threshold -20 dBFS ≈ 0.1 linear. With 20:1 and fast attack the
    // settled peak should be well under half the unity-gain peak.
    REQUIRE(out_peak < 0.5f);
}

// Release-tail sanity: after the detector sees loud material, once the
// input goes back below threshold the compressor must eventually
// relax back to unity gain. We drive a loud burst, then zero-fill,
// then a tiny sub-threshold signal and check it passes near-unity.
TEST_CASE("PulpCompressor golden: gain relaxes to unity after transient",
          "[compressor][golden][issue-356]") {
    format::HeadlessHost host(create_pulp_compressor);
    host.prepare(48000.0, 1);  // block=1 forces the envelope to step per sample
    host.state().set_value(kThreshold, -20.0f);
    host.state().set_value(kRatio, 20.0f);
    host.state().set_value(kAttack, 0.5f);
    host.state().set_value(kRelease, 20.0f);  // fastish release
    host.state().set_value(kMakeupGain, 0.0f);

    auto step = [&](float v_in) {
        float in_L = v_in, in_R = v_in;
        float out_L = 0.0f, out_R = 0.0f;
        const float* ip[2] = {&in_L, &in_R};
        float* op[2] = {&out_L, &out_R};
        audio::BufferView<const float> iv(ip, 2, 1);
        audio::BufferView<float> ov(op, 2, 1);
        host.process(ov, iv);
        return out_L;
    };

    // Loud burst to drive the envelope up.
    for (int i = 0; i < 4800; ++i) step(0.8f);

    // Long silent stretch for release to finish — 96 ms at 48 kHz.
    for (int i = 0; i < 4608; ++i) step(0.0f);

    // Quiet probe below threshold, after release: should be unity.
    float probe = step(0.01f);
    REQUIRE_THAT(probe, WithinAbs(0.01, 0.001));
}
