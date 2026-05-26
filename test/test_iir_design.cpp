// IIR design (Chebyshev I / II) acceptance tests.
//
// Goldens are derived analytically from the closed-form prototype
// equations rather than from a SciPy round-trip, so the tests run with
// no external dependency. Each test exercises a property that follows
// directly from the design spec:
//   - Chebyshev I: passband ripple <= R dB, |H(jωc)| at the bottom of
//     the ripple band, strong rolloff beyond cutoff
//   - Chebyshev II: stopband attenuation >= As dB starting at ωs,
//     monotonic ~flat passband (no ripple)
//   - All: cascade is BIBO stable (every section's poles |z|<1)
//
// Elliptic (Cauer) is intentionally deferred — its acceptance goldens
// need the full complex Jacobi-cd machinery, tracked in the macOS
// plugin-authoring plan §2.1 as a follow-up.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/iir_design.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

double mag_to_db(double mag) {
    if (mag < 1e-10) return -200.0;
    return 20.0 * std::log10(mag);
}

double cascade_db_at_hz(const std::vector<FilterDesign::Coefficients>& sos,
                        double freq_hz, double fs) {
    double omega = 2.0 * kPi * freq_hz / fs;
    return mag_to_db(cascade_magnitude(sos, omega));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  Chebyshev Type I
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("IirDesign chebyshev1_lowpass produces correct section count",
          "[signal][iir_design]") {
    auto sos2 = IirDesign::chebyshev1_lowpass(2, 1000.0f, 1.0f, 44100.0f);
    REQUIRE(sos2.size() == 1);
    auto sos4 = IirDesign::chebyshev1_lowpass(4, 1000.0f, 1.0f, 44100.0f);
    REQUIRE(sos4.size() == 2);
    auto sos8 = IirDesign::chebyshev1_lowpass(8, 1000.0f, 1.0f, 44100.0f);
    REQUIRE(sos8.size() == 4);
}

TEST_CASE("IirDesign chebyshev1_lowpass cascade is stable",
          "[signal][iir_design]") {
    for (int N : {2, 4, 6, 8}) {
        for (float ripple : {0.5f, 1.0f, 3.0f}) {
            auto sos = IirDesign::chebyshev1_lowpass(N, 2000.0f, ripple, 48000.0f);
            INFO("N=" << N << " ripple=" << ripple);
            REQUIRE(cascade_is_stable(sos));
        }
    }
}

TEST_CASE("IirDesign chebyshev1_lowpass passband ripple matches spec",
          "[signal][iir_design]") {
    // For Chebyshev I, the passband 0..wc oscillates between 0 dB peaks
    // and troughs of -R dB. Peak-to-trough span must be <= R dB + tolerance.
    const double fs = 48000.0;
    const double fc = 2000.0;
    const double ripple_db = 1.0;
    auto sos = IirDesign::chebyshev1_lowpass(6, static_cast<float>(fc),
                                              static_cast<float>(ripple_db),
                                              static_cast<float>(fs));
    REQUIRE_FALSE(sos.empty());

    double max_db = -1e9, min_db = 1e9;
    for (int i = 1; i <= 200; ++i) {
        double f = fc * static_cast<double>(i) / 200.0;
        double db = cascade_db_at_hz(sos, f, fs);
        max_db = std::max(max_db, db);
        min_db = std::min(min_db, db);
    }
    INFO("max " << max_db << " min " << min_db);
    REQUIRE((max_db - min_db) <= ripple_db + 0.15);
}

TEST_CASE("IirDesign chebyshev1_lowpass gain at cutoff is -R dB",
          "[signal][iir_design]") {
    // |H(jωc)| = 1/sqrt(1+ε²)  ->  -R dB for even N when normalized so peaks
    // sit at 0 dB. Equivalently, the gain at cutoff equals the bottom of the
    // ripple band.
    const double fs = 48000.0;
    const double fc = 1000.0;
    const double ripple_db = 1.0;
    auto sos = IirDesign::chebyshev1_lowpass(4, static_cast<float>(fc),
                                              static_cast<float>(ripple_db),
                                              static_cast<float>(fs));
    double db_at_cutoff = cascade_db_at_hz(sos, fc, fs);
    INFO("gain at cutoff = " << db_at_cutoff << " dB");
    REQUIRE_THAT(db_at_cutoff, WithinAbs(-ripple_db, 0.1));
}

TEST_CASE("IirDesign chebyshev1_lowpass attenuates above cutoff",
          "[signal][iir_design]") {
    auto sos = IirDesign::chebyshev1_lowpass(6, 1000.0f, 1.0f, 48000.0f);
    // 6th-order Cheb I with 1 dB ripple has strong rolloff beyond cutoff.
    double db_2x = cascade_db_at_hz(sos, 2000.0, 48000.0);
    REQUIRE(db_2x < -30.0);
    double db_4x = cascade_db_at_hz(sos, 4000.0, 48000.0);
    REQUIRE(db_4x < -60.0);
}

TEST_CASE("IirDesign chebyshev1_highpass blocks DC and passes high freqs",
          "[signal][iir_design]") {
    auto sos = IirDesign::chebyshev1_highpass(4, 1000.0f, 1.0f, 48000.0f);
    REQUIRE(cascade_is_stable(sos));
    double db_dc = cascade_db_at_hz(sos, 1.0, 48000.0);
    REQUIRE(db_dc < -60.0);
    double db_high = cascade_db_at_hz(sos, 8000.0, 48000.0);
    REQUIRE(db_high > -1.5);
}

TEST_CASE("IirDesign chebyshev1 rejects invalid inputs",
          "[signal][iir_design]") {
    REQUIRE(IirDesign::chebyshev1_lowpass(3, 1000.0f, 1.0f, 44100.0f).empty());
    REQUIRE(IirDesign::chebyshev1_lowpass(0, 1000.0f, 1.0f, 44100.0f).empty());
    REQUIRE(IirDesign::chebyshev1_lowpass(4, 1000.0f, -1.0f, 44100.0f).empty());
    REQUIRE(IirDesign::chebyshev1_lowpass(4, 22050.0f, 1.0f, 44100.0f).empty());
}

// ─────────────────────────────────────────────────────────────────────────
//  Chebyshev Type II (inverse Chebyshev)
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("IirDesign chebyshev2_lowpass is stable across orders",
          "[signal][iir_design]") {
    for (int N : {2, 4, 6, 8}) {
        auto sos = IirDesign::chebyshev2_lowpass(N, 5000.0f, 40.0f, 48000.0f);
        INFO("N=" << N);
        REQUIRE_FALSE(sos.empty());
        REQUIRE(cascade_is_stable(sos));
    }
}

TEST_CASE("IirDesign chebyshev2_lowpass meets stopband attenuation",
          "[signal][iir_design]") {
    const double fs = 48000.0;
    const double fs_edge = 5000.0;
    const double As = 40.0;
    auto sos = IirDesign::chebyshev2_lowpass(4, static_cast<float>(fs_edge),
                                              static_cast<float>(As),
                                              static_cast<float>(fs));
    // At the stopband edge and beyond, the worst-case gain should be no
    // greater than -As dB (the stopband ripple bound).
    for (double f : {fs_edge, fs_edge * 1.5, fs_edge * 2.0, fs_edge * 3.0}) {
        if (f >= 0.5 * fs) break;
        double db = cascade_db_at_hz(sos, f, fs);
        INFO("f=" << f << " db=" << db);
        REQUIRE(db <= -As + 0.5);
    }
}

TEST_CASE("IirDesign chebyshev2_lowpass passband is monotonic and ~flat",
          "[signal][iir_design]") {
    // Inverse Cheb is maximally flat in the passband (no ripple).
    auto sos = IirDesign::chebyshev2_lowpass(4, 5000.0f, 40.0f, 48000.0f);
    double db_dc = cascade_db_at_hz(sos, 1.0, 48000.0);
    double db_low = cascade_db_at_hz(sos, 1000.0, 48000.0);
    REQUIRE_THAT(db_dc, WithinAbs(0.0, 0.5));
    REQUIRE(std::abs(db_low - db_dc) < 1.0);
}

TEST_CASE("IirDesign chebyshev2_highpass blocks DC",
          "[signal][iir_design]") {
    auto sos = IirDesign::chebyshev2_highpass(4, 1000.0f, 40.0f, 48000.0f);
    REQUIRE(cascade_is_stable(sos));
    double db_dc = cascade_db_at_hz(sos, 1.0, 48000.0);
    // Cheb II HP: DC sits deep in the stopband, attenuation >= As.
    REQUIRE(db_dc <= -39.5);
}

TEST_CASE("IirDesign chebyshev2 rejects invalid inputs",
          "[signal][iir_design]") {
    REQUIRE(IirDesign::chebyshev2_lowpass(3, 5000.0f, 40.0f, 48000.0f).empty());
    REQUIRE(IirDesign::chebyshev2_lowpass(0, 5000.0f, 40.0f, 48000.0f).empty());
    REQUIRE(IirDesign::chebyshev2_lowpass(4, 0.0f, 40.0f, 48000.0f).empty());
    REQUIRE(IirDesign::chebyshev2_lowpass(4, 24000.0f, 40.0f, 48000.0f).empty());
    REQUIRE(IirDesign::chebyshev2_lowpass(4, 5000.0f, 0.0f, 48000.0f).empty());
}

// ─────────────────────────────────────────────────────────────────────────
//  Common utilities
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("cascade_magnitude returns 1.0 for an identity cascade",
          "[signal][iir_design]") {
    std::vector<FilterDesign::Coefficients> sos{
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f}};
    REQUIRE_THAT(cascade_magnitude(sos, 0.1), WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(cascade_magnitude(sos, kPi), WithinAbs(1.0, 1e-12));
}

TEST_CASE("cascade_is_stable rejects an unstable section",
          "[signal][iir_design]") {
    // a2 = 1.5 is outside the unit-disk stability triangle.
    std::vector<FilterDesign::Coefficients> bad{
        {1.0f, 0.0f, 0.0f, 0.1f, 1.5f}};
    REQUIRE_FALSE(cascade_is_stable(bad));
}
