#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/processor_duplicator.hpp>
#include <pulp/signal/matrix.hpp>
#include <pulp/signal/special_functions.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/fast_math.hpp>
#include <vector>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

TEST_CASE("FastMath clamps saturation helper boundaries",
          "[dsp][fast_math][coverage][phase3-large]") {
    REQUIRE_THAT(FastMath::clamp_unit(-2.0f), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(FastMath::clamp_unit(-0.25f), WithinAbs(-0.25f, 1e-6f));
    REQUIRE_THAT(FastMath::clamp_unit(0.25f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(FastMath::clamp_unit(2.0f), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("FastMath soft clip covers linear and saturated regions",
          "[dsp][fast_math][coverage][phase3-large]") {
    REQUIRE_THAT(FastMath::soft_clip(-2.0f), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(FastMath::soft_clip(2.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(FastMath::soft_clip(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(FastMath::soft_clip(0.75f), WithinAbs(0.6875f, 1e-6f));
    REQUIRE_THAT(FastMath::soft_clip(-0.75f), WithinAbs(-0.6875f, 1e-6f));
}

TEST_CASE("FastMath dB conversion covers silent and unity cases",
          "[dsp][fast_math][coverage][phase3-large]") {
    REQUIRE_THAT(FastMath::db_to_gain(0.0f), WithinAbs(1.0f, 1e-5f));
    REQUIRE(FastMath::gain_to_db(0.0f) == -200.0f);
    REQUIRE(FastMath::gain_to_db(-1.0f) == -200.0f);
    REQUIRE_THAT(FastMath::gain_to_db(1.0f), WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("FastMath tanh covers clamps and odd symmetry",
          "[dsp][fast_math][coverage][phase3-large]") {
    REQUIRE_THAT(FastMath::tanh(-5.0f), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(FastMath::tanh(5.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(FastMath::tanh(0.0f), WithinAbs(0.0f, 1e-6f));

    const auto positive = FastMath::tanh(0.75f);
    const auto negative = FastMath::tanh(-0.75f);
    REQUIRE_THAT(negative, WithinAbs(-positive, 1e-6f));
}

TEST_CASE("FastMath pow and reciprocal helpers cover scalar edges",
          "[dsp][fast_math][coverage][phase3-large]") {
    REQUIRE_THAT(FastMath::pow(0.0f, 2.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(FastMath::pow(-2.0f, 3.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(FastMath::pow(4.0f, 0.5f), WithinAbs(2.0f, 0.02f));

    REQUIRE_THAT(FastMath::rcp(4.0f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(FastMath::rsqrt(4.0f), WithinAbs(0.5f, 0.001f));
}

// ── DryWetMixer ─────────────────────────────────────────────────────────

TEST_CASE("DryWetMixer fully wet", "[dsp][dry_wet]") {
    DryWetMixer mixer;
    mixer.set_mix(1.0f);
    mixer.prepare(1, 256);

    float dry[] = {0.5f, 0.5f, 0.5f, 0.5f};
    float wet[] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float* dry_ptrs[] = {dry};
    float* wet_ptrs[] = {wet};

    mixer.push_dry(dry_ptrs, 1, 4);
    mixer.mix_wet(wet_ptrs, 1, 4);

    for (int i = 0; i < 4; ++i)
        REQUIRE_THAT(wet[i], WithinAbs(1.0f, 1e-5));
}

TEST_CASE("DryWetMixer fully dry", "[dsp][dry_wet]") {
    DryWetMixer mixer;
    mixer.set_mix(0.0f);
    mixer.prepare(1, 256);

    float dry[] = {0.5f, 0.5f};
    float wet[] = {1.0f, 1.0f};
    const float* dry_ptrs[] = {dry};
    float* wet_ptrs[] = {wet};

    mixer.push_dry(dry_ptrs, 1, 2);
    mixer.mix_wet(wet_ptrs, 1, 2);

    for (int i = 0; i < 2; ++i)
        REQUIRE_THAT(wet[i], WithinAbs(0.5f, 1e-5));
}

TEST_CASE("DryWetMixer 50/50 linear", "[dsp][dry_wet]") {
    DryWetMixer mixer;
    mixer.set_mix(0.5f);
    mixer.set_curve(MixCurve::Linear);
    mixer.prepare(1, 256);

    float dry[] = {0.0f};
    float wet[] = {1.0f};
    const float* dry_ptrs[] = {dry};
    float* wet_ptrs[] = {wet};

    mixer.push_dry(dry_ptrs, 1, 1);
    mixer.mix_wet(wet_ptrs, 1, 1);

    REQUIRE_THAT(wet[0], WithinAbs(0.5f, 1e-5));
}

TEST_CASE("DryWetMixer clamps mix and applies equal-power curve", "[dsp][dry_wet][issue-645]") {
    DryWetMixer mixer;
    mixer.set_mix(-0.5f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(0.0f, 1e-6f));
    mixer.set_mix(2.0f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(1.0f, 1e-6f));

    mixer.set_mix(0.5f);
    mixer.set_curve(MixCurve::EqualPower);
    mixer.prepare(1, 16);

    float dry[] = {1.0f};
    float wet[] = {0.0f};
    const float* dry_ptrs[] = {dry};
    float* wet_ptrs[] = {wet};

    mixer.push_dry(dry_ptrs, 1, 1);
    mixer.mix_wet(wet_ptrs, 1, 1);

    REQUIRE_THAT(wet[0], WithinAbs(0.7071068f, 1e-4f));
}

TEST_CASE("DryWetMixer latency delays dry path and reset clears history", "[dsp][dry_wet][issue-645]") {
    DryWetMixer mixer;
    mixer.set_mix(0.0f);
    mixer.set_wet_latency(2);
    mixer.prepare(2, 8);

    float dry_l[] = {1.0f, 2.0f, 3.0f};
    float dry_r[] = {10.0f, 20.0f, 30.0f};
    float wet_l[] = {100.0f, 100.0f, 100.0f};
    float wet_r[] = {200.0f, 200.0f, 200.0f};
    const float* dry_ptrs[] = {dry_l, dry_r};
    float* wet_ptrs[] = {wet_l, wet_r};

    mixer.push_dry(dry_ptrs, 2, 3);
    mixer.mix_wet(wet_ptrs, 2, 3);

    REQUIRE_THAT(wet_l[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_l[1], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_l[2], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(wet_r[2], WithinAbs(10.0f, 1e-6f));

    mixer.reset();
    float wet_after_reset[] = {7.0f, 7.0f};
    float* reset_ptrs[] = {wet_after_reset};
    mixer.push_dry(dry_ptrs, 1, 2);
    mixer.mix_wet(reset_ptrs, 1, 2);

    REQUIRE_THAT(wet_after_reset[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_after_reset[1], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("DryWetMixer can enable latency after prepare and leaves unmatched wet channels",
          "[dsp][dry_wet][codecov]") {
    DryWetMixer mixer;
    mixer.prepare(2, 4);
    mixer.set_wet_latency(1);
    mixer.set_mix(0.0f);

    float dry_l[] = {3.0f, 4.0f};
    float wet_l[] = {10.0f, 10.0f};
    float wet_r[] = {20.0f, 20.0f};
    const float* dry_ptrs[] = {dry_l};
    float* wet_ptrs[] = {wet_l, wet_r};

    mixer.push_dry(dry_ptrs, 1, 2);
    mixer.mix_wet(wet_ptrs, 2, 2);

    REQUIRE_THAT(wet_l[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_l[1], WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(wet_r[0], WithinAbs(20.0f, 1e-6f));
    REQUIRE_THAT(wet_r[1], WithinAbs(20.0f, 1e-6f));
}

TEST_CASE("DryWetMixer retuning latency clears stale delay history",
          "[dsp][dry_wet][codecov]") {
    DryWetMixer mixer;
    mixer.set_mix(0.0f);
    mixer.set_wet_latency(2);
    mixer.prepare(1, 4);

    float dry_a[] = {1.0f, 2.0f, 3.0f};
    float wet_a[] = {100.0f, 100.0f, 100.0f};
    const float* dry_a_ptrs[] = {dry_a};
    float* wet_a_ptrs[] = {wet_a};
    mixer.push_dry(dry_a_ptrs, 1, 3);
    mixer.mix_wet(wet_a_ptrs, 1, 3);
    REQUIRE_THAT(wet_a[2], WithinAbs(1.0f, 1e-6f));

    mixer.set_wet_latency(1);

    float dry_b[] = {9.0f, 10.0f};
    float wet_b[] = {200.0f, 200.0f};
    const float* dry_b_ptrs[] = {dry_b};
    float* wet_b_ptrs[] = {wet_b};
    mixer.push_dry(dry_b_ptrs, 1, 2);
    mixer.mix_wet(wet_b_ptrs, 1, 2);

    REQUIRE_THAT(wet_b[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_b[1], WithinAbs(9.0f, 1e-6f));

    mixer.set_wet_latency(0);

    float dry_c[] = {12.0f};
    float wet_c[] = {300.0f};
    const float* dry_c_ptrs[] = {dry_c};
    float* wet_c_ptrs[] = {wet_c};
    mixer.push_dry(dry_c_ptrs, 1, 1);
    mixer.mix_wet(wet_c_ptrs, 1, 1);
    REQUIRE_THAT(wet_c[0], WithinAbs(12.0f, 1e-6f));
}

TEST_CASE("DryWetMixer handles nonpositive and over-channel latency edges",
          "[dsp][dry_wet][codecov]") {
    DryWetMixer mixer;
    mixer.set_mix(0.0f);
    mixer.set_wet_latency(-8);
    mixer.prepare(1, 4);

    float dry_l[] = {1.0f, 2.0f};
    float dry_r[] = {3.0f, 4.0f};
    float wet_l[] = {10.0f, 10.0f, 10.0f};
    float wet_r[] = {20.0f, 20.0f, 20.0f};
    const float* dry_ptrs[] = {dry_l, dry_r};
    float* wet_ptrs[] = {wet_l, wet_r};

    mixer.push_dry(dry_ptrs, 2, 2);
    mixer.mix_wet(wet_ptrs, 2, 3);
    REQUIRE_THAT(wet_l[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(wet_l[1], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(wet_l[2], WithinAbs(10.0f, 1e-6f));
    REQUIRE_THAT(wet_r[0], WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(wet_r[1], WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(wet_r[2], WithinAbs(20.0f, 1e-6f));

    mixer.set_wet_latency(1);
    mixer.prepare(1, 4);

    float wet_l_limited[] = {30.0f, 30.0f};
    float wet_r_limited[] = {40.0f, 40.0f};
    float* limited_ptrs[] = {wet_l_limited, wet_r_limited};
    mixer.push_dry(dry_ptrs, 2, 2);
    mixer.mix_wet(limited_ptrs, 2, 2);

    REQUIRE_THAT(wet_l_limited[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(wet_l_limited[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(wet_r_limited[0], WithinAbs(40.0f, 1e-6f));
    REQUIRE_THAT(wet_r_limited[1], WithinAbs(40.0f, 1e-6f));

    mixer.prepare(0, 4);
    mixer.push_dry(dry_ptrs, 1, 2);
    float wet_zero_channels[] = {50.0f, 50.0f};
    float* zero_channel_ptrs[] = {wet_zero_channels};
    mixer.mix_wet(zero_channel_ptrs, 1, 2);
    REQUIRE_THAT(wet_zero_channels[0], WithinAbs(50.0f, 1e-6f));
    REQUIRE_THAT(wet_zero_channels[1], WithinAbs(50.0f, 1e-6f));

    mixer.push_dry(nullptr, 1, 2);
    mixer.mix_wet(zero_channel_ptrs, 1, 2);
    REQUIRE_THAT(wet_zero_channels[0], WithinAbs(50.0f, 1e-6f));
    REQUIRE_THAT(wet_zero_channels[1], WithinAbs(50.0f, 1e-6f));
}

// ── ProcessorDuplicator ─────────────────────────────────────────────────

// Simple gain processor for testing
struct TestGain {
    float gain = 1.0f;
    void set_sample_rate(float) {}
    float process(float x) { return x * gain; }
    void reset() {}
};

struct StatefulGain {
    float gain = 1.0f;
    float sample_rate = 0.0f;
    bool reset_called = false;

    void set_sample_rate(float sr) { sample_rate = sr; }
    float process(float x) { return x * gain; }
    void reset() { reset_called = true; }
};

TEST_CASE("ProcessorDuplicator applies to all channels", "[dsp][duplicator]") {
    ProcessorDuplicator<TestGain> dup;
    dup.prepare(2, 44100.0f);
    dup.for_each([](TestGain& g) { g.gain = 0.5f; });

    float ch0[] = {1.0f, 2.0f, 3.0f};
    float ch1[] = {4.0f, 5.0f, 6.0f};
    float* channels[] = {ch0, ch1};

    dup.process(channels, 2, 3);

    REQUIRE_THAT(ch0[0], WithinAbs(0.5f, 1e-5));
    REQUIRE_THAT(ch1[2], WithinAbs(3.0f, 1e-5));
}

TEST_CASE("ProcessorDuplicator per-channel params", "[dsp][duplicator]") {
    ProcessorDuplicator<TestGain> dup;
    dup.prepare(2, 44100.0f);
    dup[0].gain = 2.0f;
    dup[1].gain = 0.5f;

    float ch0[] = {1.0f};
    float ch1[] = {1.0f};
    float* channels[] = {ch0, ch1};

    dup.process(channels, 2, 1);

    REQUIRE_THAT(ch0[0], WithinAbs(2.0f, 1e-5));
    REQUIRE_THAT(ch1[0], WithinAbs(0.5f, 1e-5));
}

TEST_CASE("ProcessorDuplicator bounds channels and exposes channel state",
          "[dsp][duplicator][issue-645]") {
    ProcessorDuplicator<StatefulGain> dup;
    dup.prepare(1, 48000.0f);
    dup[0].gain = 3.0f;

    float ch0[] = {1.0f, -2.0f};
    float ch1[] = {10.0f, 20.0f};
    float* channels[] = {ch0, ch1};

    dup.process(channels, 2, 2);

    REQUIRE(dup.num_channels() == 1);
    REQUIRE_THAT(ch0[0], WithinAbs(3.0f, 1e-5));
    REQUIRE_THAT(ch0[1], WithinAbs(-6.0f, 1e-5));
    REQUIRE_THAT(ch1[0], WithinAbs(10.0f, 1e-5));
    REQUIRE_THAT(ch1[1], WithinAbs(20.0f, 1e-5));

    dup.process_channel(ch1, -1, 2);
    dup.process_channel(ch1, 1, 2);
    REQUIRE_THAT(ch1[0], WithinAbs(10.0f, 1e-5));
    REQUIRE_THAT(ch1[1], WithinAbs(20.0f, 1e-5));

    dup.process_channel(ch1, 0, 2);
    REQUIRE_THAT(ch1[0], WithinAbs(30.0f, 1e-5));
    REQUIRE_THAT(ch1[1], WithinAbs(60.0f, 1e-5));

    const auto& const_dup = dup;
    REQUIRE_THAT(const_dup[0].sample_rate, WithinAbs(48000.0f, 1e-5));

    dup.reset();
    REQUIRE(dup[0].reset_called);
}

// ── Matrix ──────────────────────────────────────────────────────────────

TEST_CASE("Matrix identity", "[dsp][matrix]") {
    auto m = Matrix4::identity();
    REQUIRE(m(0, 0) == 1.0f);
    REQUIRE(m(1, 1) == 1.0f);
    REQUIRE(m(0, 1) == 0.0f);
}

TEST_CASE("Matrix multiply identity", "[dsp][matrix]") {
    auto a = Matrix4::identity();
    auto b = Matrix4::identity();
    auto c = a * b;
    REQUIRE(c == Matrix4::identity());
}

TEST_CASE("Matrix2 determinant", "[dsp][matrix]") {
    Matrix2 m;
    m(0, 0) = 1; m(0, 1) = 2;
    m(1, 0) = 3; m(1, 1) = 4;
    REQUIRE_THAT(determinant(m), WithinAbs(-2.0f, 1e-5));
}

TEST_CASE("Matrix transpose", "[dsp][matrix]") {
    Matrix2 m;
    m(0, 0) = 1; m(0, 1) = 2;
    m(1, 0) = 3; m(1, 1) = 4;
    auto t = m.transposed();
    REQUIRE(t(0, 1) == 3.0f);
    REQUIRE(t(1, 0) == 2.0f);
}

TEST_CASE("Matrix4 translation", "[dsp][matrix]") {
    auto m = translation_matrix(1.0f, 2.0f, 3.0f);
    Vec3 v{0, 0, 0};
    auto result = transform(m, v);
    REQUIRE_THAT(result.x, WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(result.y, WithinAbs(2.0f, 1e-5));
    REQUIRE_THAT(result.z, WithinAbs(3.0f, 1e-5));
}

TEST_CASE("Matrix zero, scalar multiply, and add compose element-wise",
          "[dsp][matrix][issue-645]") {
    auto zero = Matrix3::zero();
    REQUIRE(zero == Matrix3{});

    auto a = Matrix3::identity();
    a(0, 1) = 2.0f;
    a(2, 0) = -1.0f;

    auto doubled = a * 2.0f;
    REQUIRE_THAT(doubled(0, 0), WithinAbs(2.0f, 1e-5));
    REQUIRE_THAT(doubled(0, 1), WithinAbs(4.0f, 1e-5));
    REQUIRE_THAT(doubled(2, 0), WithinAbs(-2.0f, 1e-5));

    auto sum = a + doubled;
    REQUIRE_THAT(sum(0, 0), WithinAbs(3.0f, 1e-5));
    REQUIRE_THAT(sum(0, 1), WithinAbs(6.0f, 1e-5));
    REQUIRE_THAT(sum(2, 0), WithinAbs(-3.0f, 1e-5));
    REQUIRE_FALSE(sum == doubled);
}

TEST_CASE("Matrix3 determinant covers non-triangular matrices",
          "[dsp][matrix][issue-645]") {
    Matrix3 m;
    m(0, 0) = 6.0f;  m(0, 1) = 1.0f;  m(0, 2) = 1.0f;
    m(1, 0) = 4.0f;  m(1, 1) = -2.0f; m(1, 2) = 5.0f;
    m(2, 0) = 2.0f;  m(2, 1) = 8.0f;  m(2, 2) = 7.0f;

    REQUIRE_THAT(determinant(m), WithinAbs(-306.0f, 1e-5));
}

TEST_CASE("Matrix4 scale and rotations transform vectors",
          "[dsp][matrix][issue-645]") {
    constexpr float half_pi = 1.57079632679f;

    auto scaled = transform(scale_matrix(2.0f, 3.0f, 4.0f), Vec3{1.0f, -2.0f, 0.5f});
    REQUIRE_THAT(scaled.x, WithinAbs(2.0f, 1e-5));
    REQUIRE_THAT(scaled.y, WithinAbs(-6.0f, 1e-5));
    REQUIRE_THAT(scaled.z, WithinAbs(2.0f, 1e-5));

    auto around_x = transform(rotation_x(half_pi), Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE_THAT(around_x.x, WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(around_x.y, WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(around_x.z, WithinAbs(1.0f, 1e-5));

    auto around_y = transform(rotation_y(half_pi), Vec3{0.0f, 0.0f, 1.0f});
    REQUIRE_THAT(around_y.x, WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(around_y.y, WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(around_y.z, WithinAbs(0.0f, 1e-5));

    auto around_z = transform(rotation_z(half_pi), Vec3{1.0f, 0.0f, 0.0f});
    REQUIRE_THAT(around_z.x, WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(around_z.y, WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(around_z.z, WithinAbs(0.0f, 1e-5));
}

TEST_CASE("Matrix affine helpers compose transforms and preserve row-major layout",
          "[dsp][matrix][coverage][phase3]") {
    auto translate = translation_matrix(2.0f, -3.0f, 4.0f);
    auto scale = scale_matrix(3.0f, 4.0f, 5.0f);
    auto composed = translate * scale;

    REQUIRE_THAT(composed(0, 0), WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(composed(1, 1), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(composed(2, 2), WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(composed(0, 3), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(composed(1, 3), WithinAbs(-3.0f, 1e-6f));
    REQUIRE_THAT(composed(2, 3), WithinAbs(4.0f, 1e-6f));

    auto transformed = transform(composed, Vec3{1.0f, 2.0f, -1.0f});
    REQUIRE_THAT(transformed.x, WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(transformed.y, WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(transformed.z, WithinAbs(-1.0f, 1e-6f));

    auto transposed = composed.transposed();
    REQUIRE_THAT(transposed(3, 0), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(transposed(3, 1), WithinAbs(-3.0f, 1e-6f));
    REQUIRE_THAT(transposed(3, 2), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(transposed(0, 3), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Matrix arithmetic covers cancellation, determinant signs, and inequality",
          "[dsp][matrix][coverage][phase3]") {
    Matrix2 a;
    a(0, 0) = 2.0f;  a(0, 1) = -3.0f;
    a(1, 0) = 4.0f;  a(1, 1) = 5.0f;

    Matrix2 b;
    b(0, 0) = -2.0f; b(0, 1) = 3.0f;
    b(1, 0) = -4.0f; b(1, 1) = -5.0f;

    auto cancelled = a + b;
    REQUIRE(cancelled == Matrix2::zero());
    REQUIRE_FALSE(a == b);
    REQUIRE_THAT(determinant(a), WithinAbs(22.0f, 1e-6f));
    REQUIRE_THAT(determinant(b), WithinAbs(22.0f, 1e-6f));

    auto product = a * b;
    REQUIRE_THAT(product(0, 0), WithinAbs(8.0f, 1e-6f));
    REQUIRE_THAT(product(0, 1), WithinAbs(21.0f, 1e-6f));
    REQUIRE_THAT(product(1, 0), WithinAbs(-28.0f, 1e-6f));
    REQUIRE_THAT(product(1, 1), WithinAbs(-13.0f, 1e-6f));
}

// ── SpecialFunctions ────────────────────────────────────────────────────

TEST_CASE("sinc function", "[dsp][special]") {
    REQUIRE_THAT(sinc(0.0f), WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(sinc(1.0f), WithinAbs(0.0f, 1e-5));
}

TEST_CASE("bessel_i0", "[dsp][special]") {
    REQUIRE_THAT(bessel_i0(0.0f), WithinAbs(1.0f, 1e-5));
    REQUIRE(bessel_i0(2.0f) > 1.0f);
}

TEST_CASE("dB conversions", "[dsp][special]") {
    REQUIRE_THAT(db_to_linear(0.0f), WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(db_to_linear(-6.0f), WithinAbs(0.5012f, 0.01f));
    REQUIRE_THAT(linear_to_db(1.0f), WithinAbs(0.0f, 1e-5));
}

TEST_CASE("MIDI/frequency conversions", "[dsp][special]") {
    REQUIRE_THAT(midi_to_freq(69.0f), WithinAbs(440.0f, 0.01f));
    REQUIRE_THAT(freq_to_midi(440.0f), WithinAbs(69.0f, 0.01f));
    REQUIRE_THAT(midi_to_freq(60.0f), WithinAbs(261.63f, 0.1f));
}

TEST_CASE("Special functions cover kernels and statistical wrappers",
          "[dsp][special][issue-645]") {
    REQUIRE_THAT(gamma_fn(5.0f), WithinAbs(24.0f, 0.001f));
    REQUIRE_THAT(erf_fn(0.0f), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(erfc_fn(0.0f), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(erf_fn(1.0f) + erfc_fn(1.0f), WithinAbs(1.0f, 0.001f));

    REQUIRE_THAT(lanczos(0.0f, 3), WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(lanczos(3.0f, 3), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(lanczos(-3.0f, 3), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(lanczos(0.5f, 3), WithinAbs(0.6079f, 0.001f));

    REQUIRE_THAT(linear_to_db(0.0f), WithinAbs(-200.0f, 0.001f));
}

// ── Limiter (already exists in compressor.hpp) ──────────────────────────

TEST_CASE("Limiter clamps signal", "[dsp][limiter]") {
    Limiter lim;
    lim.set_threshold_db(-6.0f);
    lim.set_release_ms(10.0f);
    lim.set_sample_rate(44100.0f);

    // Process a loud signal
    float signal = 1.0f;
    float result = lim.process(signal);
    REQUIRE(std::abs(result) <= 0.51f);  // Should be limited to ~-6dB
}
