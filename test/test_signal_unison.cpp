#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"
#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/supersaw.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <utility>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kAliasFundamental = 4100.0;
constexpr int kAliasFrames = 8192;
constexpr double kAudibleBandHz = 20000.0;
constexpr double kMinimumAliasImprovementDb = 8.0;

pulp::test::audio::AliasReport analyze_aliasing(const pulp::audio::Buffer<float>& signal) {
    AliasOptions options;
    options.num_harmonics = static_cast<int>(std::ceil(3.0 * kSampleRate / kAliasFundamental));
    options.analysis_length = static_cast<int>(signal.num_samples());
    options.max_alias_frequency_hz = kAudibleBandHz;
    return measure_aliasing(std::as_const(signal).view(), kAliasFundamental, kSampleRate, options);
}

} // namespace

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
    REQUIRE_FALSE(
        layout.configure({.voice_count = 2, .gain_law = static_cast<UnisonGainLaw>(255)}));
    REQUIRE_FALSE(layout.configure(
        {.voice_count = 2, .detune_cents = std::numeric_limits<double>::quiet_NaN()}));
    REQUIRE(layout.configure({.voice_count = 2, .phase_randomization = 1.0}, 42, 7));
    const auto phase = layout[1].phase;
    UnisonLayout<4> again;
    REQUIRE(again.configure({.voice_count = 2, .phase_randomization = 1.0}, 42, 7));
    REQUIRE(again[1].phase == phase);
    REQUIRE(layout.configure({.voice_count = 1, .phase_randomization = 1.0}, 999, 888));
    REQUIRE(layout[0].phase == 0.0);
}

TEST_CASE("Invalid unison configuration leaves the active layout unchanged", "[signal][unison]") {
    UnisonLayout<4> layout;
    const UnisonSpec valid{.voice_count = 3,
                           .detune_cents = 12.0,
                           .stereo_spread = 0.75,
                           .phase_randomization = 1.0,
                           .drift_cents = 2.0,
                           .drift_period_frames = 19};
    REQUIRE(layout.configure(valid, 41, 99));
    const auto before = layout[2];
    REQUIRE_FALSE(layout.configure(
        {.voice_count = 3, .phase_randomization = std::numeric_limits<double>::infinity()}));
    REQUIRE(layout.size() == valid.voice_count);
    REQUIRE(layout[2].detune_cents == before.detune_cents);
    REQUIRE(layout[2].pan == before.pan);
    REQUIRE(layout[2].gain == before.gain);
    REQUIRE(layout[2].phase == before.phase);
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

TEST_CASE("Supersaw reset and retrigger reproduce the same finite render", "[signal][supersaw]") {
    Supersaw saw;
    REQUIRE(saw.prepare(48000.0f, {.voice_count = 5,
                                   .detune_cents = 14.0,
                                   .stereo_spread = 0.6,
                                   .phase_randomization = 1.0,
                                   .drift_cents = 1.0,
                                   .drift_period_frames = 23}));
    REQUIRE(saw.trigger(330.0f, 123, 456));
    std::array<StereoSampleT<float>, 96> first{};
    for (std::uint64_t frame = 0; frame < first.size(); ++frame) {
        first[frame] = saw.next(frame);
        REQUIRE(std::isfinite(first[frame].left));
        REQUIRE(std::isfinite(first[frame].right));
    }
    saw.reset();
    REQUIRE(saw.trigger(330.0f, 123, 456));
    for (std::uint64_t frame = 0; frame < first.size(); ++frame) {
        const auto replay = saw.next(frame);
        REQUIRE(replay.left == first[frame].left);
        REQUIRE(replay.right == first[frame].right);
    }
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

TEST_CASE("PolyBLEP supersaw reduces measured audible-band alias energy",
          "[signal][supersaw][alias]") {
    Supersaw saw;
    REQUIRE(saw.prepare(static_cast<float>(kSampleRate), {.voice_count = 1}));
    REQUIRE(saw.trigger(static_cast<float>(kAliasFundamental), 0, 0));
    pulp::audio::Buffer<float> poly(1, kAliasFrames);
    pulp::audio::Buffer<float> naive(1, kAliasFrames);
    double naive_phase = 0.0;
    for (std::uint64_t frame = 0; frame < kAliasFrames; ++frame) {
        poly.channel(0)[static_cast<int>(frame)] =
            saw.next(frame).left * static_cast<float>(std::sqrt(2.0));
        naive.channel(0)[static_cast<int>(frame)] = static_cast<float>(2.0 * naive_phase - 1.0);
        naive_phase += kAliasFundamental / kSampleRate;
        if (naive_phase >= 1.0) naive_phase -= 1.0;
    }

    const auto poly_alias = analyze_aliasing(poly);
    const auto naive_alias = analyze_aliasing(naive);
    INFO("PolyBLEP worst alias " << poly_alias.worst_alias_db << " dB at "
                                 << poly_alias.worst_alias_hz << " Hz");
    INFO("Naive worst alias " << naive_alias.worst_alias_db << " dB at "
                              << naive_alias.worst_alias_hz << " Hz");
    REQUIRE_FALSE(poly_alias.has_unresolved_in_band_alias);
    REQUIRE_FALSE(naive_alias.has_unresolved_in_band_alias);
    REQUIRE(poly_alias.worst_alias_index != 0);
    REQUIRE(naive_alias.worst_alias_index != 0);
    REQUIRE(poly_alias.worst_alias_db > poly_alias.detection_floor_db);
    REQUIRE(naive_alias.worst_alias_db > naive_alias.detection_floor_db);
    REQUIRE(poly_alias.worst_alias_db <= naive_alias.worst_alias_db - kMinimumAliasImprovementDb);
}
