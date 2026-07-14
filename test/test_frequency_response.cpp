/// Tests for the biquad analysis pipeline: coefficients in, response out.
///
/// These assert the properties that distinguish a real filter response from a
/// plausible-looking approximation — a low-pass must actually roll off, a notch
/// must actually null, a shelf must actually plateau. The bug that motivated
/// this file (EqCurveView drawing every band as a Gaussian bell regardless of
/// type) passed every "is it finite / is it smooth" check; only asserting the
/// SHAPE catches it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/filter_design.hpp>
#include <pulp/signal/frequency_response.hpp>

#include <cmath>
#include <vector>

using namespace pulp::signal;

namespace {
constexpr float sr = 48000.0f;

// Magnitude of a single designed section at a frequency, in dB.
float db_at(const BiquadCoefficients& c, float hz) {
    return section_magnitude_db(c, hz, sr);
}
} // namespace

TEST_CASE("peaking EQ boosts at its center frequency and is flat far away",
          "[signal][frequency-response]") {
    auto peak = FilterDesign::peaking_eq(1000.0f, 2.0f, 6.0f, sr);

    REQUIRE_THAT(db_at(peak, 1000.0f), Catch::Matchers::WithinAbs(6.0, 0.1));
    // Far outside the bell the filter does nothing.
    REQUIRE_THAT(db_at(peak, 20.0f), Catch::Matchers::WithinAbs(0.0, 0.2));
    REQUIRE_THAT(db_at(peak, 20000.0f), Catch::Matchers::WithinAbs(0.0, 0.2));
}

TEST_CASE("peaking EQ with negative gain cuts", "[signal][frequency-response]") {
    auto cut = FilterDesign::peaking_eq(1000.0f, 2.0f, -9.0f, sr);
    REQUIRE_THAT(db_at(cut, 1000.0f), Catch::Matchers::WithinAbs(-9.0, 0.1));
}

// ── The shape assertions the Gaussian-bell fake would have failed ──────────

TEST_CASE("lowpass passes low frequencies and rolls off above cutoff",
          "[signal][frequency-response]") {
    auto lp = FilterDesign::lowpass(1000.0f, 0.707f, sr);

    REQUIRE_THAT(db_at(lp, 50.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
    // Butterworth Q: -3 dB at the cutoff, by definition.
    REQUIRE_THAT(db_at(lp, 1000.0f), Catch::Matchers::WithinAbs(-3.0, 0.5));
    // A second-order rolloff is ~12 dB/octave; two octaves up must be well down.
    REQUIRE(db_at(lp, 4000.0f) < -20.0f);
    REQUIRE(db_at(lp, 16000.0f) < -40.0f);

    // Monotonically falling across the stopband — a bell would come back up.
    REQUIRE(db_at(lp, 2000.0f) > db_at(lp, 4000.0f));
    REQUIRE(db_at(lp, 4000.0f) > db_at(lp, 8000.0f));
    REQUIRE(db_at(lp, 8000.0f) > db_at(lp, 16000.0f));
}

TEST_CASE("highpass blocks DC and passes high frequencies",
          "[signal][frequency-response]") {
    auto hp = FilterDesign::highpass(1000.0f, 0.707f, sr);

    REQUIRE(db_at(hp, 20.0f) < -30.0f);
    REQUIRE_THAT(db_at(hp, 1000.0f), Catch::Matchers::WithinAbs(-3.0, 0.5));
    REQUIRE_THAT(db_at(hp, 10000.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
}

TEST_CASE("notch nulls at its center and passes everything else",
          "[signal][frequency-response]") {
    auto n = FilterDesign::notch(1000.0f, 8.0f, sr);

    // The defining property: a deep null exactly at center.
    REQUIRE(db_at(n, 1000.0f) < -40.0f);
    // And unity gain well away from it, on both sides.
    REQUIRE_THAT(db_at(n, 100.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
    REQUIRE_THAT(db_at(n, 10000.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
}

TEST_CASE("shelves plateau rather than returning to unity",
          "[signal][frequency-response]") {
    auto low = FilterDesign::low_shelf(500.0f, 6.0f, sr);
    // A low shelf holds its boost all the way down to DC — this is precisely
    // what a symmetric bell approximation cannot represent.
    REQUIRE_THAT(db_at(low, 20.0f), Catch::Matchers::WithinAbs(6.0, 0.5));
    REQUIRE_THAT(db_at(low, 50.0f), Catch::Matchers::WithinAbs(6.0, 0.5));
    REQUIRE_THAT(db_at(low, 15000.0f), Catch::Matchers::WithinAbs(0.0, 0.5));

    auto high = FilterDesign::high_shelf(5000.0f, -6.0f, sr);
    REQUIRE_THAT(db_at(high, 20000.0f), Catch::Matchers::WithinAbs(-6.0, 0.5));
    REQUIRE_THAT(db_at(high, 100.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
}

TEST_CASE("bandpass peaks at center and falls off both sides",
          "[signal][frequency-response]") {
    auto bp = FilterDesign::bandpass(1000.0f, 4.0f, sr);
    REQUIRE_THAT(db_at(bp, 1000.0f), Catch::Matchers::WithinAbs(0.0, 0.5));
    REQUIRE(db_at(bp, 100.0f) < -15.0f);
    REQUIRE(db_at(bp, 10000.0f) < -15.0f);
}

// ── Cascade ───────────────────────────────────────────────────────────────

TEST_CASE("cascade magnitude sums the sections' gains in dB",
          "[signal][frequency-response]") {
    // Two peaks at the same frequency: +6 and +4 must read as +10 dB.
    std::vector<BiquadCoefficients> sos{
        FilterDesign::peaking_eq(1000.0f, 4.0f, 6.0f, sr),
        FilterDesign::peaking_eq(1000.0f, 4.0f, 4.0f, sr)};

    REQUIRE_THAT(cascade_magnitude_db(sos, 1000.0f, sr),
                 Catch::Matchers::WithinAbs(10.0, 0.2));
}

TEST_CASE("an empty cascade is unity gain", "[signal][frequency-response]") {
    std::vector<BiquadCoefficients> none;
    REQUIRE_THAT(cascade_magnitude_db(none, 1000.0f, sr),
                 Catch::Matchers::WithinAbs(0.0, 1e-6));
}

// ── Round trip: the runtime filter can report its own response ─────────────

TEST_CASE("a Biquad can be loaded from designed coefficients and read back",
          "[signal][frequency-response][biquad]") {
    auto designed = FilterDesign::peaking_eq(2000.0f, 1.5f, 5.0f, sr);

    Biquad filter;
    filter.set_coefficients(designed);
    auto read_back = filter.coefficients();

    REQUIRE_THAT(read_back.b0, Catch::Matchers::WithinRel(designed.b0, 1e-6f));
    REQUIRE_THAT(read_back.a2, Catch::Matchers::WithinRel(designed.a2, 1e-6f));

    // And the live filter reports the response of what it is actually running.
    REQUIRE_THAT(magnitude_db(filter, 2000.0f, sr), Catch::Matchers::WithinAbs(5.0, 0.1));
}

TEST_CASE("a filter tuned by type reports the matching response",
          "[signal][frequency-response][biquad]") {
    // The path a plugin actually takes: set_coefficients(Type, ...) to process
    // audio, then ask the same object what curve it is drawing.
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.707f, sr);

    REQUIRE_THAT(magnitude_db(filter, 1000.0f, sr), Catch::Matchers::WithinAbs(-3.0, 0.5));
    REQUIRE(magnitude_db(filter, 8000.0f, sr) < -30.0f);
}

TEST_CASE("processing audio agrees with the reported magnitude",
          "[signal][frequency-response][biquad]") {
    // The strongest form of the claim: the curve is not merely self-consistent,
    // it predicts what the audio path does. Run a sine through the filter and
    // compare the measured output level to the advertised magnitude.
    constexpr float test_hz = 500.0f;
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.707f, sr);

    const float predicted_db = magnitude_db(filter, test_hz, sr);

    const int n = 48000; // 1 second — long enough for the transient to decay
    float peak = 0.0f;
    const auto two_pi = 6.283185307179586f;
    for (int i = 0; i < n; ++i) {
        float in = std::sin(two_pi * test_hz * static_cast<float>(i) / sr);
        float out = filter.process(in);
        if (i > n / 2) peak = std::max(peak, std::abs(out)); // steady state only
    }
    const float measured_db = 20.0f * std::log10(peak);

    REQUIRE_THAT(measured_db, Catch::Matchers::WithinAbs(predicted_db, 0.2));
}

// ── Sampling helpers ──────────────────────────────────────────────────────

TEST_CASE("log_frequency_at spans the range endpoint to endpoint",
          "[signal][frequency-response]") {
    REQUIRE_THAT(log_frequency_at(0, 100, 20.0, 20000.0),
                 Catch::Matchers::WithinRel(20.0, 1e-6));
    REQUIRE_THAT(log_frequency_at(99, 100, 20.0, 20000.0),
                 Catch::Matchers::WithinRel(20000.0, 1e-6));
    // Logarithmic, so the midpoint is the geometric mean (~632 Hz), not 10 kHz.
    REQUIRE_THAT(log_frequency_at(50, 101, 20.0, 20000.0),
                 Catch::Matchers::WithinRel(632.4555, 1e-3));
}

TEST_CASE("fft_bin_frequency spans DC to Nyquist", "[signal][frequency-response]") {
    // A 1024-point real FFT yields 513 bins: bin 0 is DC, bin 512 is Nyquist.
    REQUIRE_THAT(fft_bin_frequency(0, 513, 48000.0), Catch::Matchers::WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(fft_bin_frequency(512, 513, 48000.0),
                 Catch::Matchers::WithinRel(24000.0, 1e-9));
    REQUIRE_THAT(fft_bin_frequency(256, 513, 48000.0),
                 Catch::Matchers::WithinRel(12000.0, 1e-9));
}

TEST_CASE("response_curve_db fills a buffer matching point-wise evaluation",
          "[signal][frequency-response]") {
    std::vector<BiquadCoefficients> sos{FilterDesign::peaking_eq(1000.0f, 2.0f, 6.0f, sr)};

    std::vector<float> curve(128);
    response_curve_db(sos, 20.0, 20000.0, sr, curve);

    REQUIRE(curve.size() == 128);
    for (size_t i = 0; i < curve.size(); ++i) {
        const double hz = log_frequency_at(i, curve.size(), 20.0, 20000.0);
        REQUIRE_THAT(curve[i],
                     Catch::Matchers::WithinAbs(cascade_magnitude_db(sos, hz, sr), 1e-4));
    }
}

TEST_CASE("a notch's null is floored rather than negative infinity",
          "[signal][frequency-response]") {
    // A perfect null is -inf dB. Curve points must stay finite and plottable.
    std::vector<BiquadCoefficients> sos{FilterDesign::notch(1000.0f, 10.0f, sr)};
    std::vector<float> curve(512);
    response_curve_db(sos, 20.0, 20000.0, sr, curve);

    for (float db : curve) {
        REQUIRE(std::isfinite(db));
        REQUIRE(db >= min_response_db);
    }
}
