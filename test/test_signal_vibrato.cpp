// test_signal_vibrato.cpp — acceptance suite for the three vibrato lineages.
//
// The load-bearing claim of this module is that `DelayVibratoT` shifts pitch and
// the two phase engines do not, so the suite is built around ONE instrument —
// peak instantaneous-frequency deviation in cents — pointed at all three. A
// suite where the three engines pass the same assertions would not be testing
// this module at all.
//
// The instrument is calibrated against a synthetic FM tone of known deviation
// before it is pointed at any engine (first test case below). It has already
// been wrong once: differentiating the demodulated phase pointwise reads almost
// exactly 2x the truth, because the demodulator leaves a ripple at twice the
// carrier that is negligible in the phase and dominant in its derivative. The
// fix, and the reason the fit is coherent over whole modulator cycles, is
// documented on `peak_cents_deviation`.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/vibrato.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace pulp::signal;

namespace {

// ── Measurement-recipe constants (acceptance class: they define how we
// measure, not what ships) ────────────────────────────────────────────────
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kFs = 48000.0;
/// Modulator rate used by the pitch measurements. Faster than either engine's
/// default so a render covers several cycles cheaply; 48000/5 is an integer, so
/// the coherent fit lands on whole cycles.
constexpr double kProbeRateHz = 5.0;
/// Modulator harmonics kept by the coherent fit. The demodulator's residual
/// ripple sits at harmonic 2*f0/f_lfo (1600 at the 4 kHz carrier), far above
/// this, and a coherent fit over whole cycles rejects it exactly.
constexpr int kFitHarmonics = 16;
/// Cycles rendered, and cycles skipped before the fit window, so filter and
/// delay-line startup never enters a measurement.
constexpr int kRenderCycles = 3;
constexpr int kSkipCycles = 1;

std::vector<double> sine_buffer(std::size_t count, double hz, double fs = kFs) {
    std::vector<double> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::sin(kTwoPi * hz * static_cast<double>(i) / fs);
    }
    return out;
}

void unwrap(std::vector<double>& phase) {
    double offset = 0.0;
    for (std::size_t i = 1; i < phase.size(); ++i) {
        const double step = (phase[i] + offset) - phase[i - 1];
        if (step > std::numbers::pi) {
            offset -= kTwoPi;
        } else if (step < -std::numbers::pi) {
            offset += kTwoPi;
        }
        phase[i] += offset;
    }
}

/// Complex envelope of a signal about `carrier_hz`, from a sliding DFT whose
/// window is exactly one carrier period. One period is the point: the
/// sum-frequency image lands on exactly two cycles inside the window and
/// cancels, so no analysis filter — and therefore no analysis-filter
/// passband droop — sits between the signal and the measurement.
std::vector<std::complex<double>> demodulate(const std::vector<double>& x, double carrier_hz,
                                             double fs = kFs) {
    const auto period = static_cast<int>(std::llround(fs / carrier_hz));
    REQUIRE(std::abs(fs / carrier_hz - static_cast<double>(period)) < 1e-9);
    const int count = static_cast<int>(x.size()) - period;
    REQUIRE(count > 0);

    std::vector<double> cosine(x.size());
    std::vector<double> sine(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double angle = kTwoPi * carrier_hz * static_cast<double>(i) / fs;
        cosine[i] = std::cos(angle);
        sine[i] = std::sin(angle);
    }

    std::vector<std::complex<double>> envelope(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        double real = 0.0;
        double imag = 0.0;
        for (int k = 0; k < period; ++k) {
            const auto n = static_cast<std::size_t>(i + k);
            real += x[n] * cosine[n];
            imag -= x[n] * sine[n];
        }
        const double scale = 2.0 / static_cast<double>(period);
        envelope[static_cast<std::size_t>(i)] = {real * scale, imag * scale};
    }
    return envelope;
}

std::vector<double> instantaneous_phase(const std::vector<double>& x, double carrier_hz,
                                        double fs = kFs) {
    const auto envelope = demodulate(x, carrier_hz, fs);
    std::vector<double> phase(envelope.size());
    for (std::size_t i = 0; i < envelope.size(); ++i) phase[i] = std::arg(envelope[i]);
    unwrap(phase);
    return phase;
}

/// Peak-to-peak amplitude ripple of a demodulated carrier, in dB. A pitch
/// vibrato that also modulates amplitude is doing something it was not asked to
/// do, and the size of that ripple is set almost entirely by how flat the
/// fractional-delay interpolator's magnitude response is across the fractional
/// range.
double envelope_ripple_db(const std::vector<double>& x, double carrier_hz, int start,
                          double fs = kFs) {
    const auto envelope = demodulate(x, carrier_hz, fs);
    double low = 1e9;
    double high = -1e9;
    for (std::size_t i = static_cast<std::size_t>(start); i < envelope.size(); ++i) {
        const double magnitude = std::abs(envelope[i]);
        low = std::min(low, magnitude);
        high = std::max(high, magnitude);
    }
    return 20.0 * std::log10(high / low);
}

struct Deviation {
    double up_cents = 0.0;
    double down_cents = 0.0;

    double span() const { return up_cents - down_cents; }
    /// How lopsided the sweep is. 1 is symmetric; a vactrol pushes it away.
    double asymmetry() const { return std::abs(up_cents) / std::abs(down_cents); }
};

/// Peak cents deviation from a coherent harmonic fit of the phase.
///
/// Pointwise differencing does not work here and the failure is not subtle: the
/// demodulator's residual ripple at twice the carrier carries a tiny phase
/// amplitude but an enormous slope, and reading the max of a pointwise
/// derivative returns roughly twice the true deviation. Fitting the first
/// `kFitHarmonics` modulator harmonics over a whole number of modulator cycles
/// removes it exactly rather than approximately, because a coherent DFT over
/// whole cycles has no leakage between harmonics.
Deviation peak_cents_deviation(const std::vector<double>& phase, int start, double carrier_hz,
                               double modulator_hz, int harmonics = kFitHarmonics,
                               double fs = kFs) {
    const auto cycle = static_cast<int>(std::llround(fs / modulator_hz));
    const int cycles = (static_cast<int>(phase.size()) - start) / cycle;
    REQUIRE(cycles >= 1);
    const int count = cycles * cycle;

    std::vector<double> cos_term(static_cast<std::size_t>(harmonics) + 1, 0.0);
    std::vector<double> sin_term(static_cast<std::size_t>(harmonics) + 1, 0.0);
    for (int k = 1; k <= harmonics; ++k) {
        double a = 0.0;
        double b = 0.0;
        for (int i = 0; i < count; ++i) {
            const double angle =
                kTwoPi * k * modulator_hz * static_cast<double>(start + i) / fs;
            a += phase[static_cast<std::size_t>(start + i)] * std::cos(angle);
            b += phase[static_cast<std::size_t>(start + i)] * std::sin(angle);
        }
        cos_term[static_cast<std::size_t>(k)] = 2.0 * a / count;
        sin_term[static_cast<std::size_t>(k)] = 2.0 * b / count;
    }

    Deviation result{-1e9, 1e9};
    for (int i = 0; i < cycle; ++i) {
        double hz = 0.0;
        for (int k = 1; k <= harmonics; ++k) {
            const double angle =
                kTwoPi * k * modulator_hz * static_cast<double>(start + i) / fs;
            hz += k * modulator_hz * (sin_term[static_cast<std::size_t>(k)] * std::cos(angle) -
                                      cos_term[static_cast<std::size_t>(k)] * std::sin(angle));
        }
        const double cents = 1200.0 * std::log2((carrier_hz + hz) / carrier_hz);
        result.up_cents = std::max(result.up_cents, cents);
        result.down_cents = std::min(result.down_cents, cents);
    }
    return result;
}

/// Renders a carrier through a mono engine and returns its peak deviation.
template <typename Engine>
Deviation measure_engine(Engine& engine, double carrier_hz, double modulator_hz) {
    const auto cycle = static_cast<std::size_t>(std::llround(kFs / modulator_hz));
    const auto input = sine_buffer(cycle * kRenderCycles, carrier_hz);
    std::vector<double> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) output[i] = engine.process(input[i]);
    return peak_cents_deviation(instantaneous_phase(output, carrier_hz),
                                static_cast<int>(cycle * kSkipCycles), carrier_hz, modulator_hz);
}

/// Quasi-static prediction of what an allpass cascade summed with a direct path
/// does to a carrier's instantaneous frequency, given the cascade's per-sample
/// corner trajectory. This is an INDEPENDENT model — textbook allpass phase
/// `-2*atan(tan(pi*f/fs) / tan(pi*fc/fs))` plus the blend geometry — not a
/// second copy of the engine, so agreement between the two is evidence and not
/// a tautology.
Deviation predict_from_corners(const std::vector<std::vector<double>>& corners_per_sample,
                               double carrier_hz, double mix, int start) {
    const double omega = std::tan(std::numbers::pi * carrier_hz / kFs);
    std::vector<double> argument(corners_per_sample.size());
    for (std::size_t i = 0; i < corners_per_sample.size(); ++i) {
        double phi = 0.0;
        for (double fc : corners_per_sample[i]) {
            const double clamped = std::clamp(fc, 1.0, kFs * 0.49);
            phi -= 2.0 * std::atan(omega / std::tan(std::numbers::pi * clamped / kFs));
        }
        const std::complex<double> response =
            (1.0 - mix) + mix * std::polar(1.0, phi);
        argument[i] = std::arg(response);
    }
    unwrap(argument);

    Deviation result{-1e9, 1e9};
    for (std::size_t i = static_cast<std::size_t>(start); i + 1 < argument.size(); ++i) {
        const double slope = 0.5 * (argument[i + 1] - argument[i - 1]);
        const double cents =
            1200.0 * std::log2((carrier_hz + slope * kFs / kTwoPi) / carrier_hz);
        result.up_cents = std::max(result.up_cents, cents);
        result.down_cents = std::min(result.down_cents, cents);
    }
    return result;
}

/// Coherent magnitude of a static system at one frequency: RMS ratio over a
/// whole number of periods, after an equally long settling render. Exact for a
/// pure tone through an LTI system, and immune to the peak-sample under-read
/// that bites anyone who measures a 20 kHz tone at 48 kHz by its highest
/// sample.
template <typename Process>
double coherent_gain_db(Process process, double hz, double fs = kFs) {
    // 4800 samples is a whole number of periods for every test frequency below,
    // all of which are multiples of 10 Hz.
    constexpr int kWindow = 4800;
    for (int i = 0; i < kWindow; ++i) {
        process(std::sin(kTwoPi * hz * static_cast<double>(i) / fs));
    }
    double energy_in = 0.0;
    double energy_out = 0.0;
    for (int i = kWindow; i < 2 * kWindow; ++i) {
        const double x = std::sin(kTwoPi * hz * static_cast<double>(i) / fs);
        const double y = process(x);
        energy_in += x * x;
        energy_out += y * y;
    }
    return 10.0 * std::log10(energy_out / energy_in);
}

/// L1 norm of a static allpass cascade's impulse response — the exact worst-case
/// sample gain over all bounded inputs, and the honest ceiling for these
/// engines. Unity MAGNITUDE response does not bound sample gain: an allpass's
/// impulse response changes sign, so a sign-matched input accumulates.
double cascade_impulse_l1(const std::vector<double>& corners, int length = 200000) {
    std::vector<TptFilter64> stages(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        stages[i].prepare(kFs);
        stages[i].set_cutoff(corners[i]);
    }
    double l1 = 0.0;
    for (int i = 0; i < length; ++i) {
        double x = (i == 0) ? 1.0 : 0.0;
        for (auto& stage : stages) x = stage.process_allpass(x);
        l1 += std::abs(x);
    }
    return l1;
}

/// Peak output magnitude over a battery of unit-amplitude signals a real source
/// can produce: low square waves (the shape that drives an allpass hardest),
/// sines across the band, an impulse, and a step.
template <typename Make>
double battery_peak_gain(Make make) {
    double peak = 0.0;
    for (double hz : {20.0, 40.0, 55.0, 110.0, 220.0, 440.0}) {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) {
            const double x =
                std::sin(kTwoPi * hz * static_cast<double>(i) / kFs) >= 0.0 ? 1.0 : -1.0;
            peak = std::max(peak, std::abs(engine.process(x)));
        }
    }
    for (double hz : {30.0, 70.0, 200.0, 600.0, 2000.0, 6000.0}) {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) {
            peak = std::max(peak, std::abs(engine.process(
                                      std::sin(kTwoPi * hz * static_cast<double>(i) / kFs))));
        }
    }
    {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(engine.process(i == 0 ? 1.0 : 0.0)));
    }
    {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(engine.process(1.0)));
    }
    return peak;
}

/// Scalar face of the stereo Univibe, so the gain probes can treat all three
/// engines the same way. Returns the wet output in either mode.
struct UniVibeWetTap {
    UniVibe64 engine;
    double process(double x) {
        double left = 0.0;
        double right = 0.0;
        engine.process(x, left, right);
        return right;
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// The instrument, before anything is measured with it.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Pitch instrument recovers a known FM deviation", "[vibrato][instrument]") {
    // Synthetic tone with an exactly known peak deviation of beta*f_m Hz.
    constexpr double kBeta = 0.4225;
    const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);

    for (double carrier : {200.0, 1000.0, 4000.0}) {
        std::vector<double> x(cycle * kRenderCycles);
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double t = static_cast<double>(i) / kFs;
            x[i] = std::sin(kTwoPi * carrier * t + kBeta * std::sin(kTwoPi * kProbeRateHz * t));
        }
        const auto measured =
            peak_cents_deviation(instantaneous_phase(x, carrier),
                                 static_cast<int>(cycle * kSkipCycles), carrier, kProbeRateHz);

        const double expected_up =
            1200.0 * std::log2((carrier + kBeta * kProbeRateHz) / carrier);
        const double expected_down =
            1200.0 * std::log2((carrier - kBeta * kProbeRateHz) / carrier);
        // 0.5 % covers the one-period window's own sinc droop, which is largest
        // at the lowest carrier because the window is longest there.
        CHECK(measured.up_cents == Approx(expected_up).epsilon(0.005));
        CHECK(measured.down_cents == Approx(expected_down).epsilon(0.005));
    }

    // An unmodulated tone must read as no vibrato at all, or every "the engine
    // does nothing" branch below would pass for the wrong reason.
    const auto flat = peak_cents_deviation(instantaneous_phase(sine_buffer(cycle * kRenderCycles, 1000.0), 1000.0),
                                           static_cast<int>(cycle * kSkipCycles), 1000.0, kProbeRateHz);
    CHECK(std::abs(flat.up_cents) < 1e-3);
    CHECK(std::abs(flat.down_cents) < 1e-3);
}

// ─────────────────────────────────────────────────────────────────────────
// DelayVibratoT — the only engine that shifts pitch.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("DelayVibrato hits the cents depth its own constants predict", "[vibrato][delay]") {
    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.set_rate_hz(kProbeRateHz);
    engine.set_depth_cents(DelayVibrato64::kDefaultDepthCents);
    engine.reset();

    // Predicted from the SHIPPED amplitude, not from a restated literal:
    // d'(t) peaks at A*2*pi*f_m, and the frequency scale is 1 - d'(t).
    const double slope_peak =
        engine.modulation_amplitude_samples() / kFs * kTwoPi * engine.rate_hz();
    const double expected_up = 1200.0 * std::log2(1.0 + slope_peak);
    const double expected_down = 1200.0 * std::log2(1.0 - slope_peak);

    const auto measured = measure_engine(engine, 1000.0, kProbeRateHz);
    CHECK(measured.up_cents == Approx(expected_up).epsilon(0.02));
    CHECK(measured.down_cents == Approx(expected_down).epsilon(0.02));

    // The upward and downward excursions are NOT equal, and the asymmetry is a
    // property of the arithmetic rather than an artefact: a symmetric swing in
    // delay is an asymmetric swing in cents because cents are logarithmic.
    CHECK(std::abs(expected_down) > std::abs(expected_up));
}

TEST_CASE("DelayVibrato shifts every frequency by the same cents", "[vibrato][delay]") {
    // The normative distinction. A modulated tap scales frequency by the delay's
    // time derivative, which knows nothing about the frequency it is scaling.
    std::vector<Deviation> readings;
    for (double carrier : {200.0, 1000.0, 4000.0}) {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        readings.push_back(measure_engine(engine, carrier, kProbeRateHz));
    }

    for (std::size_t i = 1; i < readings.size(); ++i) {
        CHECK(readings[i].up_cents == Approx(readings[0].up_cents).epsilon(0.02));
        CHECK(readings[i].down_cents == Approx(readings[0].down_cents).epsilon(0.02));
    }
}

TEST_CASE("DelayVibrato pitch-modulates without amplitude-modulating",
          "[vibrato][delay][interpolator]") {
    // The reason the spec calls for a Lagrange-3 read rather than the delay
    // line's own two-point default. Both track pitch identically — the delay
    // trajectory sets that, not the interpolator — so the pitch tests above
    // cannot tell them apart. What separates them is magnitude flatness across
    // the fractional range: a two-point read loses roughly 0.3 dB at a half
    // sample by 4 kHz and over a dB by 8 kHz, which arrives as amplitude
    // modulation at the vibrato rate on exactly the material a lead line lives
    // in.
    //
    // Bounds are acceptance-class. The residual at 1 kHz is the measurement's
    // own floor (the one-period analysis window dips slightly under FM), so the
    // 1 kHz bound documents that floor rather than the interpolator.
    const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);
    const std::vector<std::pair<double, double>> limits{
        {1000.0, 0.15}, {4000.0, 0.20}, {8000.0, 0.60}};

    for (const auto& [carrier, limit_db] : limits) {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        const auto input = sine_buffer(cycle * kRenderCycles, carrier);
        std::vector<double> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) output[i] = engine.process(input[i]);
        CHECK(envelope_ripple_db(output, carrier, static_cast<int>(cycle * kSkipCycles)) <
              limit_db);
    }
}

TEST_CASE("DelayVibrato reports its exact latency", "[vibrato][delay][latency]") {
    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.reset();

    const auto expected = static_cast<std::size_t>(
        std::ceil(engine.base_delay_samples() + engine.modulation_amplitude_samples()));
    CHECK(engine.latency_samples() == expected);
    CHECK(engine.latency_samples() > 0);

    // Zero depth still costs the interpolator's base delay: a fractional read
    // cannot be free, and reporting 0 would misalign a host's compensation.
    DelayVibrato64 flat;
    flat.prepare(kFs);
    flat.set_depth_cents(0.0);
    flat.reset();
    CHECK(flat.latency_samples() > 0);
    CHECK(flat.base_delay_samples() == Approx(DelayVibrato64::kMinBaseDelaySamples));
}

TEST_CASE("DelayVibrato lifecycle delays then fades the depth in", "[vibrato][delay][lifecycle]") {
    constexpr double kDelayMs = 400.0;
    constexpr double kFadeMs = 600.0;
    const auto delay_samples = static_cast<int>(kDelayMs * 0.001 * kFs);
    const auto full_samples = static_cast<int>((kDelayMs + kFadeMs) * 0.001 * kFs);

    DelayVibrato64 engine;
    engine.prepare(kFs);
    engine.set_delay_ms(kDelayMs);
    engine.set_fade_in_ms(kFadeMs);
    engine.reset();

    // A bit-exactly periodic carrier: one tabulated period, tiled. Built this
    // way because `sin(2*pi*f*i/fs)` is NOT bit-periodic in i — the argument
    // differs by ulps between periods — and the assertion below is about exact
    // sample equality.
    constexpr int kPeriod = 100;  // 480 Hz at 48 kHz
    std::vector<double> period_table(kPeriod);
    for (int i = 0; i < kPeriod; ++i) {
        period_table[static_cast<std::size_t>(i)] =
            std::sin(kTwoPi * static_cast<double>(i) / static_cast<double>(kPeriod));
    }

    int first_moving = -1;
    int first_full = -1;
    std::vector<double> rendered(static_cast<std::size_t>(2 * full_samples));
    for (int i = 0; i < 2 * full_samples; ++i) {
        rendered[static_cast<std::size_t>(i)] =
            engine.process(period_table[static_cast<std::size_t>(i % kPeriod)]);
        if (first_moving < 0 && engine.depth_envelope() > 0.0) first_moving = i;
        if (first_full < 0 && engine.depth_envelope() >= 1.0) first_full = i;
    }

    // Both land one sample late against the nominal count. That is the
    // envelope's accumulator summing 1/N exactly N times and landing a few ulp
    // short of 1, not a policy difference — hence the spec's own +/-1 sample.
    CHECK(std::abs(first_moving - delay_samples) <= 1);
    CHECK(std::abs(first_full - (full_samples - 1)) <= 1);

    // Zero pitch deviation, stated as a property of the audio rather than of the
    // envelope: a constant delay applied to an exactly periodic input yields an
    // exactly periodic output. Any modulation at all breaks it.
    const int settle = static_cast<int>(std::ceil(engine.latency_samples())) + kPeriod;
    for (int i = settle; i + kPeriod < delay_samples; ++i) {
        REQUIRE(rendered[static_cast<std::size_t>(i)] ==
                rendered[static_cast<std::size_t>(i + kPeriod)]);
    }

    // And the same check must FAIL once the fade has finished, or it would be
    // passing because the instrument cannot see modulation rather than because
    // there is none.
    int broken = 0;
    for (int i = full_samples; i + kPeriod < 2 * full_samples; ++i) {
        if (rendered[static_cast<std::size_t>(i)] !=
            rendered[static_cast<std::size_t>(i + kPeriod)]) {
            ++broken;
        }
    }
    CHECK(broken > 0);
}

// ─────────────────────────────────────────────────────────────────────────
// PhaseVibratoT — Magnatone lineage. Moves notches, not pitch.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("PhaseVibrato wobble depends strongly on frequency", "[vibrato][phase]") {
    // The counterpart of the DelayVibrato frequency-independence test, at the
    // engine's documented default blend. The spec asks for at least 6 dB of
    // difference; the mechanism delivers roughly 34.
    auto measure = [](double carrier) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        return measure_engine(engine, carrier, kProbeRateHz);
    };

    const auto low = measure(200.0);
    const auto high = measure(4000.0);

    const double ratio_db = 20.0 * std::log10(low.span() / high.span());
    CHECK(ratio_db > 6.0);

    // Named the other way round too, so a future change that made the engine
    // frequency-independent could not pass by shrinking both readings.
    CHECK(low.up_cents > 10.0);
    CHECK(high.up_cents < 1.0);
}

TEST_CASE("PhaseVibrato matches an independent allpass phase model", "[vibrato][phase]") {
    // Independent ground truth: rebuild the corner trajectory from a separate
    // LFO and the shipped sweep formula, then predict the carrier's frequency
    // deviation from textbook allpass phase. Agreement means the engine really
    // is a two-stage allpass cascade swept the way the doc says.
    for (double carrier : {200.0, 4000.0}) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.reset();
        const auto measured = measure_engine(engine, carrier, kProbeRateHz);

        const auto cycle = static_cast<std::size_t>(kFs / kProbeRateHz);
        EffectLfoT<double> lfo;
        lfo.set_wave(LfoWave::sine);
        lfo.prepare(kFs);
        lfo.set_rate_hz(kProbeRateHz);
        lfo.reset();
        std::vector<std::vector<double>> corners(cycle * kRenderCycles);
        for (auto& sample : corners) {
            const double fc = PhaseVibrato64::kDefaultCenterHz *
                              std::exp2(PhaseVibrato64::kDefaultDepth *
                                        PhaseVibrato64::kSweepOctaves * lfo.next());
            sample.assign(static_cast<std::size_t>(PhaseVibrato64::kDefaultStageCount), fc);
        }
        const auto predicted =
            predict_from_corners(corners, carrier, PhaseVibrato64::kDefaultMix,
                                 static_cast<int>(cycle * kSkipCycles));

        CHECK(measured.up_cents == Approx(predicted.up_cents).epsilon(0.05));
        CHECK(measured.down_cents == Approx(predicted.down_cents).epsilon(0.05));
    }
}

TEST_CASE("PhaseVibrato two stages sweep deeper than one", "[vibrato][phase]") {
    auto measure = [](int stages) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kProbeRateHz);
        engine.set_mix(1.0);
        engine.set_stage_count(stages);
        engine.reset();
        return measure_engine(engine, 200.0, kProbeRateHz).span();
    };

    const double one = measure(1);
    const double two = measure(2);
    CHECK(two / one > 1.5);
    // Cascading identical stages adds phase linearly, so the honest expectation
    // is 2x, not merely "more". Asserting the real number would catch a stage
    // that silently stopped contributing where ">= 1.5" would not.
    CHECK(two / one == Approx(2.0).epsilon(0.02));
}

TEST_CASE("PhaseVibrato reports zero latency", "[vibrato][phase][latency]") {
    PhaseVibrato64 engine;
    engine.prepare(kFs);
    CHECK(engine.latency_samples() == 0);
    UniVibe64 vibe;
    vibe.prepare(kFs);
    CHECK(vibe.latency_samples() == 0);
}

// ─────────────────────────────────────────────────────────────────────────
// Allpass unity gain — series law 1, asserted rather than assumed.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Every allpass stage is unity gain across the band", "[vibrato][allpass]") {
    // All test frequencies are multiples of 10 Hz, so 4800 samples is a whole
    // number of periods for each and the RMS ratio is a coherent measurement.
    const std::vector<double> frequencies{20.0,   50.0,   100.0,  200.0,   500.0,
                                          1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
    const std::vector<double> corners{70.0, 100.0, 200.0, 500.0, 1900.0, 3800.0, 5700.0};

    for (double fc : corners) {
        for (double hz : frequencies) {
            TptFilter64 stage;
            stage.prepare(kFs);
            stage.set_cutoff(fc);
            const double db =
                coherent_gain_db([&stage](double x) { return stage.process_allpass(x); }, hz);
            CHECK(std::abs(db) < 0.05);
        }
    }

    // And the shipped cascades, held static, are unity too — the property has to
    // survive composition, not just hold per stage.
    for (double hz : frequencies) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_depth(0.0);
        engine.set_mix(1.0);
        engine.set_stage_count(PhaseVibrato64::kMaxStages);
        engine.reset();
        const double db =
            coherent_gain_db([&engine](double x) { return engine.process(x); }, hz);
        CHECK(std::abs(db) < 0.05);
    }
}

TEST_CASE("Allpass phase crosses ninety degrees at the corner", "[vibrato][allpass]") {
    // What makes a corner frequency mean something in phase terms, and therefore
    // what makes the Univibe's corner accessors testable claims rather than
    // labels.
    for (double fc : {200.0, 430.0, 900.0, 1900.0}) {
        TptFilter64 stage;
        stage.prepare(kFs);
        stage.set_cutoff(fc);
        const auto period = static_cast<std::size_t>(std::llround(kFs / 10.0));
        std::vector<double> out(period * 40);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = stage.process_allpass(std::sin(kTwoPi * fc * static_cast<double>(i) / kFs));
        }
        // Coherent single-bin phase over whole periods of the drive.
        double real = 0.0;
        double imag = 0.0;
        const std::size_t start = out.size() / 2;
        for (std::size_t i = start; i < out.size(); ++i) {
            const double angle = kTwoPi * fc * static_cast<double>(i) / kFs;
            real += out[i] * std::cos(angle);
            imag -= out[i] * std::sin(angle);
        }
        // Drive is sin, so the reference itself sits at -90 degrees in this
        // basis; the allpass adds another -90 at its corner.
        const double degrees = std::atan2(imag, real) * 180.0 / std::numbers::pi;
        CHECK(degrees == Approx(-180.0).margin(3.0));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// UniVibeT — staggered corners behind a vactrol.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("UniVibe corners stay staggered and track the shipped formula",
          "[vibrato][univibe]") {
    UniVibe64 engine;
    engine.prepare(kFs);
    engine.reset();

    for (int n = 0; n < 6000; ++n) {
        double left = 0.0;
        double right = 0.0;
        engine.process(0.0, left, right);
    }

    const double scale = UniVibe64::corner_scale(engine.control(), engine.depth());
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        const double expected =
            UniVibe64::kStageBaseHz[static_cast<std::size_t>(i)] * scale;
        CHECK(engine.stage_corner_hz(i) == Approx(expected).epsilon(1e-12));
    }

    // Unequal by a wide margin, at every instant, because all four ride one
    // shared scale. Pairwise, not just adjacent.
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        for (int j = i + 1; j < UniVibe64::kStageCount; ++j) {
            CHECK(engine.stage_corner_hz(j) / engine.stage_corner_hz(i) > 1.5);
        }
    }

    // The stagger is a set of ratios, and those ratios must not breathe with the
    // sweep — one dimensionless shape over four base frequencies.
    const double ratio_now = engine.stage_corner_hz(3) / engine.stage_corner_hz(0);
    for (int n = 0; n < 4000; ++n) {
        double left = 0.0;
        double right = 0.0;
        engine.process(0.0, left, right);
    }
    CHECK(engine.stage_corner_hz(3) / engine.stage_corner_hz(0) ==
          Approx(ratio_now).epsilon(1e-12));
    CHECK(ratio_now == Approx(UniVibe64::kStageBaseHz[3] / UniVibe64::kStageBaseHz[0])
                           .epsilon(1e-12));
}

TEST_CASE("UniVibe audio matches its staggered corner trajectory", "[vibrato][univibe]") {
    // End-to-end: the corners are not just computed, they are the ones the audio
    // actually passed through. Predicted from an independently driven LFO,
    // vactrol, and the textbook four-stage allpass phase.
    constexpr double kRate = 3.0;
    const auto cycle = static_cast<std::size_t>(kFs / kRate);

    for (double carrier : {200.0, 4000.0}) {
        UniVibe64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(kRate);
        engine.reset();

        const auto input = sine_buffer(cycle * 4, carrier);
        std::vector<double> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            double left = 0.0;
            double right = 0.0;
            engine.process(input[i], left, right);
            output[i] = left;
        }
        const auto measured =
            peak_cents_deviation(instantaneous_phase(output, carrier),
                                 static_cast<int>(cycle), carrier, kRate, 24);

        EffectLfoT<double> lfo;
        lfo.set_wave(LfoWave::sine);
        lfo.prepare(kFs);
        lfo.set_rate_hz(kRate);
        lfo.reset();
        VactrolConditioner64 vactrol;
        vactrol.prepare(kFs);
        vactrol.set_rise_ms(UniVibe64::kVactrolRiseMs);
        vactrol.set_fall_ms(UniVibe64::kVactrolFallMs);
        vactrol.reset();

        std::vector<std::vector<double>> corners(input.size());
        for (auto& sample : corners) {
            const double control = vactrol.process(lfo.next_unipolar());
            const double scale = UniVibe64::corner_scale(control, UniVibe64::kDefaultDepth);
            sample.resize(UniVibe64::kStageCount);
            for (int k = 0; k < UniVibe64::kStageCount; ++k) {
                sample[static_cast<std::size_t>(k)] =
                    UniVibe64::kStageBaseHz[static_cast<std::size_t>(k)] * scale;
            }
        }
        const auto predicted =
            predict_from_corners(corners, carrier, UniVibe64::kVibratoMix, static_cast<int>(cycle));

        CHECK(measured.up_cents == Approx(predicted.up_cents).epsilon(0.05));
        CHECK(measured.down_cents == Approx(predicted.down_cents).epsilon(0.05));
    }
}

TEST_CASE("UniVibe vactrol rises fast and falls slow", "[vibrato][univibe][vactrol]") {
    VactrolConditioner64 vactrol;
    vactrol.prepare(kFs);
    vactrol.set_rise_ms(UniVibe64::kVactrolRiseMs);
    vactrol.set_fall_ms(UniVibe64::kVactrolFallMs);
    vactrol.reset();

    auto crossing = [](double previous, double current, double level, int index) {
        return static_cast<double>(index) - 1.0 + (level - previous) / (current - previous);
    };

    double previous = vactrol.control();
    double t10 = 0.0;
    double t90 = 0.0;
    for (int i = 1; i < 48000; ++i) {
        const double current = vactrol.process(1.0);
        if (previous < 0.1 && current >= 0.1) t10 = crossing(previous, current, 0.1, i);
        if (previous < 0.9 && current >= 0.9) {
            t90 = crossing(previous, current, 0.9, i);
            break;
        }
        previous = current;
    }
    for (int i = 0; i < 48000; ++i) vactrol.process(1.0);

    previous = vactrol.control();
    double f90 = 0.0;
    double f10 = 0.0;
    for (int i = 1; i < 480000; ++i) {
        const double current = vactrol.process(0.0);
        if (previous > 0.9 && current <= 0.9) f90 = crossing(previous, current, 0.9, i);
        if (previous > 0.1 && current <= 0.1) {
            f10 = crossing(previous, current, 0.1, i);
            break;
        }
        previous = current;
    }

    const double rise_ms = (t90 - t10) / kFs * 1000.0;
    const double fall_ms = (f10 - f90) / kFs * 1000.0;

    // A one-pole's 10-90 % time is tau*ln(9), so the ratio of the two 10-90 %
    // times is exactly the ratio of the shipped time constants. Computed, not
    // restated.
    const double expected_ratio = UniVibe64::kVactrolFallMs / UniVibe64::kVactrolRiseMs;
    CHECK(fall_ms / rise_ms == Approx(expected_ratio).epsilon(0.05));
    CHECK(rise_ms == Approx(UniVibe64::kVactrolRiseMs * std::log(9.0)).epsilon(0.02));
    CHECK(fall_ms == Approx(UniVibe64::kVactrolFallMs * std::log(9.0)).epsilon(0.02));
}

TEST_CASE("UniVibe sweep is lopsided where the Magnatone sweep is not",
          "[vibrato][univibe][vactrol]") {
    // The vactrol has to be audible in the SHAPE of the sweep, not just present
    // in a step response. A symmetric sine drives both engines; only the one
    // with an asymmetric conditioner produces an asymmetric pitch excursion.
    constexpr double kRate = 3.0;

    UniVibe64 vibe;
    vibe.prepare(kFs);
    vibe.set_rate_hz(kRate);
    vibe.reset();
    const auto cycle = static_cast<std::size_t>(kFs / kRate);
    const auto input = sine_buffer(cycle * 4, 200.0);
    std::vector<double> wet(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        double left = 0.0;
        double right = 0.0;
        vibe.process(input[i], left, right);
        wet[i] = left;
    }
    const auto vibe_dev = peak_cents_deviation(instantaneous_phase(wet, 200.0),
                                               static_cast<int>(cycle), 200.0, kRate, 24);

    PhaseVibrato64 magnatone;
    magnatone.prepare(kFs);
    magnatone.set_rate_hz(kRate);
    magnatone.set_mix(1.0);
    magnatone.reset();
    const auto magnatone_dev = measure_engine(magnatone, 200.0, kRate);

    CHECK(vibe_dev.asymmetry() > 1.10);
    CHECK(magnatone_dev.asymmetry() == Approx(1.0).margin(0.03));
}

TEST_CASE("UniVibe chorus splits the paths and vibrato does not", "[vibrato][univibe][mode]") {
    const auto input = sine_buffer(24000, 1000.0);

    UniVibe64 chorus;
    chorus.prepare(kFs);
    chorus.set_mode(UniVibe64::Mode::chorus);
    chorus.reset();

    double sum_input = 0.0;
    double sum_difference = 0.0;
    for (double x : input) {
        double left = 0.0;
        double right = 0.0;
        chorus.process(x, left, right);
        // Documented split-path behaviour: one output is untouched. Bit-exact,
        // because "untouched" is a claim about identity, not about level.
        CHECK(left == x);
        sum_input += x * x;
        sum_difference += (right - x) * (right - x);
    }
    const double difference_db = 10.0 * std::log10(sum_difference / sum_input);
    CHECK(difference_db > -40.0);

    UniVibe64 vibrato;
    vibrato.prepare(kFs);
    vibrato.set_mode(UniVibe64::Mode::vibrato);
    vibrato.reset();
    bool moved = false;
    for (double x : input) {
        double left = 0.0;
        double right = 0.0;
        vibrato.process(x, left, right);
        CHECK(left == right);
        if (left != x) moved = true;
    }
    CHECK(moved);
}

// ─────────────────────────────────────────────────────────────────────────
// Gain bounds, determinism, and RT safety.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Lagrange kernel peak gain is the shipped constant", "[vibrato][delay][gain]") {
    // The reason DelayVibrato's worst-case sample gain is not 0 dB. Scanned over
    // the whole fractional range rather than assumed at the midpoint.
    double worst = 0.0;
    double worst_at = 0.0;
    for (int i = 0; i <= 100000; ++i) {
        const double frac = static_cast<double>(i) / 100000.0;
        const double l1 = std::abs(Interpolator::lagrange(frac, 1.0, 0.0, 0.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 1.0, 0.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 0.0, 1.0, 0.0)) +
                          std::abs(Interpolator::lagrange(frac, 0.0, 0.0, 0.0, 1.0));
        if (l1 > worst) {
            worst = l1;
            worst_at = frac;
        }
    }
    CHECK(worst == Approx(DelayVibrato64::kInterpolatorPeakGain).epsilon(1e-9));
    CHECK(worst_at == Approx(0.5).margin(1e-4));
}

TEST_CASE("Worst-case sample gain stays under the cascade L1 ceiling", "[vibrato][gain]") {
    // Series law 8: the registry number has to be a bound this suite asserts.
    //
    // The ceiling is the L1 norm of the impulse response at the LOWEST corner
    // each engine's parameter range can reach, with every stage active and the
    // path fully wet — the exact worst-case sample gain of a static cascade over
    // all bounded inputs. The unity-MAGNITUDE property bounds steady-state
    // sinusoids only; it says nothing about sample gain, and an allpass
    // amplifies a sign-matched input by exactly this factor.
    const double phase_low_corner =
        PhaseVibrato64::kMinCenterHz * std::exp2(-PhaseVibrato64::kSweepOctaves);
    const double phase_ceiling = cascade_impulse_l1(std::vector<double>(
        static_cast<std::size_t>(PhaseVibrato64::kMaxStages), phase_low_corner));

    const double vibe_scale = UniVibe64::corner_scale(0.0, 1.0);
    std::vector<double> vibe_corners(UniVibe64::kStageCount);
    for (int i = 0; i < UniVibe64::kStageCount; ++i) {
        vibe_corners[static_cast<std::size_t>(i)] =
            UniVibe64::kStageBaseHz[static_cast<std::size_t>(i)] * vibe_scale;
    }
    const double vibe_ceiling = cascade_impulse_l1(vibe_corners);

    // Both ceilings sit far above the +6 dB an unnormalised direct+shifted sum
    // would suggest. That is the point of measuring instead of quoting.
    CHECK(phase_ceiling > 5.0);
    CHECK(vibe_ceiling > 5.0);

    const double phase_peak = battery_peak_gain([] {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_center_hz(PhaseVibrato64::kMinCenterHz);
        engine.set_depth(1.0);
        engine.set_stage_count(PhaseVibrato64::kMaxStages);
        engine.set_mix(1.0);
        engine.set_rate_hz(PhaseVibrato64::kMinRateHz);
        engine.reset();
        return engine;
    });
    CHECK(phase_peak < phase_ceiling);
    CHECK(phase_peak > 2.0);  // and genuinely above the sinusoidal bound

    const double vibe_peak = battery_peak_gain([] {
        UniVibeWetTap tap;
        tap.engine.prepare(kFs);
        tap.engine.set_depth(1.0);
        tap.engine.set_rate_hz(UniVibe64::kMinRateHz);
        tap.engine.reset();
        return tap;
    });
    CHECK(vibe_peak < vibe_ceiling);
    CHECK(vibe_peak > 2.0);

    const double delay_peak = battery_peak_gain([] {
        DelayVibrato64 engine;
        engine.prepare(kFs);
        engine.set_rate_hz(DelayVibrato64::kMinRateHz);
        engine.set_depth_cents(DelayVibrato64::kMaxDepthCents);
        engine.reset();
        return engine;
    });
    CHECK(delay_peak < DelayVibrato64::kInterpolatorPeakGain);
    CHECK(delay_peak > 1.0);  // not 0 dB, despite being one unit-gain tap

    // Steady-state sinusoids ARE bounded by 1 for a crossfade of unity-magnitude
    // paths, which is the claim the allpass property actually supports.
    for (double hz : {100.0, 1000.0, 10000.0}) {
        PhaseVibrato64 engine;
        engine.prepare(kFs);
        engine.set_depth(0.0);
        engine.set_mix(1.0);
        engine.reset();
        const double db =
            coherent_gain_db([&engine](double x) { return engine.process(x); }, hz);
        CHECK(db <= 20.0 * std::log10(PhaseVibrato64::kSinusoidalGainBound) + 0.05);
    }
}

TEST_CASE("Vibrato engines render deterministically", "[vibrato][determinism]") {
    const auto input = sine_buffer(8000, 220.0);

    auto render_delay = [&input] {
        DelayVibrato delay;
        delay.prepare(kFs);
        delay.set_delay_ms(30.0);
        delay.set_fade_in_ms(50.0);
        delay.reset();
        std::vector<float> out(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            out[i] = delay.process(static_cast<float>(input[i]));
        }
        return out;
    };
    CHECK(render_delay() == render_delay());

    auto render_phase = [&input] {
        PhaseVibrato phase;
        phase.prepare(kFs);
        phase.reset();
        std::vector<float> out(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            out[i] = phase.process(static_cast<float>(input[i]));
        }
        return out;
    };
    CHECK(render_phase() == render_phase());

    auto render_vibe = [&input] {
        UniVibe vibe;
        vibe.prepare(kFs);
        vibe.set_mode(UniVibe::Mode::chorus);
        vibe.reset();
        std::vector<float> out(input.size() * 2);
        for (std::size_t i = 0; i < input.size(); ++i) {
            vibe.process(static_cast<float>(input[i]), out[2 * i], out[2 * i + 1]);
        }
        return out;
    };
    CHECK(render_vibe() == render_vibe());

    // Determinism has to survive a reset mid-stream, which is what a host does
    // between transport stops.
    DelayVibrato reused;
    reused.prepare(kFs);
    reused.reset();
    std::vector<float> first(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        first[i] = reused.process(static_cast<float>(input[i]));
    }
    reused.reset();
    std::vector<float> second(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        second[i] = reused.process(static_cast<float>(input[i]));
    }
    CHECK(first == second);
}

TEST_CASE("Vibrato engines allocate nothing after prepare", "[vibrato][rt]") {
    DelayVibrato delay;
    PhaseVibrato phase;
    UniVibe vibe;
    delay.prepare(kFs);
    phase.prepare(kFs);
    vibe.prepare(kFs);

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 4096; ++i) {
        const auto x = static_cast<float>(std::sin(kTwoPi * 440.0 * i / kFs));
        float left = 0.0f;
        float right = 0.0f;
        delay.process(x);
        phase.process(x);
        vibe.process(x, left, right);
    }

    // Every setter on the RT surface, including the ones that re-derive
    // coefficients or re-arm the lifecycle.
    delay.set_rate_hz(7.0);
    delay.set_depth_cents(45.0);
    delay.set_delay_ms(120.0);
    delay.set_fade_in_ms(250.0);
    phase.set_rate_hz(4.0);
    phase.set_depth(0.9);
    phase.set_center_hz(1200.0);
    phase.set_stage_count(4);
    phase.set_mix(0.25);
    vibe.set_rate_hz(6.0);
    vibe.set_depth(0.4);
    vibe.set_mode(UniVibe::Mode::chorus);
    delay.reset();
    phase.reset();
    vibe.reset();

    CHECK(probe.allocation_count() == 0);
}

TEST_CASE("Vibrato parameter setters clamp to their declared ranges", "[vibrato][params]") {
    DelayVibrato64 delay;
    delay.prepare(kFs);
    delay.set_rate_hz(1e6);
    CHECK(delay.rate_hz() == Approx(DelayVibrato64::kMaxRateHz));
    delay.set_rate_hz(-1.0);
    CHECK(delay.rate_hz() == Approx(DelayVibrato64::kMinRateHz));
    delay.set_depth_cents(1e6);
    CHECK(delay.depth_cents() == Approx(DelayVibrato64::kMaxDepthCents));

    // The delay line is sized at prepare() for the worst legal case, so the
    // slowest rate at the deepest setting must still read inside it.
    delay.set_rate_hz(DelayVibrato64::kMinRateHz);
    delay.set_depth_cents(DelayVibrato64::kMaxDepthCents);
    delay.reset();
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        const double y = delay.process(std::sin(kTwoPi * 110.0 * i / kFs));
        finite = finite && std::isfinite(y) && std::abs(y) <= 2.0;
    }
    CHECK(finite);

    PhaseVibrato64 phase;
    phase.prepare(kFs);
    phase.set_stage_count(99);
    CHECK(phase.stage_count() == PhaseVibrato64::kMaxStages);
    phase.set_stage_count(-3);
    CHECK(phase.stage_count() == 1);
    phase.set_center_hz(10.0);
    CHECK(phase.center_hz() == Approx(PhaseVibrato64::kMinCenterHz));

    UniVibe64 vibe;
    vibe.prepare(kFs);
    vibe.set_rate_hz(0.0);
    CHECK(vibe.rate_hz() == Approx(UniVibe64::kMinRateHz));
    vibe.set_depth(5.0);
    CHECK(vibe.depth() == Approx(1.0));
}
TEST_CASE("all vibrato engines reject non-finite controls and audio",
          "[signal][vibrato][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        DelayVibrato64 da, db; for(auto* v:{&da,&db}){v->prepare(kFs);v->set_rate_hz(4.1);v->set_depth_cents(37);v->set_delay_ms(11);v->set_fade_in_ms(23);v->reset();}
        da.set_rate_hz(bad); da.set_depth_cents(bad); da.set_delay_ms(bad); da.set_fade_in_ms(bad);
        REQUIRE(da.process(bad)==0); db.reset(); for(int i=0;i<64;++i) REQUIRE(da.process(.2)==db.process(.2));
        PhaseVibrato64 pa,pb; for(auto* v:{&pa,&pb}){v->prepare(kFs);v->set_rate_hz(3.2);v->set_depth(.6);v->set_center_hz(777);v->set_mix(.7);v->reset();}
        pa.set_rate_hz(bad);pa.set_depth(bad);pa.set_center_hz(bad);pa.set_mix(bad);REQUIRE(pa.process(bad)==0);pb.reset();for(int i=0;i<64;++i)REQUIRE(pa.process(.2)==pb.process(.2));
        UniVibe64 ua,ub;for(auto* v:{&ua,&ub}){v->prepare(kFs);v->set_rate_hz(2.4);v->set_depth(.8);v->reset();}ua.set_rate_hz(bad);ua.set_depth(bad);double l=1,r=1;ua.process(bad,l,r);REQUIRE(l==0);REQUIRE(r==0);ub.reset();for(int i=0;i<64;++i){double bl=0,br=0;ua.process(.2,l,r);ub.process(.2,bl,br);REQUIRE(l==bl);REQUIRE(r==br);}
    }
}
