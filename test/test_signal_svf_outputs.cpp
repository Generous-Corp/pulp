// Multi-output TPT state-variable filter: every response of one pass.
// Expected values come from the analytic transfer functions of the analog
// prototype (Zavalishin, "The Art of VA Filter Design", chapter 4), evaluated
// at the cutoff, where the prewarped TPT filter matches the prototype exactly.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/svf.hpp>

#include <array>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::Svf64;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

struct Gains {
    double lowpass = 0, highpass = 0, bandpass = 0, unity_bandpass = 0, notch = 0, allpass = 0,
           peak = 0, band_shelf = 0;
};

// Steady-state sine amplitude (from RMS) of each output for a unit sine at `hz`.
Gains measure(double cutoff, double q, double hz, double shelf_gain = 1.0) {
    Svf64 filter;
    filter.configure(Svf64::Mode::lowpass, kSampleRate, cutoff, q);
    filter.set_shelf_gain(shelf_gain);
    const int settle = 20000;
    const int window = 4800; // 100 ms: many whole periods for the tested tones
    Gains peak{};
    for (int n = 0; n < settle + window; ++n) {
        const double x = std::sin(2.0 * kPi * hz * n / kSampleRate);
        const auto o = filter.process_all(x);
        if (n < settle)
            continue;
        peak.lowpass += o.lowpass * o.lowpass;
        peak.highpass += o.highpass * o.highpass;
        peak.bandpass += o.bandpass * o.bandpass;
        peak.unity_bandpass += o.unity_bandpass * o.unity_bandpass;
        peak.notch += o.notch * o.notch;
        peak.allpass += o.allpass * o.allpass;
        peak.peak += o.peak * o.peak;
        peak.band_shelf += o.band_shelf * o.band_shelf;
    }
    // RMS over whole periods, scaled to the amplitude of a sine.
    const double scale = std::sqrt(2.0 / window);
    for (double* v : {&peak.lowpass, &peak.highpass, &peak.bandpass, &peak.unity_bandpass,
                      &peak.notch, &peak.allpass, &peak.peak, &peak.band_shelf})
        *v = std::sqrt(*v) * scale;
    return peak;
}

} // namespace

TEST_CASE("Svf outputs at the cutoff match the analytic prototype", "[signal][svf][response]") {
    for (const double q : {0.5, 0.7071, 2.0, 8.0}) {
        const double cutoff = 6000.0; // 8 samples per cycle
        const auto g = measure(cutoff, q, cutoff);
        INFO("q = " << q);
        // s = j at the cutoff: LP = 1/(jk), BP = 1/k, HP = -1/(jk) with k = 1/q.
        CHECK_THAT(g.lowpass, WithinRel(q, 1e-3));
        CHECK_THAT(g.highpass, WithinRel(q, 1e-3));
        CHECK_THAT(g.bandpass, WithinRel(q, 1e-3));
        CHECK_THAT(g.unity_bandpass, WithinRel(1.0, 1e-3));
        CHECK_THAT(g.notch, WithinAbs(0.0, 1e-3));
        CHECK_THAT(g.allpass, WithinRel(1.0, 1e-3));
        // peak = LP - HP = (1 - s^2)/D, magnitude 2/k at s = j.
        CHECK_THAT(g.peak, WithinRel(2.0 * q, 1e-3));
        // Transparent until a shelf gain is set.
        CHECK_THAT(g.band_shelf, WithinRel(1.0, 1e-3));
    }
}

TEST_CASE("Svf allpass has unit magnitude at every frequency", "[signal][svf][allpass]") {
    for (const double q : {0.5, 3.0}) {
        for (const double hz : {150.0, 1000.0, 4000.0, 6000.0, 9000.0, 18000.0}) {
            const auto g = measure(6000.0, q, hz);
            INFO("q = " << q << " hz = " << hz);
            CHECK_THAT(g.allpass, WithinRel(1.0, 2e-3));
        }
    }
}

TEST_CASE("Svf notch plus unity bandpass reconstructs the input", "[signal][svf][reconstruction]") {
    Svf64 filter;
    filter.configure(Svf64::Mode::lowpass, kSampleRate, 1234.0, 3.5);
    double worst = 0.0;
    double state = 12345.0;
    for (int n = 0; n < 5000; ++n) {
        state = std::fmod(state * 1103515245.0 + 12345.0, 2147483648.0);
        const double x = state / 1073741824.0 - 1.0;
        const auto o = filter.process_all(x);
        worst = std::max(worst, std::abs(o.notch + o.unity_bandpass - x));
        // Lowpass plus highpass is the notch by construction.
        worst = std::max(worst, std::abs(o.lowpass + o.highpass - o.notch));
    }
    CHECK(worst < 1e-12);
}

TEST_CASE("Svf band shelf reaches its gain at the cutoff and is flat far away",
          "[signal][svf][shelf]") {
    const double gain = 3.0;
    const auto at_cutoff = measure(6000.0, 1.5, 6000.0, gain);
    CHECK_THAT(at_cutoff.band_shelf, WithinRel(gain, 2e-3));
    const auto far_below = measure(6000.0, 1.5, 60.0, gain);
    CHECK_THAT(far_below.band_shelf, WithinRel(1.0, 2e-2));
    const auto far_above = measure(6000.0, 1.5, 23000.0, gain);
    CHECK_THAT(far_above.band_shelf, WithinRel(1.0, 5e-2));
    // A cut is the mirror image.
    const auto cut = measure(6000.0, 1.5, 6000.0, 0.25);
    CHECK_THAT(cut.band_shelf, WithinRel(0.25, 2e-3));
}

TEST_CASE("Svf legacy modes are unchanged and the new modes match process_all",
          "[signal][svf][compat]") {
    using Mode = Svf64::Mode;
    // Numeric values of the original modes are part of the API.
    static_assert(static_cast<int>(Mode::lowpass) == 0);
    static_assert(static_cast<int>(Mode::highpass) == 1);
    static_assert(static_cast<int>(Mode::bandpass) == 2);
    static_assert(static_cast<int>(Mode::notch) == 3);

    const std::array<Mode, 8> modes{Mode::lowpass,       Mode::highpass, Mode::bandpass,
                                    Mode::notch,         Mode::unity_bandpass, Mode::allpass,
                                    Mode::peak,          Mode::band_shelf};
    for (const Mode mode : modes) {
        Svf64 single, bulk, block;
        for (Svf64* f : {&single, &bulk, &block}) {
            f->configure(mode, kSampleRate, 900.0, 2.2);
            f->set_shelf_gain(2.0);
        }
        std::vector<double> input(777);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = std::sin(0.01 * i * i) + 0.3 * std::cos(0.7 * i);
        std::vector<double> buffer = input;
        block.process(buffer.data(), static_cast<int>(buffer.size()));
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double s = single.process(input[i]);
            const auto o = bulk.process_all(input[i]);
            const double expect[] = {o.lowpass, o.highpass,       o.bandpass, o.notch,
                                     o.unity_bandpass, o.allpass, o.peak,    o.band_shelf};
            const double want = expect[static_cast<int>(mode)];
            REQUIRE_THAT(s, WithinAbs(want, 1e-12));
            REQUIRE_THAT(buffer[i], WithinAbs(want, 1e-9));
        }
    }
}

TEST_CASE("Svf allpass stays bounded at extreme resonance and near Nyquist",
          "[signal][svf][stability]") {
    for (const double hz : {20.0, 5000.0, 23900.0}) {
        Svf64 filter;
        filter.configure(Svf64::Mode::allpass, kSampleRate, hz, 200.0);
        double worst = 0.0;
        for (int n = 0; n < 20000; ++n) {
            const double x = (n % 97 == 0) ? 1.0 : 0.0;
            worst = std::max(worst, std::abs(filter.process(x)));
        }
        CHECK(std::isfinite(worst));
        CHECK(worst < 1e3);
    }
}
