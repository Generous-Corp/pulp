#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/interaction_residual.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

// The interaction residual and the best-fit-gain null are measurement tools we
// point at real voices, so they are validated against synthetic voices whose
// state mechanism we chose on purpose. Every source here is built in this file
// — no plugin, no licence, no reference audio — so the checks run anywhere.

using pulp::test::audio::best_fit_gain_null;
using pulp::test::audio::Hit;
using pulp::test::audio::HitRenderFn;
using pulp::test::audio::measure_interaction_residual;
using pulp::test::audio::sweep_interaction_residual;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr double kFreqHz = 50.0;
constexpr double kT60Seconds = 0.5;

std::size_t frames_of(double seconds) {
    return static_cast<std::size_t>(seconds * kSampleRate);
}

// A struck resonator: two states rotated by w each sample and scaled toward
// silence, excited additively so a hit never clears what is already ringing.
//
// `nonlinearity` makes the rotation angle track the resonator's own
// instantaneous amplitude. That is the mechanism by which a still-ringing
// voice changes what the next hit sounds like, and the only reason a pair of
// hits is not the sum of two solos.
class StruckResonator {
public:
    StruckResonator(double freq_hz, double t60_seconds, double sample_rate, double nonlinearity)
        : w0_(2.0 * kPi * freq_hz / sample_rate),
          decay_(std::pow(10.0, -3.0 / (t60_seconds * sample_rate))),
          nonlinearity_(nonlinearity) {}

    void excite(float velocity) { s1_ += static_cast<double>(velocity); }

    float tick() {
        const double w = w0_ * (1.0 + nonlinearity_ * std::abs(s1_));
        const double c = std::cos(w);
        const double s = std::sin(w);
        const double n1 = decay_ * (c * s1_ - s * s2_);
        const double n2 = decay_ * (s * s1_ + c * s2_);
        s1_ = n1;
        s2_ = n2;
        return static_cast<float>(s1_);
    }

private:
    double w0_;
    double decay_;
    double nonlinearity_;
    double s1_ = 0.0;
    double s2_ = 0.0;
};

// One resonator for the whole render; every hit lands in the state the
// previous hit left behind.
HitRenderFn make_persistent_voice(double nonlinearity) {
    return [nonlinearity](const std::vector<Hit>& hits, std::size_t frames) {
        StruckResonator res(kFreqHz, kT60Seconds, kSampleRate, nonlinearity);
        std::vector<float> out(frames, 0.0f);
        std::size_t next = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            while (next < hits.size() && hits[next].frame == i) {
                res.excite(hits[next].velocity);
                ++next;
            }
            out[i] = res.tick();
        }
        return out;
    };
}

// One resonator per hit, summed — the classic note-on-allocates-a-voice model.
// Deliberately built with the *same* nonlinearity as the persistent voice:
// what changes is only that no hit can observe another's state.
HitRenderFn make_per_hit_voice(double nonlinearity) {
    return [nonlinearity](const std::vector<Hit>& hits, std::size_t frames) {
        std::vector<float> out(frames, 0.0f);
        for (const Hit& hit : hits) {
            StruckResonator res(kFreqHz, kT60Seconds, kSampleRate, nonlinearity);
            res.excite(hit.velocity);
            for (std::size_t i = hit.frame; i < frames; ++i) {
                out[i] += res.tick();
            }
        }
        return out;
    };
}

}  // namespace

TEST_CASE("persistent nonlinear voice leaves a residual that decays with the gap",
          "[interaction-residual]") {
    const HitRenderFn render = make_persistent_voice(0.5);
    const std::size_t frames = frames_of(3.0);
    const Hit first{frames_of(0.01), 1.0f};

    const std::size_t gaps[] = {frames_of(0.02), frames_of(0.1), frames_of(0.4), frames_of(2.0)};
    const auto sweep = sweep_interaction_residual(render, first, 1.0f, gaps, frames);

    REQUIRE(sweep.size() == 4);
    for (const auto& r : sweep) {
        INFO("gap_frames=" << r.gap_frames << " ratio=" << r.ratio);
        CHECK(r.pair_rms > 0.0);
    }

    // A hit into a loud ring is nothing like the same hit into silence.
    CHECK(sweep[0].ratio > 0.1);
    // What decays is the ring the second hit lands in, so the residual follows
    // the envelope rather than the gap itself. Adjacent gaps are compared only
    // across a decay's worth of separation: within a few tens of ms the ratio
    // is not monotone, because it also depends on where in the ring's cycle the
    // second hit arrives (see the phase-sensitivity case below).
    CHECK(sweep[0].ratio > sweep[1].ratio);
    CHECK(sweep[1].ratio > sweep[2].ratio);
    CHECK(sweep[2].ratio > sweep[3].ratio);
    // Past several T60s the first hit has nothing left to interact with, which
    // is the sweep's own negative control.
    CHECK(sweep[3].ratio < 1e-4);
}

TEST_CASE("interaction residual is phase-sensitive within a ring cycle",
          "[interaction-residual]") {
    // Pins a property that would otherwise look like a flaky assertion and
    // invite someone to "fix" it: the residual is not a monotone function of
    // gap. A hit landing on a peak of the ring interacts differently from one
    // landing near a zero crossing, so neighbouring gaps can trade places while
    // the envelope still decays over decades. Any threshold on this metric must
    // therefore be read across well-separated gaps, never between adjacent ones.
    const HitRenderFn render = make_persistent_voice(0.5);
    const std::size_t frames = frames_of(3.0);
    const Hit first{frames_of(0.01), 1.0f};

    const std::size_t gaps[] = {frames_of(0.02), frames_of(0.06)};
    const auto sweep = sweep_interaction_residual(render, first, 1.0f, gaps, frames);

    REQUIRE(sweep.size() == 2);
    INFO("ratio(20ms)=" << sweep[0].ratio << " ratio(60ms)=" << sweep[1].ratio);
    CHECK(sweep[0].ratio > 0.1);
    CHECK(sweep[1].ratio > 0.1);
    // Both gaps sit well inside one T60, so neither dominates.
    CHECK(sweep[1].ratio > sweep[0].ratio);
}

TEST_CASE("per-hit voice allocation nulls the residual exactly at every gap",
          "[interaction-residual]") {
    // The metric's own self-check. A voice that allocates per hit renders a
    // pair as the arithmetic sum of its solos, in the same order, with the same
    // rounding — so the residual must be bit-exact zero, not merely small. Any
    // nonzero value here means the metric is measuring the harness and every
    // number it reports about a real voice is suspect.
    const HitRenderFn render = make_per_hit_voice(0.5);
    const std::size_t frames = frames_of(3.0);
    const Hit first{frames_of(0.01), 1.0f};

    const std::size_t gaps[] = {frames_of(0.02), frames_of(0.06), frames_of(0.15),
                                frames_of(0.4),  frames_of(1.2)};
    const auto sweep = sweep_interaction_residual(render, first, 1.0f, gaps, frames);

    REQUIRE(sweep.size() == 5);
    for (const auto& r : sweep) {
        INFO("gap_frames=" << r.gap_frames);
        REQUIRE(r.pair_rms > 0.0);
        REQUIRE(!r.residual.empty());
        CHECK(r.residual_rms == 0.0);
        CHECK(r.ratio == 0.0);
        for (const float v : r.residual) {
            REQUIRE(v == 0.0f);
        }
    }
}

TEST_CASE("persistent but linear voice nulls the residual to rounding",
          "[interaction-residual]") {
    // Persistence alone is not what the residual detects. A linear resonator
    // obeys superposition however hot it is still ringing, so its pair render
    // matches the sum of its solos to floating-point noise. Only a voice that
    // is persistent *and* couples its own state back into its behaviour has a
    // residual to show.
    const HitRenderFn render = make_persistent_voice(0.0);
    const std::size_t frames = frames_of(3.0);
    const Hit first{frames_of(0.01), 1.0f};
    const Hit second{first.frame + frames_of(0.02), 1.0f};

    const auto r = measure_interaction_residual(render, first, second, frames);
    INFO("ratio=" << r.ratio);
    CHECK(r.pair_rms > 0.0);
    CHECK(r.ratio < 1e-5);
}

TEST_CASE("best-fit gain null clears a level-only difference", "[interaction-residual]") {
    // Two hits from a linear voice differ by exactly a gain, so handing the
    // gain-only hypothesis its optimum should leave nothing behind.
    const HitRenderFn render = make_per_hit_voice(0.0);
    const std::size_t frames = frames_of(1.0);

    const std::vector<float> loud = render({Hit{0, 1.0f}}, frames);
    const std::vector<float> quiet = render({Hit{0, 0.4f}}, frames);

    const auto null = best_fit_gain_null(loud, quiet);
    INFO("gain=" << null.gain << " residual_ratio=" << null.residual_ratio);
    CHECK(null.gain > 0.0);
    CHECK(null.gain == Catch::Approx(2.5).epsilon(0.01));
    CHECK(null.residual_ratio < 1e-5);
}

TEST_CASE("best-fit gain null survives a timbre-only difference", "[interaction-residual]") {
    // The same two velocities through the nonlinear voice ring at different
    // frequencies. No gain can turn one into the other, and the optimum is
    // allowed to prove it rather than being assumed.
    const HitRenderFn render = make_per_hit_voice(0.5);
    const std::size_t frames = frames_of(1.0);

    const std::vector<float> loud = render({Hit{0, 1.0f}}, frames);
    const std::vector<float> quiet = render({Hit{0, 0.4f}}, frames);

    const auto null = best_fit_gain_null(loud, quiet);
    INFO("gain=" << null.gain << " residual_ratio=" << null.residual_ratio);
    CHECK(null.residual_ratio > 0.5);
}
