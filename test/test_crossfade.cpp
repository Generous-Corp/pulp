// SF-2 crossfade unification — the ONE fixture covering every SIGNAL-side fade.
//
// After SF-2 there is a single old->new crossfade law (signal::crossfade_gains
// over an optional signal::crossfade_smoothstep shaping). This fixture proves:
//   1. the shared law's invariants (both gain laws + the smoothstep ramp);
//   2. signal::TransitionMixer (the live plugin swap + convolver IR swap) blends
//      through it — float AND double, both curves;
//   3. the live_kernel structural-swap fade blends through it (matching native);
//   4. the audio LoopRenderer wrap-crossfade blends through it.
// The PartitionedConvolver IR-swap fade blends via signal::TransitionMixer (see
// §2 + test_convolver_bg_swap), so it shares the identical law by construction.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>

#include <live_kernel/crossfade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::CrossfadeGainLaw;
using pulp::signal::crossfade_gains;
using pulp::signal::crossfade_smoothstep;
using pulp::signal::TransitionCurve;
using pulp::signal::TransitionMixer;
using pulp::signal::TransitionMixer64;

// ── §1 — the shared law's invariants ────────────────────────────────────────

TEST_CASE("crossfade: smoothstep ramp is click-free (zero slope, symmetric)",
          "[signal][crossfade][sf2]") {
    REQUIRE(crossfade_smoothstep(0.0) == 0.0);
    REQUIRE(crossfade_smoothstep(1.0) == 1.0);
    REQUIRE_THAT(crossfade_smoothstep(0.5), WithinAbs(0.5, 1e-15));
    // Clamped outside [0,1].
    REQUIRE(crossfade_smoothstep(-0.3) == 0.0);
    REQUIRE(crossfade_smoothstep(1.7) == 1.0);
    // Symmetric: s(t) + s(1-t) == 1.
    for (double t = 0.0; t <= 1.0; t += 0.05) {
        REQUIRE_THAT(crossfade_smoothstep(t) + crossfade_smoothstep(1.0 - t),
                     WithinAbs(1.0, 1e-12));
    }
    // Zero slope at the ends (finite-difference derivative -> 0).
    const double h = 1e-6;
    REQUIRE(std::abs(crossfade_smoothstep(h) - crossfade_smoothstep(0.0)) / h < 1e-4);
    REQUIRE(std::abs(crossfade_smoothstep(1.0) - crossfade_smoothstep(1.0 - h)) / h < 1e-4);
}

TEST_CASE("crossfade: gain laws hit the right sums and endpoints",
          "[signal][crossfade][sf2]") {
    for (double u = 0.0; u <= 1.0; u += 0.05) {
        double og = 0.0, ng = 0.0;
        crossfade_gains(u, CrossfadeGainLaw::EqualGain, og, ng);
        REQUIRE_THAT(og + ng, WithinAbs(1.0, 1e-15));      // amplitude sum == 1
        crossfade_gains(u, CrossfadeGainLaw::EqualPower, og, ng);
        REQUIRE_THAT(og * og + ng * ng, WithinAbs(1.0, 1e-12));  // power sum == 1
    }
    // Endpoints: fully old at u=0, fully new at u=1.
    double og = 0.0, ng = 0.0;
    crossfade_gains(0.0, CrossfadeGainLaw::EqualPower, og, ng);
    REQUIRE_THAT(og, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(ng, WithinAbs(0.0, 1e-12));
    crossfade_gains(1.0, CrossfadeGainLaw::EqualPower, og, ng);
    REQUIRE_THAT(og, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(ng, WithinAbs(1.0, 1e-12));
}

// ── §2 — TransitionMixer routes through the shared law ──────────────────────

TEST_CASE("crossfade: TransitionMixer gains equal the shared law (both curves)",
          "[signal][crossfade][sf2]") {
    const std::size_t len = 257;  // odd, so the midpoint is not a special case
    for (auto curve : {TransitionCurve::Smoothstep, TransitionCurve::EqualPower}) {
        const auto law = curve == TransitionCurve::EqualPower
                             ? CrossfadeGainLaw::EqualPower
                             : CrossfadeGainLaw::EqualGain;
        TransitionMixer mf;
        mf.configure(len, curve);
        TransitionMixer64 md;
        md.configure(len, curve);
        for (std::size_t p = 0; p <= len + 8; ++p) {
            float mof = 0.0f, mnf = 0.0f;
            mf.gains_at(p, mof, mnf);
            float sof = 0.0f, snf = 0.0f;
            crossfade_gains(crossfade_smoothstep(static_cast<float>(p) /
                                                 static_cast<float>(len)),
                            law, sof, snf);
            REQUIRE(mof == sof);  // bit-exact: the mixer runs the shared law
            REQUIRE(mnf == snf);

            double mod = 0.0, mnd = 0.0;
            md.gains_at(p, mod, mnd);
            double sod = 0.0, snd = 0.0;
            crossfade_gains(crossfade_smoothstep(static_cast<double>(p) /
                                                 static_cast<double>(len)),
                            law, sod, snd);
            REQUIRE(mod == sod);
            REQUIRE(mnd == snd);
        }
    }
}

// ── §3 — live_kernel structural swap routes through the shared law ──────────

TEST_CASE("crossfade: live_kernel fade equals the shared smoothstep equal-power law",
          "[signal][crossfade][sf2][live_kernel]") {
    const int fade_len = 480;
    const int n = 300;
    std::vector<float> ob(static_cast<std::size_t>(n), 1.0f);
    std::vector<float> nb(static_cast<std::size_t>(n), -1.0f);
    std::vector<float> got(static_cast<std::size_t>(n));
    // Straddle the fade end so the clamp region is exercised too.
    const int fade_pos = fade_len - 150;
    pulp::live_kernel::equal_power_fade_block(got.data(), ob.data(), nb.data(), n,
                                              fade_pos, fade_len);
    for (int i = 0; i < n; ++i) {
        float go = 0.0f, gn = 0.0f;
        crossfade_gains(crossfade_smoothstep(static_cast<float>(fade_pos + i) /
                                             static_cast<float>(fade_len)),
                        CrossfadeGainLaw::EqualPower, go, gn);
        const float expected = ob[static_cast<std::size_t>(i)] * go +
                               nb[static_cast<std::size_t>(i)] * gn;
        REQUIRE(got[static_cast<std::size_t>(i)] == expected);
    }
}

// ── §4 — LoopRenderer wrap-crossfade routes through the shared law ──────────

TEST_CASE("crossfade: LoopRenderer wrap blend equals the shared (raw-ramp) law",
          "[signal][crossfade][sf2][loop]") {
    using pulp::audio::Buffer;
    using pulp::audio::BufferView;
    using pulp::audio::LoopCrossfadeCurve;
    using pulp::audio::LoopInterpolationMode;
    using pulp::audio::LoopPlaybackMode;
    using pulp::audio::LoopReader;
    using pulp::audio::LoopRegion;
    using pulp::audio::LoopRenderer;

    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kSourceFrames = 1100;
    constexpr std::uint64_t kStart = 100;
    constexpr std::uint64_t kEnd = 1000;
    constexpr std::uint32_t kXfade = 64;

    Buffer<float> source(kChannels, kSourceFrames);
    for (std::size_t i = 0; i < kSourceFrames; ++i) {
        source.channel(0)[i] = std::sin(0.017f * static_cast<float>(i)) * 0.9f;
        source.channel(1)[i] = std::cos(0.023f * static_cast<float>(i) + 0.5f) * 0.7f;
    }
    std::vector<const float*> ptrs(kChannels);
    for (std::size_t ch = 0; ch < kChannels; ++ch) ptrs[ch] = source.channel(ch).data();
    const BufferView<const float> input(ptrs.data(), kChannels, kSourceFrames);

    // The shared-law oracle for a forward loop's wrap crossfade: the loop uses
    // the RAW ramp (clamped t, no smoothstep), so the oracle shapes nothing.
    auto oracle = [&](const LoopRegion& region, std::uint32_t ch, double position) {
        const auto crossfade = static_cast<double>(region.crossfade_frames);
        const auto start = static_cast<double>(region.start_frame);
        const auto end = static_cast<double>(region.end_frame);
        const auto norm = LoopReader::normalize_position(region, position);
        const auto law = region.crossfade_curve == LoopCrossfadeCurve::EqualPower
                             ? CrossfadeGainLaw::EqualPower
                             : CrossfadeGainLaw::EqualGain;
        auto blended = [&](double t, double wrapped_pos) {
            const double u = std::clamp(t, 0.0, 1.0);
            double og = 0.0, ng = 0.0;
            crossfade_gains(u, law, og, ng);
            const double a = LoopReader::read_validated(input, region, ch, norm);
            const double b = LoopReader::read_validated(input, region, ch, wrapped_pos);
            return static_cast<float>(a * og + b * ng);
        };
        if (norm >= end - crossfade)
            return blended((norm - (end - crossfade)) / crossfade,
                           start + (norm - (end - crossfade)));
        return LoopReader::read_validated(input, region, ch, norm);
    };

    for (auto curve : {LoopCrossfadeCurve::EqualPower, LoopCrossfadeCurve::Linear}) {
        LoopRegion region;
        region.start_frame = kStart;
        region.end_frame = kEnd;
        region.crossfade_frames = kXfade;
        region.crossfade_curve = curve;
        region.playback_mode = LoopPlaybackMode::Forward;
        region.reverse_entry = false;
        region.interpolation = LoopInterpolationMode::None;
        region.source_sample_rate = 48000.0;

        LoopRenderer renderer;
        REQUIRE(renderer.set_region(region, kSourceFrames));
        renderer.set_playback_rate(1.0);
        renderer.start();

        Buffer<float> frame_out(kChannels, 1);
        bool wrap_seen = false;
        for (int f = 0; f < 1000; ++f) {
            const double pos_before = renderer.position();
            renderer.render(input, frame_out.view(), 1);
            for (std::uint32_t ch = 0; ch < kChannels; ++ch)
                REQUIRE(frame_out.channel(ch)[0] == oracle(region, ch, pos_before));
            const double norm = LoopReader::normalize_position(region, pos_before);
            if (norm >= static_cast<double>(kEnd) - static_cast<double>(kXfade))
                wrap_seen = true;
        }
        REQUIRE(wrap_seen);  // the crossfade region was actually exercised
    }
}
