#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/spectrum_bin_mapping.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

SpectrumBinMapping make(std::size_t columns, int fft = 2048, double sr = 48000.0,
                        double lo = 20.0, double hi = 20000.0) {
    SpectrumBinMapping m;
    REQUIRE(m.configure({sr, fft, columns, lo, hi, -200.0f, 200.0f}));
    return m;
}

// A spectrum with a parabolic (dB) peak whose vertex sits at `vertex_bin`.
std::vector<float> parabola_peak(int bins, double vertex_bin, float top_db, float curvature) {
    std::vector<float> v(static_cast<std::size_t>(bins));
    for (int k = 0; k < bins; ++k) {
        const double d = k - vertex_bin;
        v[static_cast<std::size_t>(k)] = top_db - curvature * static_cast<float>(d * d);
    }
    return v;
}

} // namespace

TEST_CASE("SpectrumBinMapping column geometry", "[signal][spectrum]") {
    auto m = make(100);
    REQUIRE(m.columns() == 100);
    REQUIRE(m.bin_count() == 1025);
    // Centre of the whole axis is the geometric mean of the range.
    CHECK_THAT(m.column_frequency(49) * m.column_frequency(50), WithinAbs(20.0 * 20000.0, 20.0));
    // Frequencies and columns round-trip (a column centre is x.5).
    for (std::size_t i : {0u, 10u, 57u, 99u})
        CHECK_THAT(m.frequency_to_column(m.column_frequency(i)),
                   WithinAbs(static_cast<double>(i) + 0.5, 1e-9));
    CHECK_THAT(m.frequency_to_column(20.0), WithinAbs(0.0, 1e-9));
    CHECK_THAT(m.frequency_to_column(20000.0), WithinAbs(100.0, 1e-9));
    // Low columns are narrower than a bin, high columns much wider.
    CHECK(m.column_width_bins(0) < 1.0);
    CHECK(m.column_width_bins(99) > 5.0);
}

TEST_CASE("SpectrumBinMapping reproduces a peak between bins", "[signal][spectrum]") {
    // Vertex at 42.667 (a 1 kHz tone at 48 kHz/2048), parabolic in dB.
    const double vertex = 1000.0 / (48000.0 / 2048.0);
    auto bins = parabola_peak(1025, vertex, -3.0f, 6.0f);
    auto m = make(2000);
    std::vector<float> out(2000);
    m.map(bins, out, SpectrumAggregation::peak);

    // A parabola through three samples of a parabola IS that parabola, so the
    // mapped value equals the analytic one at every narrow column.
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (m.column_width_bins(i) > 1.0)
            break;
        const double d = m.column_bin_position(i) - vertex;
        const double expected = -3.0 - 6.0 * d * d;
        // Away from the clamped floor and from the bin-0 edge (linear there).
        if (std::abs(d) > 3.0 || m.column_bin_position(i) < 1.0)
            continue;
        CHECK_THAT(out[i], WithinAbs(expected, 1e-3));
    }

    // The mapped peak column is the one nearest the vertex frequency.
    std::size_t best = 0;
    for (std::size_t i = 1; i < out.size(); ++i)
        if (out[i] > out[best])
            best = i;
    const double peak_col = m.frequency_to_column(1000.0) - 0.5;
    CHECK(std::abs(static_cast<double>(best) - peak_col) <= 0.5);
    // And it exceeds both neighbouring bins: the vertex is not clipped to a bin.
    CHECK(out[best] > bins[43]);
}

TEST_CASE("SpectrumBinMapping reads at the position, not the vertex", "[signal][spectrum]") {
    // Three bins around k=10: 0, -6, -2 dB... concave about bin 10 with the
    // vertex pulled toward bin 11.
    std::vector<float> bins(33, -100.0f);
    bins[9] = -10.0f;
    bins[10] = -2.0f;
    bins[11] = -4.0f;
    SpectrumBinMapping m;
    REQUIRE(m.configure({32000.0, 64, 4, 100.0, 10000.0, -200.0f, 200.0f}));
    const float at_bin = m.interpolate(bins, 10.0);
    CHECK_THAT(at_bin, WithinAbs(-2.0, 1e-6));
    // d = +0.25: y0 + b*d + c*d^2, b = (yp-ym)/2 = 3, c = (yp+ym)/2-y0 = -5
    CHECK_THAT(m.interpolate(bins, 10.25), WithinAbs(-2.0 + 3.0 * 0.25 - 5.0 * 0.0625, 1e-5));
    // The position 10.25 must differ from the vertex value (10.3 -> -1.55).
    CHECK(m.interpolate(bins, 10.25) < -1.55f);
}

TEST_CASE("SpectrumBinMapping falls back to linear on ramps and valleys", "[signal][spectrum]") {
    std::vector<float> ramp(1025);
    for (std::size_t k = 0; k < ramp.size(); ++k)
        ramp[k] = -80.0f + 0.05f * static_cast<float>(k); // rising 0.05 dB/bin
    auto m = make(500);
    std::vector<float> out(500);
    m.map(ramp, out, SpectrumAggregation::peak);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (m.column_width_bins(i) > 1.0)
            break;
        CHECK_THAT(out[i], WithinAbs(-80.0 + 0.05 * m.column_bin_position(i), 1e-3));
        if (i > 0)
            CHECK(out[i] >= out[i - 1]); // monotone data stays monotone
    }

    // A valley is convex: linear interpolation cannot dip below the bins.
    std::vector<float> valley(1025, -10.0f);
    valley[100] = -60.0f;
    SpectrumBinMapping m2;
    REQUIRE(m2.configure({48000.0, 2048, 10, 20.0, 20000.0, -200.0f, 200.0f}));
    for (double pos = 99.0; pos <= 101.0; pos += 0.125)
        CHECK(m2.interpolate(valley, pos) >= -60.0f - 1e-6f);
    CHECK_THAT(m2.interpolate(valley, 100.0), WithinAbs(-60.0, 1e-4));
    // Down into the valley the reading falls monotonically between the bins.
    float previous = m2.interpolate(valley, 99.0);
    for (double pos = 99.1; pos <= 100.0001; pos += 0.1) {
        const float v = m2.interpolate(valley, pos);
        CHECK(v <= previous + 1e-4f);
        previous = v;
    }
}

TEST_CASE("SpectrumBinMapping is continuous across a peak", "[signal][spectrum]") {
    // Bins of a Hann-windowed sine: no jump or overshoot where the reading
    // passes from one side of the loudest bin to the other.
    std::vector<float> bins(64, -80.0f);
    bins[41] = -20.63f;
    bins[42] = -2.57f;
    bins[43] = -0.63f;
    bins[44] = -11.51f;
    bins[45] = -31.51f;
    SpectrumBinMapping m;
    REQUIRE(m.configure({48000.0, 126, 4, 100.0, 10000.0, -200.0f, 200.0f}));
    float previous = m.interpolate(bins, 41.0);
    float highest = previous;
    for (double pos = 41.0; pos <= 45.0; pos += 0.01) {
        const float v = m.interpolate(bins, pos);
        CHECK(std::abs(v - previous) < 0.3f); // no step at any cell boundary
        highest = std::max(highest, v);
        previous = v;
    }
    // The peak estimate sits just above the loudest bin, at the true vertex.
    CHECK(highest > -0.63f);
    CHECK(highest < 0.5f);
}

TEST_CASE("SpectrumBinMapping adjacent low columns are not a staircase", "[signal][spectrum]") {
    // A smooth spectrum: every narrow column must read a different value.
    auto bins = parabola_peak(1025, 60.0, -10.0f, 0.01f);
    auto m = make(800);
    std::vector<float> out(800);
    m.map(bins, out, SpectrumAggregation::peak);
    int identical = 0, narrow = 0;
    for (std::size_t i = 1; i < out.size() && m.column_width_bins(i) <= 1.0; ++i) {
        ++narrow;
        if (out[i] == out[i - 1])
            ++identical;
    }
    REQUIRE(narrow > 100);
    CHECK(identical == 0);
}

TEST_CASE("SpectrumBinMapping aggregates wide columns", "[signal][spectrum]") {
    // 1 kHz range [1000, 4000] with 2 columns: both cover many bins.
    SpectrumBinMapping m;
    REQUIRE(m.configure({48000.0, 4096, 1, 1000.0, 4000.0, -200.0f, 200.0f}));
    const std::size_t count = m.bin_count();
    std::vector<float> bins(count, -60.0f);
    // Left edge ~85.3 bins, right edge ~341.3 bins.
    bins[200] = -20.0f;
    std::vector<float> out(1);

    m.map(bins, out, SpectrumAggregation::peak);
    CHECK_THAT(out[0], WithinAbs(-20.0, 1e-4));

    // Power sum: one -20 dB bin plus the other bins at -60 dB.
    m.map(bins, out, SpectrumAggregation::power_sum);
    const double width = m.column_width_bins(0);
    const double expected_sum = 10.0 * std::log10(1e-2 + (width - 1.0) * 1e-6);
    CHECK_THAT(out[0], WithinAbs(expected_sum, 1e-3));

    // Mean power divides by the column width in bins.
    m.map(bins, out, SpectrumAggregation::power_mean);
    CHECK_THAT(out[0], WithinAbs(expected_sum - 10.0 * std::log10(width), 1e-3));

    // Fractional edges: a loud bin only half inside counts half.
    std::vector<float> edge(count, -200.0f);
    const double lo_edge = m.column_bin_position(0) * 0.0 + 1000.0 / (48000.0 / 4096.0);
    const auto k = static_cast<std::size_t>(std::floor(lo_edge + 0.5));
    edge[k] = 0.0f; // cell [k-0.5, k+0.5]
    const double inside = (static_cast<double>(k) + 0.5) - lo_edge;
    REQUIRE(inside > 0.0);
    REQUIRE(inside <= 1.0);
    m.map(edge, out, SpectrumAggregation::power_sum);
    CHECK_THAT(out[0], WithinAbs(10.0 * std::log10(inside), 1e-3));
}

TEST_CASE("SpectrumBinMapping single-bin band", "[signal][spectrum]") {
    // A column exactly one bin wide: width <= 1 takes the interpolation path
    // in every aggregation mode, so all three agree.
    SpectrumBinMapping m;
    const double bin_hz = 48000.0 / 2048.0;
    REQUIRE(m.configure({48000.0, 2048, 1, 100.0 * bin_hz, 101.0 * bin_hz, -200.0f, 200.0f}));
    REQUIRE(m.column_width_bins(0) <= 1.0 + 1e-9);
    auto bins = parabola_peak(1025, 100.4, -5.0f, 3.0f);
    std::vector<float> a(1), b(1), c(1);
    m.map(bins, a, SpectrumAggregation::peak);
    m.map(bins, b, SpectrumAggregation::power_sum);
    m.map(bins, c, SpectrumAggregation::power_mean);
    CHECK(a[0] == b[0]);
    CHECK(a[0] == c[0]);
    const double d = m.column_bin_position(0) - 100.4;
    CHECK_THAT(a[0], WithinAbs(-5.0 - 3.0 * d * d, 1e-3));
}

TEST_CASE("SpectrumBinMapping edge columns", "[signal][spectrum]") {
    auto m = make(64);
    std::vector<float> bins(1025, -30.0f);
    std::vector<float> out(64);
    m.map(bins, out, SpectrumAggregation::peak);
    for (float v : out)
        CHECK_THAT(v, WithinAbs(-30.0, 1e-4));

    // Above Nyquist: 8 kHz sample rate, range to 20 kHz.
    SpectrumBinMapping low;
    REQUIRE(low.configure({8000.0, 512, 100, 20.0, 20000.0, -200.0f, 200.0f}));
    std::vector<float> b2(257, -10.0f);
    std::vector<float> o2(100);
    low.map(b2, o2, SpectrumAggregation::peak);
    CHECK_THAT(o2.front(), WithinAbs(-10.0, 1e-4));
    CHECK(o2.back() == -200.0f); // no phantom shelf past Nyquist
    // The column that straddles Nyquist still reads the top bin, not garbage.
    for (std::size_t i = 0; i < o2.size(); ++i)
        CHECK((o2[i] == -200.0f || std::abs(o2[i] + 10.0f) < 1e-3f));
}

TEST_CASE("SpectrumBinMapping survives FFT-size changes", "[signal][spectrum]") {
    SpectrumBinMapping m;
    REQUIRE(m.configure({48000.0, 1024, 50, 20.0, 20000.0, -200.0f, 200.0f}));
    std::vector<float> b1024(513, -20.0f);
    std::vector<float> out(50, 0.0f);
    m.map(b1024, out, SpectrumAggregation::peak);
    CHECK_THAT(out[10], WithinAbs(-20.0, 1e-4));

    // Bins from a different transform size must not be misread: floor.
    std::vector<float> b4096(2049, -20.0f);
    m.map(b4096, out, SpectrumAggregation::peak);
    for (float v : out)
        CHECK(v == -200.0f);

    REQUIRE(m.configure({48000.0, 4096, 50, 20.0, 20000.0, -200.0f, 200.0f}));
    m.map(b4096, out, SpectrumAggregation::peak);
    CHECK_THAT(out[10], WithinAbs(-20.0, 1e-4));

    // Same peak frequency, different resolution: the mapped peak column holds.
    const double f0 = 1000.0;
    for (int fft : {1024, 4096, 16384}) {
        SpectrumBinMapping mm;
        REQUIRE(mm.configure({48000.0, fft, 400, 20.0, 20000.0, -200.0f, 200.0f}));
        const double vertex = f0 / (48000.0 / fft);
        // Peak sharpness in bins scales with the FFT size.
        auto bins = parabola_peak(fft / 2 + 1, vertex, 0.0f, 6.0f);
        std::vector<float> o(400);
        mm.map(bins, o, SpectrumAggregation::peak);
        std::size_t best = 0;
        for (std::size_t i = 1; i < o.size(); ++i)
            if (o[i] > o[best])
                best = i;
        CHECK(std::abs(static_cast<double>(best) + 0.5 - mm.frequency_to_column(f0)) <= 1.0);
    }
}

TEST_CASE("SpectrumBinMapping degenerate inputs", "[signal][spectrum]") {
    SpectrumBinMapping m;
    // Zero columns: valid, maps nothing, never touches the output.
    REQUIRE(m.configure({48000.0, 2048, 0, 20.0, 20000.0, -200.0f, 200.0f}));
    std::vector<float> none;
    m.map(std::vector<float>(1025, 0.0f), none, SpectrumAggregation::peak);

    CHECK_FALSE(m.configure({0.0, 2048, 10, 20.0, 20000.0, -200.0f, 200.0f}));
    CHECK_FALSE(m.configure({48000.0, 1, 10, 20.0, 20000.0, -200.0f, 200.0f}));
    CHECK_FALSE(m.configure({48000.0, 2048, 10, 2000.0, 20.0, -200.0f, 200.0f}));
    CHECK_FALSE(m.configure({48000.0, 2048, 10, 0.0, 20.0, -200.0f, 200.0f}));
    std::vector<float> out(10, 1.0f);
    m.map(std::vector<float>(1025, 0.0f), out, SpectrumAggregation::peak);
    for (float v : out)
        CHECK(v == -200.0f);

    // NaN / inf bins never leak into the output.
    REQUIRE(m.configure({48000.0, 2048, 300, 20.0, 20000.0, -200.0f, 200.0f}));
    std::vector<float> bins(1025, -40.0f);
    bins[30] = std::numeric_limits<float>::quiet_NaN();
    bins[31] = -std::numeric_limits<float>::infinity();
    bins[500] = std::numeric_limits<float>::infinity();
    bins[501] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> o(300);
    for (auto agg : {SpectrumAggregation::peak, SpectrumAggregation::power_sum,
                     SpectrumAggregation::power_mean}) {
        m.map(bins, o, agg);
        for (float v : o) {
            CHECK(std::isfinite(v));
            CHECK(v >= -200.0f);
            CHECK(v <= 200.0f);
        }
    }
}
