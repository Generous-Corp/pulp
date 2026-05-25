#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/panner.hpp>

#include <cmath>

using namespace pulp::signal;

namespace {

// Compute the (dry, wet) gains exposed by DryWetMixer through a process call.
// We replicate the gain calculation here for test purposes by running a single
// constant sample through the mixer.
struct MixGains { float dry; float wet; };
MixGains gains(MixCurve curve, float mix) {
    DryWetMixer mx;
    mx.prepare(/*channels*/ 1, /*block*/ 1);
    mx.set_curve(curve);
    mx.set_mix(mix);
    const float dry_sample = 1.0f;
    float wet_sample = 1.0f;
    float* wet_ptr = &wet_sample;
    const float* dry_ptr = &dry_sample;
    const float* const* dry_in = &dry_ptr;
    float* const* wet_out = &wet_ptr;
    mx.push_dry(dry_in, 1, 1);
    mx.mix_wet(wet_out, 1, 1);
    // wet_sample now contains dry_gain * 1 + wet_gain * 1 = dry + wet.
    // We need them separately — easier to just trigger two pushes.
    DryWetMixer mx2;
    mx2.prepare(1, 1);
    mx2.set_curve(curve);
    mx2.set_mix(mix);
    float dry2 = 1.0f;
    float wet2 = 0.0f;
    float* wp = &wet2;
    const float* dp = &dry2;
    mx2.push_dry(&dp, 1, 1);
    float* const* wo = &wp;
    mx2.mix_wet(wo, 1, 1);
    const float dry_gain = wet2; // wet was 0, so result == dry_gain * 1
    DryWetMixer mx3;
    mx3.prepare(1, 1);
    mx3.set_curve(curve);
    mx3.set_mix(mix);
    float dry3 = 0.0f;
    float wet3 = 1.0f;
    float* wp3 = &wet3;
    const float* dp3 = &dry3;
    mx3.push_dry(&dp3, 1, 1);
    float* const* wo3 = &wp3;
    mx3.mix_wet(wo3, 1, 1);
    const float wet_gain = wet3; // dry was 0, so result == wet_gain * 1
    return {dry_gain, wet_gain};
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
// DryWetMixer curves
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("DryWetMixer Linear curve is constant amplitude",
          "[signal][mix-laws]") {
    auto g = gains(MixCurve::Linear, 0.5f);
    REQUIRE(std::abs(g.dry - 0.5f) < 1e-6f);
    REQUIRE(std::abs(g.wet - 0.5f) < 1e-6f);
    REQUIRE(std::abs(g.dry + g.wet - 1.0f) < 1e-6f);
}

TEST_CASE("DryWetMixer EqualPower / Sin3dB is constant power",
          "[signal][mix-laws]") {
    for (MixCurve c : {MixCurve::EqualPower, MixCurve::Sin3dB}) {
        for (float m : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            auto g = gains(c, m);
            const float power = g.dry * g.dry + g.wet * g.wet;
            REQUIRE(std::abs(power - 1.0f) < 1e-5f);
        }
        auto half = gains(c, 0.5f);
        // -3 dB at midpoint → ~ 0.707
        REQUIRE(std::abs(half.dry - 0.7071068f) < 1e-4f);
        REQUIRE(std::abs(half.wet - 0.7071068f) < 1e-4f);
    }
}

TEST_CASE("DryWetMixer Balanced keeps dry full below midpoint",
          "[signal][mix-laws]") {
    auto below = gains(MixCurve::Balanced, 0.25f);
    REQUIRE(below.dry == 1.0f);
    REQUIRE(std::abs(below.wet - 0.5f) < 1e-6f);
    auto above = gains(MixCurve::Balanced, 0.75f);
    REQUIRE(above.wet == 1.0f);
    REQUIRE(std::abs(above.dry - 0.5f) < 1e-6f);
}

TEST_CASE("DryWetMixer endpoints reach full dry / full wet for every curve",
          "[signal][mix-laws]") {
    for (MixCurve c : {MixCurve::Linear, MixCurve::EqualPower, MixCurve::Balanced,
                       MixCurve::Sin3dB, MixCurve::Sin4_5dB, MixCurve::Sin6dB,
                       MixCurve::Sqrt3dB, MixCurve::Sqrt4_5dB}) {
        auto at0 = gains(c, 0.0f);
        auto at1 = gains(c, 1.0f);
        REQUIRE(std::abs(at0.dry - 1.0f) < 1e-5f);
        REQUIRE(std::abs(at0.wet - 0.0f) < 1e-5f);
        REQUIRE(std::abs(at1.dry - 0.0f) < 1e-5f);
        REQUIRE(std::abs(at1.wet - 1.0f) < 1e-5f);
    }
}

TEST_CASE("DryWetMixer Sin6dB has -6dB notch at midpoint", "[signal][mix-laws]") {
    auto half = gains(MixCurve::Sin6dB, 0.5f);
    // cos(pi/4)^2 = 0.5 → -6 dB
    REQUIRE(std::abs(half.dry - 0.5f) < 1e-4f);
    REQUIRE(std::abs(half.wet - 0.5f) < 1e-4f);
}

TEST_CASE("DryWetMixer Sqrt4_5dB has -4.5dB notch at midpoint (Codex #2834 P2 regression)",
          "[signal][mix-laws][regression]") {
    auto half = gains(MixCurve::Sqrt4_5dB, 0.5f);
    // 0.5^0.75 ≈ 0.5946, 20*log10(0.5946) ≈ -4.51 dB.
    REQUIRE(std::abs(half.dry - 0.594603f) < 1e-3f);
    REQUIRE(std::abs(half.wet - 0.594603f) < 1e-3f);
}

// ────────────────────────────────────────────────────────────────────────
// Panner laws
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("Panner Linear law adds to 1", "[signal][pan-law]") {
    Panner p;
    p.set_law(PanLaw::Linear);
    for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        p.set_pan(pan);
        float l, r;
        p.compute_gains(l, r);
        REQUIRE(std::abs(l + r - 1.0f) < 1e-6f);
    }
}

TEST_CASE("Panner Sin3dB / EqualPower constant-power", "[signal][pan-law]") {
    Panner p;
    p.set_law(PanLaw::Sin3dB);
    for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        p.set_pan(pan);
        float l, r;
        p.compute_gains(l, r);
        const float power = l * l + r * r;
        REQUIRE(std::abs(power - 1.0f) < 1e-5f);
    }
    // -3 dB at centre.
    p.set_pan(0.0f);
    float l, r;
    p.compute_gains(l, r);
    REQUIRE(std::abs(l - 0.7071068f) < 1e-4f);
    REQUIRE(std::abs(r - 0.7071068f) < 1e-4f);
}

TEST_CASE("Panner Balanced stays at 1 in the same-side half", "[signal][pan-law]") {
    Panner p;
    p.set_law(PanLaw::Balanced);
    p.set_pan(-1.0f);
    float l, r;
    p.compute_gains(l, r);
    REQUIRE(l == 1.0f);
    REQUIRE(r == 0.0f);
    p.set_pan(0.0f);
    p.compute_gains(l, r);
    REQUIRE(l == 1.0f);
    REQUIRE(r == 1.0f);
    p.set_pan(1.0f);
    p.compute_gains(l, r);
    REQUIRE(l == 0.0f);
    REQUIRE(r == 1.0f);
}

TEST_CASE("Panner endpoints reach hard-left / hard-right for every law",
          "[signal][pan-law]") {
    for (PanLaw law : {PanLaw::Linear, PanLaw::Sin3dB, PanLaw::Sin4_5dB,
                       PanLaw::Sin6dB, PanLaw::Sqrt3dB, PanLaw::Sqrt4_5dB}) {
        Panner p;
        p.set_law(law);
        p.set_pan(-1.0f);
        float l, r;
        p.compute_gains(l, r);
        REQUIRE(std::abs(l - 1.0f) < 1e-5f);
        REQUIRE(std::abs(r - 0.0f) < 1e-5f);
        p.set_pan(1.0f);
        p.compute_gains(l, r);
        REQUIRE(std::abs(l - 0.0f) < 1e-5f);
        REQUIRE(std::abs(r - 1.0f) < 1e-5f);
    }
}

TEST_CASE("Panner Sin6dB has -6dB notch at centre", "[signal][pan-law]") {
    Panner p;
    p.set_law(PanLaw::Sin6dB);
    p.set_pan(0.0f);
    float l, r;
    p.compute_gains(l, r);
    REQUIRE(std::abs(l - 0.5f) < 1e-4f);
    REQUIRE(std::abs(r - 0.5f) < 1e-4f);
}

TEST_CASE("Panner Sqrt4_5dB has -4.5dB notch at centre (Codex #2834 P2 regression)",
          "[signal][pan-law][regression]") {
    Panner p;
    p.set_law(PanLaw::Sqrt4_5dB);
    p.set_pan(0.0f);
    float l, r;
    p.compute_gains(l, r);
    REQUIRE(std::abs(l - 0.594603f) < 1e-3f);
    REQUIRE(std::abs(r - 0.594603f) < 1e-3f);
}
