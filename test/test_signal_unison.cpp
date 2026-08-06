#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/supersaw.hpp>
#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <cmath>
#include <numbers>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("Unison layout is symmetric and exposes both documented gain laws",
          "[signal][unison]") {
    UnisonLayout<> layout;
    REQUIRE(layout.configure({.voice_count = 5, .detune_cents = 20.0,
                              .stereo_spread = 1.0}));
    REQUIRE(layout.size() == 5);
    REQUIRE_THAT(layout[0].detune_cents, WithinAbs(-20.0, 1e-12));
    REQUIRE_THAT(layout[2].detune_cents, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(layout[4].pan, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(layout[0].gain, WithinAbs(0.2, 1e-12));

    REQUIRE(layout.configure({.voice_count = 4,
                              .gain_law = UnisonGainLaw::EqualPower}));
    REQUIRE_THAT(layout[0].gain, WithinAbs(0.5, 1e-12));
}

TEST_CASE("Unison layout validates configuration and randomizes phase deterministically",
          "[signal][unison]") {
    UnisonLayout<4> layout;
    REQUIRE_FALSE(layout.configure({.voice_count = 0}));
    REQUIRE_FALSE(layout.configure({.voice_count = 5}));
    REQUIRE_FALSE(layout.configure({.voice_count = 2, .stereo_spread = 1.1}));
    REQUIRE_FALSE(layout.configure({.voice_count = 2, .drift_cents = 1.0,
                                    .drift_period_frames = 0}));
    REQUIRE(layout.configure({.voice_count = 2, .phase_randomization = 1.0}, 42, 7));
    const auto phase = layout[1].phase;
    UnisonLayout<4> again;
    REQUIRE(again.configure({.voice_count = 2, .phase_randomization = 1.0}, 42, 7));
    REQUIRE(again[1].phase == phase);
    REQUIRE(layout.configure({.voice_count = 1, .phase_randomization = 1.0}, 999, 888));
    REQUIRE(layout[0].phase == 0.0);
}

TEST_CASE("Unison drift is absolute-frame deterministic and bounded",
          "[signal][unison]") {
    UnisonLayout<> a, b;
    const UnisonSpec spec{.voice_count = 3, .drift_cents = 4.0,
                          .drift_period_frames = 17};
    REQUIRE(a.configure(spec, 99, 123));
    REQUIRE(b.configure(spec, 99, 123));
    for (std::uint64_t frame : {0ull, 1ull, 16ull, 17ull, 999ull}) {
        REQUIRE(a.drift_cents(2, frame) == b.drift_cents(2, frame));
        REQUIRE(std::abs(a.drift_cents(2, frame)) <= 4.0);
    }
}

TEST_CASE("Supersaw preflights Nyquist without mutating active state",
          "[signal][supersaw]") {
    Supersaw saw;
    REQUIRE(saw.prepare(48000.0f, {.voice_count = 3, .detune_cents = 30.0,
                                  .drift_cents = 2.0}));
    REQUIRE(saw.trigger(440.0f, 1, 2));
    const auto phase = saw.layout()[0].phase;
    REQUIRE_FALSE(saw.trigger(23999.0f, 8, 9));
    REQUIRE(saw.active());
    REQUIRE(saw.layout()[0].phase == phase);
    saw.reset();
    REQUIRE_FALSE(saw.active());
    const auto silent = saw.next(0);
    REQUIRE(silent.left == 0.0f);
    REQUIRE(silent.right == 0.0f);
}

TEST_CASE("Supersaw render is callback-partition invariant and allocation free",
          "[signal][supersaw][rt]") {
    Supersaw one, split;
    const UnisonSpec spec{.voice_count = 7, .detune_cents = 18.0,
                          .stereo_spread = 0.8, .phase_randomization = 1.0,
                          .drift_cents = 1.5, .drift_period_frames = 31};
    REQUIRE(one.prepare(48000.0f, spec));
    REQUIRE(split.prepare(48000.0f, spec));
    REQUIRE(one.trigger(880.0f, 44, 55));
    REQUIRE(split.trigger(880.0f, 44, 55));
    std::array<StereoSampleT<float>, 128> reference{};
    for (std::uint64_t i = 0; i < reference.size(); ++i) reference[i] = one.next(i);
    pulp::test::RtAllocationProbe probe;
    for (std::uint64_t i = 0; i < 37; ++i) {
        const auto value = split.next(i);
        REQUIRE(value.left == reference[i].left);
        REQUIRE(value.right == reference[i].right);
    }
    for (std::uint64_t i = 37; i < reference.size(); ++i) {
        const auto value = split.next(i);
        REQUIRE(value.left == reference[i].left);
        REQUIRE(value.right == reference[i].right);
    }
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("PolyBLEP supersaw reduces explicit folded-harmonic alias energy",
          "[signal][supersaw][alias]") {
    Supersaw saw;
    REQUIRE(saw.prepare(48000.0f, {.voice_count = 1}));
    REQUIRE(saw.trigger(9000.0f, 0, 0));
    std::array<double, 512> poly{};
    std::array<double, 512> naive{};
    double naive_phase = 0.0;
    for (std::uint64_t i = 0; i < poly.size(); ++i) {
        poly[i] = saw.next(i).left * std::sqrt(2.0); // undo centre-pan gain
        naive[i] = 2.0 * naive_phase - 1.0;
        naive_phase += 9000.0 / 48000.0;
        if (naive_phase >= 1.0) naive_phase -= 1.0;
    }
    const auto bin_energy = [](const auto& samples, std::size_t bin) {
        double re = 0.0, im = 0.0;
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const double angle = -2.0 * std::numbers::pi *
                                 static_cast<double>(bin * n) / samples.size();
            re += samples[n] * std::cos(angle);
            im += samples[n] * std::sin(angle);
        }
        return re * re + im * im;
    };
    // 9 kHz is exact bin 96. Its third, fourth, and fifth harmonics fold to
    // 21, 12, and 3 kHz (bins 224, 128, and 32): an explicit alias oracle.
    const double poly_alias = bin_energy(poly, 224) + bin_energy(poly, 128) +
                              bin_energy(poly, 32);
    const double naive_alias = bin_energy(naive, 224) + bin_energy(naive, 128) +
                               bin_energy(naive, 32);
    REQUIRE(poly_alias < naive_alias * 0.45);
}
