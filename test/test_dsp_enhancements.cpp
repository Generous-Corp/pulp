#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/dry_wet_mixer.hpp>
#include <pulp/signal/processor_duplicator.hpp>
#include <pulp/signal/matrix.hpp>
#include <pulp/signal/special_functions.hpp>
#include <pulp/signal/compressor.hpp>
#include <vector>
#include <cmath>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

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

// ── ProcessorDuplicator ─────────────────────────────────────────────────

// Simple gain processor for testing
struct TestGain {
    float gain = 1.0f;
    void set_sample_rate(float) {}
    float process(float x) { return x * gain; }
    void reset() {}
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
