// LeslieRotaryT and ScannerVibratoT — the two mechanical modulators.
//
// Everything here is measured out of rendered audio rather than read off a
// setter, and every expectation is computed from the shipped constants. The
// module's claims are physical claims — a pitch deviation of a stated depth, a
// rotor arriving at a stated time, a band split at a stated corner — so a test
// that only checked that a parameter round-tripped would prove none of them.
//
// One instrument does most of the work: complex demodulation at a known carrier
// gives BOTH the amplitude envelope (the tremolo) and the instantaneous
// frequency (the Doppler) from a single pass, which is exactly the pair of
// quantities this module modulates.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/leslie.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Leslie = LeslieRotaryT<double>;
using Scanner = ScannerVibratoT<double>;

constexpr double kSr = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

// ── The instrument ────────────────────────────────────────────────────────

/// One pass of complex demodulation at `carrier_hz`: multiply by
/// `e^{-j2*pi*f0*t}`, low-pass both quadratures, and read off the magnitude and
/// the phase derivative.
///
/// Chosen over peak-tracking on the waveform for the reason the series keeps
/// relearning: a peak detector on a modulated carrier samples the modulator
/// only at the carrier's peaks, so it measures the modulation ALIASED down to
/// the beat between the two, and the answer moves when the carrier moves.
/// Demodulation has no such coupling.
struct Demodulated {
    std::vector<double> envelope;  ///< |analytic|, at `rate_hz`.
    std::vector<double> freq_hz;   ///< Instantaneous frequency, at `rate_hz`.
    double rate_hz = 0.0;          ///< The decimated rate the traces live at.
};

/// `lowpass_hz` must pass the modulation and reject the image at twice the
/// carrier. The image is what limits it: a residual image of relative amplitude
/// `a` puts a ripple of `a*2*f0` Hz on the instantaneous-frequency trace, which
/// is small against a Leslie's tens-of-Hz deviation and NOT small against a
/// scanner's, so the two callers pass different corners on purpose.
Demodulated demodulate(const std::vector<double>& x, double carrier_hz, double input_rate_hz,
                       double lowpass_hz, int decimation) {
    Demodulated out;
    out.rate_hz = input_rate_hz / decimation;
    const double pole = std::exp(-kTwoPi * lowpass_hz / input_rate_hz);
    double state_i[4] = {0.0, 0.0, 0.0, 0.0};
    double state_q[4] = {0.0, 0.0, 0.0, 0.0};
    double previous_phase = 0.0;
    bool have_previous = false;

    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = kTwoPi * carrier_hz * static_cast<double>(n) / input_rate_hz;
        double i = x[n] * std::cos(w);
        double q = -x[n] * std::sin(w);
        for (int k = 0; k < 4; ++k) {
            state_i[k] = pole * state_i[k] + (1.0 - pole) * i;
            i = state_i[k];
            state_q[k] = pole * state_q[k] + (1.0 - pole) * q;
            q = state_q[k];
        }
        if (static_cast<int>(n) % decimation != 0) continue;

        out.envelope.push_back(2.0 * std::hypot(i, q));
        const double phase = std::atan2(q, i);
        if (have_previous) {
            double d = phase - previous_phase;
            while (d > M_PI) d -= kTwoPi;
            while (d < -M_PI) d += kTwoPi;
            out.freq_hz.push_back(carrier_hz + d * out.rate_hz / kTwoPi);
        }
        previous_phase = phase;
        have_previous = true;
    }
    return out;
}

/// Coherent DFT of a trace at an arbitrary frequency — not an FFT bin, so the
/// probe frequency can be the exact rate a constant predicts instead of the
/// nearest bin to it.
std::complex<double> coherent(const std::vector<double>& x, double hz, double rate_hz,
                             std::size_t begin) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = begin; n < x.size(); ++n) {
        const double w = kTwoPi * hz * static_cast<double>(n) / rate_hz;
        acc += x[n] * std::complex<double>(std::cos(w), -std::sin(w));
    }
    return acc * (2.0 / static_cast<double>(x.size() - begin));
}

/// The frequency of the strongest component in a band, by scanning the coherent
/// DFT on a fine grid. Resolution is the grid, not the render length, so a rate
/// can be pinned far below one FFT bin.
double locate_peak(const std::vector<double>& x, double lo_hz, double hi_hz, double rate_hz,
                   int steps) {
    double best_hz = lo_hz;
    double best_mag = -1.0;
    for (int k = 0; k <= steps; ++k) {
        const double hz = lo_hz + (hi_hz - lo_hz) * k / steps;
        const double mag = std::abs(coherent(x, hz, rate_hz, 0));
        if (mag > best_mag) {
            best_mag = mag;
            best_hz = hz;
        }
    }
    return best_hz;
}

std::vector<double> remove_mean(const std::vector<double>& x) {
    double sum = 0.0;
    for (double v : x) sum += v;
    const double mean = sum / static_cast<double>(x.size());
    std::vector<double> y = x;
    for (auto& v : y) v -= mean;
    return y;
}

/// Median of `|trace − centre|` over the settled tail.
///
/// The right statistic for a SQUARE-wave deviation, which is what a linearly
/// ramping delay produces: the plateau is the physical quantity and the
/// turnarounds are edges. Taking the peak instead reads the edge transient and
/// reports 2–4 % high, by an amount that depends only on the measurement filter
/// — an artefact of the instrument, not of the model.
double median_deviation(const std::vector<double>& trace, double centre) {
    std::vector<double> d;
    for (std::size_t i = trace.size() / 3; i < trace.size(); ++i)
        d.push_back(std::abs(trace[i] - centre));
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

/// Peak of `|trace − centre|` over the settled tail. The right statistic for a
/// SINE deviation, which is what a rotating source produces.
double peak_deviation(const std::vector<double>& trace, double centre) {
    double peak = 0.0;
    for (std::size_t i = trace.size() / 3; i < trace.size(); ++i)
        peak = std::max(peak, std::abs(trace[i] - centre));
    return peak;
}

/// Per-cycle rate of an oscillating trace, from interpolated upward zero
/// crossings: `{time_seconds, hz}` for each completed cycle.
///
/// Used where the trace's frequency is SWEEPING, which is the one case
/// demodulation handles badly — a narrow enough filter to reject the image is
/// also narrow enough to reject the sweep's far end, and a wide enough one lets
/// the image back in. Zero crossings have no centre frequency to be detuned
/// from, so they track a rate from rest to full speed with the same fidelity.
///
/// Interpolating the crossing rather than taking the sample index matters: at a
/// 1 kHz trace rate a 6 Hz cycle is 167 samples, so rounding to the nearest
/// sample would quantise the measured rate by 0.6 %.
std::vector<std::pair<double, double>> cycle_rates(const std::vector<double>& trace,
                                                   double rate_hz) {
    std::vector<std::pair<double, double>> out;
    double previous_crossing = -1.0;
    for (std::size_t n = 1; n < trace.size(); ++n) {
        if (!(trace[n - 1] <= 0.0 && trace[n] > 0.0)) continue;
        const double frac = trace[n] != trace[n - 1]
                                ? -trace[n - 1] / (trace[n] - trace[n - 1])
                                : 0.0;
        const double t = (static_cast<double>(n - 1) + frac) / rate_hz;
        if (previous_crossing >= 0.0) out.emplace_back(t, 1.0 / (t - previous_crossing));
        previous_crossing = t;
    }
    return out;
}

// ── Fixtures ──────────────────────────────────────────────────────────────

Leslie make_leslie(LeslieSpeed speed = LeslieSpeed::tremolo) {
    Leslie l;
    l.prepare(kSr);
    l.set_speed(speed);
    l.reset();
    return l;
}

/// A cabinet with everything except the rotors' motion switched off, so one
/// mechanism at a time can be measured.
Leslie make_bare_leslie(LeslieSpeed speed = LeslieSpeed::tremolo) {
    Leslie l = make_leslie(speed);
    l.set_am_depth(0.0);
    l.set_dir_depth_db(0.0);
    l.set_drum_dir_depth_db(0.0);
    l.set_reflection_db(-60.0);
    l.reset();
    return l;
}

enum class Channel { left, right, sum };

std::vector<double> render_tone(Leslie& l, double hz, double amplitude, double seconds,
                                Channel channel) {
    const int n = static_cast<int>(kSr * seconds);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(amplitude * std::sin(kTwoPi * hz * i / kSr), a, b);
        out.push_back(channel == Channel::left ? a
                                               : (channel == Channel::right ? b : 0.5 * (a + b)));
    }
    return out;
}

/// The magnitude of the cabinet's mic-sum response at one frequency, by steady
/// tone and coherent detection.
double response_at(Leslie& l, double hz, double seconds = 0.4) {
    const int n = static_cast<int>(kSr * seconds);
    const int skip = n / 2;
    double re = 0.0;
    double im = 0.0;
    for (int i = 0; i < n; ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(std::sin(kTwoPi * hz * i / kSr), a, b);
        if (i >= skip) {
            const double w = kTwoPi * hz * i / kSr;
            re += (a + b) * std::cos(w);
            im += (a + b) * std::sin(w);
        }
    }
    // The mic sum of a unit-amplitude tone; the extra 0.5 folds the two mics
    // back to one so an unmodulated cabinet reads 0 dB.
    return std::hypot(re, im) / static_cast<double>(n - skip);
}

/// The peak Doppler deviation the shipped geometry predicts, evaluated from the
/// module's OWN delay function rather than from a formula restated here — so
/// the expectation cannot drift from the implementation's geometry, only from
/// its physics.
double geometric_doppler_ratio(const Leslie& l, double rotor_hz, bool drum) {
    double peak = 0.0;
    constexpr int kSteps = 20000;
    constexpr double kDt = 1e-6;
    for (int k = 0; k < kSteps; ++k) {
        const double theta = static_cast<double>(k) / kSteps;
        const double d0 =
            drum ? l.drum_delay_seconds(theta, false) : l.horn_delay_seconds(theta, false);
        const double d1 = drum ? l.drum_delay_seconds(theta + rotor_hz * kDt, false)
                               : l.horn_delay_seconds(theta + rotor_hz * kDt, false);
        peak = std::max(peak, std::abs((d1 - d0) / kDt));
    }
    return peak;
}

}  // namespace

// ── 1. Doppler ────────────────────────────────────────────────────────────

TEST_CASE("the rotating source produces the Doppler depth its geometry implies",
          "[leslie][doppler]") {
    // The spec's compact figure is the FAR-FIELD limit `r*omega/c`. The shipped
    // model solves the exact law of cosines instead, so the two only have to
    // agree where the mic is far compared with the radius — which is where the
    // defaults sit. Checking them against each other is a check on the physics;
    // checking the render against the exact one is a check on the code.
    Leslie l = make_bare_leslie();
    const double far_field =
        Leslie::kHornRadiusM * kTwoPi * Leslie::kHornFastHz / 343.0;
    const double exact = geometric_doppler_ratio(l, Leslie::kHornFastHz, false);
    REQUIRE_THAT(exact, WithinRel(far_field, 0.01));

    // A 4 kHz carrier, not the 1 kHz the spec's recipe names. At 1 kHz an
    // 800 Hz LR4 crossover still passes the drum band at −11 dB, so a
    // "horn-only" measurement there is a tenth drum — and the drum runs at a
    // different rate and radius, which is exactly what would corrupt this. Two
    // octaves up the drum band is below −55 dB and the horn is alone.
    constexpr double kCarrierHz = 4000.0;
    const auto rendered = render_tone(l, kCarrierHz, 0.5, 3.0, Channel::left);
    const auto d = demodulate(rendered, kCarrierHz, kSr, 400.0, 48);
    const double measured = peak_deviation(d.freq_hz, kCarrierHz) / kCarrierHz;

    REQUIRE_THAT(measured, WithinRel(exact, 0.05));

    // The same radius at the chorale rate is an order of magnitude shallower —
    // the whole reason the two speeds are voiced by rate alone and never by a
    // second depth curve (series law 7).
    Leslie slow = make_bare_leslie(LeslieSpeed::chorale);
    const double slow_exact = geometric_doppler_ratio(slow, Leslie::kHornSlowHz, false);
    REQUIRE(slow_exact < exact * 0.2);
    REQUIRE_THAT(slow_exact / exact,
                 WithinRel(Leslie::kHornSlowHz / Leslie::kHornFastHz, 0.01));

    // The drum is deliberately shallower than the horn at the same speed
    // setting: smaller radius AND lower rate.
    const double drum_exact = geometric_doppler_ratio(l, Leslie::kDrumFastHz, true);
    REQUIRE(drum_exact < exact);
}

// ── 2. Rotor rate ─────────────────────────────────────────────────────────

TEST_CASE("the horn's tremolo lands exactly on the configured rate", "[leslie][rate]") {
    // The amplitude modulation is locked to the horn's phase, so measuring the
    // envelope's rate measures the phase accumulator through the whole audio
    // path. Resolution comes from the scan grid rather than from an FFT bin, so
    // 30 s is enough to resolve far below the 0.1 % the spec asks for.
    Leslie l = make_leslie();
    const auto rendered = render_tone(l, 4000.0, 0.5, 30.0, Channel::left);
    const auto d = demodulate(rendered, 4000.0, kSr, 400.0, 48);
    const auto env = remove_mean(d.envelope);

    const double measured = locate_peak(env, Leslie::kHornFastHz * 0.98,
                                        Leslie::kHornFastHz * 1.02, d.rate_hz, 4000);
    REQUIRE_THAT(measured, WithinRel(Leslie::kHornFastHz, 0.001));
}

TEST_CASE("the two rotors beat instead of locking together", "[leslie][rate]") {
    // The signature of the class. Both rotors modulate the same mic, at rates
    // that differ on purpose, so the mic sum's envelope carries their
    // difference frequency. A model that shared one LFO between the rotors
    // would show nothing here.
    Leslie l = make_leslie();
    const auto rendered = render_tone(l, 1000.0, 0.5, 40.0, Channel::sum);
    const auto d = demodulate(rendered, 1000.0, kSr, 400.0, 48);
    const auto env = remove_mean(d.envelope);

    const double expected = std::abs(Leslie::kHornFastHz - Leslie::kDrumFastHz);
    const double measured = locate_peak(env, expected * 0.7, expected * 1.3, d.rate_hz, 3000);
    REQUIRE_THAT(measured, WithinRel(expected, 0.05));

    // Not a vacuous find: the beat is a real component, not the scan settling
    // on noise at the edge of its window.
    REQUIRE(std::abs(coherent(env, measured, d.rate_hz, 0)) >
            10.0 * std::abs(coherent(env, expected * 0.55, d.rate_hz, 0)));
}

// ── 3. Inertia — the gesture ──────────────────────────────────────────────

TEST_CASE("the horn reaches a new speed before the drum does", "[leslie][inertia]") {
    // THE behaviour. A speed change is not one ramp, it is two of different
    // lengths, and the gap between them is what a listener hears as the swirl
    // rising and the low end smearing in behind it.
    //
    // Measured from the audio in each band separately: a 4 kHz carrier sits in
    // the horn band and a 150 Hz carrier in the drum band, so each render's
    // envelope carries exactly one rotor's rate. The settling time is then the
    // last moment that envelope's own cycle rate was still outside 5 % of its
    // new target.
    //
    // The directivity shelf is switched off so the envelope is pure amplitude
    // modulation, whose mean is `1 − depth/2` of the carrier regardless of the
    // rotor's rate — which is what lets one mean serve as the zero-crossing
    // reference across a sweep from rest to full speed.
    const auto settle_seconds = [](bool drum) {
        const double carrier = drum ? 150.0 : 4000.0;
        const double target = drum ? Leslie::kDrumFastHz : Leslie::kHornFastHz;

        Leslie l = make_leslie(LeslieSpeed::chorale);
        l.set_reflection_db(-60.0);
        l.set_dir_depth_db(0.0);
        l.set_drum_dir_depth_db(0.0);
        l.reset();

        const int pre = static_cast<int>(kSr * 2.0);
        const int post = static_cast<int>(kSr * 14.0);
        std::vector<double> rendered;
        rendered.reserve(static_cast<std::size_t>(post));
        for (int i = 0; i < pre + post; ++i) {
            if (i == pre) l.set_speed(LeslieSpeed::tremolo);
            double a = 0.0;
            double b = 0.0;
            l.process(0.5 * std::sin(kTwoPi * carrier * i / kSr), a, b);
            if (i >= pre) rendered.push_back(a);
        }

        // The demodulation corner is a fixed fraction of the carrier, not a
        // fixed frequency: the image sits at twice the carrier, so a corner that
        // is comfortable under a 4 kHz carrier sits ABOVE the image of a 150 Hz
        // one and fills the envelope with it. A tenth of the carrier clears the
        // image at both ends and still passes a 6 Hz modulation.
        const auto d = demodulate(rendered, carrier, kSr, carrier * 0.1, 48);
        const auto rates = cycle_rates(remove_mean(d.envelope), d.rate_hz);
        REQUIRE(rates.size() > 10);

        // Walk back from the end: the arrival is the last moment the rate was
        // still outside the band, so a rate that happens to sweep through the
        // band on its way up cannot be mistaken for having settled there.
        const double tolerance = 0.05 * target;
        std::size_t index = rates.size();
        while (index > 0 && std::abs(rates[index - 1].second - target) < tolerance) --index;
        return index == 0 ? 0.0 : rates[index - 1].first;
    };

    const double horn = settle_seconds(false);
    const double drum = settle_seconds(true);

    INFO("horn settled at " << horn << " s, drum at " << drum << " s");

    // Each rotor arrives on the schedule its own shipped constant sets. The
    // constants are "time to close `kSettleFraction` of the change", so this is
    // the number the header promises, not a derived one.
    REQUIRE_THAT(horn, WithinAbs(Leslie::kHornAccelS, 0.6));
    REQUIRE_THAT(drum, WithinAbs(Leslie::kDrumAccelS, 1.2));

    // And the asymmetry itself, which is the part that matters: the drum is
    // still catching up well after the horn has arrived.
    REQUIRE(drum > horn + 1.0);
    REQUIRE_THAT(drum / horn, WithinRel(Leslie::kDrumAccelS / Leslie::kHornAccelS, 0.5));
}

TEST_CASE("the inertia ramp is a time constant, not a fixed slide time",
          "[leslie][inertia]") {
    // Mechanical inertia has a constant TIME CONSTANT: a bigger speed change
    // takes proportionally longer. The other slew law available — constant
    // total time — would cover any distance in the same wall clock, which is a
    // portamento, not a flywheel. Asserted by comparing a small change against
    // a large one: under a constant time constant the two reach the SAME
    // FRACTION of their own change at the same moment.
    const auto fraction_after = [](double from_hz, double to_hz, double seconds) {
        Leslie l = make_leslie(LeslieSpeed::stop);
        l.set_horn_slow_hz(from_hz);
        l.set_horn_fast_hz(to_hz);
        l.set_speed(LeslieSpeed::chorale);
        l.reset();
        l.set_speed(LeslieSpeed::tremolo);
        double a = 0.0;
        double b = 0.0;
        for (int i = 0; i < static_cast<int>(kSr * seconds); ++i) l.process(0.0, a, b);
        return (l.horn_rate_hz() - from_hz) / (to_hz - from_hz);
    };

    const double small = fraction_after(6.0, 6.5, 0.5);
    const double large = fraction_after(0.6, 7.0, 0.5);
    REQUIRE_THAT(small, WithinRel(large, 0.02));

    // ...and that shared fraction is the one the one-pole predicts at that
    // instant, computed from the shipped accel time and settle fraction.
    const double tau = Leslie::kHornAccelS / -std::log(1.0 - Leslie::kSettleFraction);
    REQUIRE_THAT(small, WithinRel(1.0 - std::exp(-0.5 / tau), 0.02));
}

TEST_CASE("braking freezes the modulation rather than muting it", "[leslie][inertia]") {
    // `stop` is a third colour, not silence: the rotors coast to rest and the
    // horn parks facing wherever it stopped, so the tone holds bright or dark
    // depending on phase. Nothing crossfades to a bypass — a stopped rotor IS
    // the stationary path, because a phase that stops advancing freezes the
    // delay, the beam gain and the shelf together.
    Leslie l = make_leslie(LeslieSpeed::tremolo);
    double a = 0.0;
    double b = 0.0;
    for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i)
        l.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);

    l.set_speed(LeslieSpeed::stop);
    for (int i = 0; i < static_cast<int>(kSr * 20.0); ++i)
        l.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);

    REQUIRE_THAT(l.horn_rate_hz(), WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(l.drum_rate_hz(), WithinAbs(0.0, 1e-3));

    const double parked = l.horn_phase();
    const auto rendered = render_tone(l, 4000.0, 0.5, 2.0, Channel::left);
    REQUIRE_THAT(l.horn_phase(), WithinAbs(parked, 1e-12));

    // Frozen, not muted: the cabinet still passes signal at a level set by
    // wherever the horn happens to be pointing.
    double energy = 0.0;
    for (double v : rendered) energy += v * v;
    REQUIRE(std::sqrt(energy / static_cast<double>(rendered.size())) > 0.05);
}

// ── 4. Crossover ──────────────────────────────────────────────────────────

TEST_CASE("the crossover splits at its stated corner", "[leslie][crossover]") {
    // Located from OUTSIDE the box, using the rotors themselves as band labels:
    // the horn band's content is amplitude-modulated at the horn rate and the
    // drum band's at the drum rate, so the frequency where the envelope carries
    // equal energy at the two rates is the frequency where the two band gains
    // are equal — which for an LR4 is the crossover.
    //
    // The Doppler is switched off for the measurement (radius 0) on purpose.
    // With it on, the two bands reach the mic through delays that differ by
    // their differing radii, their relative phase is modulated, and that
    // interference adds an amplitude term at BOTH rotor rates which biases the
    // crossing — it reads 8.9 % low. Zero radius removes the difference without
    // touching the split being measured.
    const auto locate_crossover = [](double asked_hz) {
        double lo = asked_hz * 0.4;
        double hi = asked_hz * 2.5;
        for (int iteration = 0; iteration < 16; ++iteration) {
            const double mid = std::sqrt(lo * hi);
            Leslie l = make_leslie();
            l.set_crossover_hz(asked_hz);
            l.set_dir_depth_db(0.0);
            l.set_drum_dir_depth_db(0.0);
            l.set_reflection_db(-60.0);
            l.set_horn_radius_m(0.0);
            l.set_drum_radius_m(0.0);
            l.reset();

            const auto rendered = render_tone(l, mid, 0.5, 10.0, Channel::left);
            const auto d = demodulate(rendered, mid, kSr, 200.0, 48);
            const auto env = remove_mean(d.envelope);
            const std::size_t skip = env.size() / 5;
            const double horn = std::abs(coherent(env, Leslie::kHornFastHz, d.rate_hz, skip));
            const double drum = std::abs(coherent(env, Leslie::kDrumFastHz, d.rate_hz, skip));
            if (horn > drum) hi = mid; else lo = mid;
        }
        return std::sqrt(lo * hi);
    };

    REQUIRE_THAT(locate_crossover(Leslie::kCrossoverHz), WithinRel(Leslie::kCrossoverHz, 0.03));
    // It is the parameter that moves it, not a coincidence at one value.
    REQUIRE_THAT(locate_crossover(500.0), WithinRel(500.0, 0.03));
}

TEST_CASE("the crossover itself is inaudible", "[leslie][crossover]") {
    // §3.4's claim: an LR4 pair recombines flat, so with the rotors stopped the
    // split leaves no trace. Asserted two ways, because the absolute version
    // runs into a limit that has nothing to do with the crossover.
    const auto neutral = [](double crossover_hz) {
        Leslie l = make_leslie(LeslieSpeed::stop);
        l.set_crossover_hz(crossover_hz);
        l.set_am_depth(0.0);
        l.set_dir_depth_db(0.0);
        l.set_drum_dir_depth_db(0.0);
        l.set_reflection_db(-60.0);
        l.set_mix(1.0);
        l.reset();
        return l;
    };

    // (a) The crossover's OWN contribution, across the full audio band. Moving
    // the corner from one end of its range to the other leaves every other
    // stage — the fractional-delay read, the shelves, the DC blocker —
    // bit-identical, so the ratio of the two responses isolates the crossover
    // and nothing else. A flat ratio is exactly the claim "the split is
    // inaudible", and it holds where the absolute version cannot.
    {
        Leslie low = neutral(700.0);
        Leslie high = neutral(900.0);
        double worst_db = 0.0;
        for (double hz = 20.0; hz <= 20000.0; hz *= 1.15) {
            const double ratio_db =
                units::linear_to_db(response_at(high, hz) / response_at(low, hz));
            worst_db = std::max(worst_db, std::abs(ratio_db));
        }
        INFO("worst crossover-dependent deviation: " << worst_db << " dB");
        REQUIRE(worst_db < 0.5);
    }

    // (b) The absolute flatness, up to the ceiling the SIGNAL PATH allows.
    //
    // The spec asks for +/-0.5 dB from 20 Hz to 20 kHz. That is not reachable
    // by any implementation of this chain, and the crossover is not the reason:
    // every rotor read is a fractional delay, and the 4-point Lagrange kernel
    // the catalog uses for modulated delays is −0.23 dB at 8 kHz, −0.53 dB at
    // 10 kHz and −3.3 dB at 16 kHz at its worst fractional offset. The
    // interpolator alone spends the whole budget before 10 kHz. The band
    // asserted here is therefore the one the claim can be true over, and the
    // ceiling is stated rather than quietly widened tolerance.
    {
        constexpr double kFlatnessCeilingHz = 8000.0;
        Leslie l = neutral(Leslie::kCrossoverHz);
        double worst_db = 0.0;
        double worst_hz = 0.0;
        for (double hz = 20.0; hz <= kFlatnessCeilingHz; hz *= 1.15) {
            const double db = units::linear_to_db(response_at(l, hz));
            if (std::abs(db) > std::abs(worst_db)) {
                worst_db = db;
                worst_hz = hz;
            }
        }
        INFO("worst absolute deviation " << worst_db << " dB at " << worst_hz << " Hz");
        REQUIRE(std::abs(worst_db) < 0.5);
    }
}

// ── 5. The stereo mic pair ────────────────────────────────────────────────

TEST_CASE("the mic pair produces a real stereo image, not a doubled mono",
          "[leslie][stereo]") {
    // Two mics at different angles see the horn face them at different phases,
    // so their tremolos run at the same rate with a phase offset — and that
    // offset IS the included angle, converted from degrees to a fraction of a
    // rotor revolution. Asserting the angle rather than merely "L differs from
    // R" is what makes this a test of the geometry instead of a test that some
    // decorrelation happened.
    const auto measured_offset_deg = [](double angle_deg) {
        Leslie l = make_leslie();
        l.set_mic_angle_deg(angle_deg);
        l.set_reflection_db(-60.0);
        l.reset();

        const int n = static_cast<int>(kSr * 10.0);
        std::vector<double> left;
        std::vector<double> right;
        left.reserve(static_cast<std::size_t>(n));
        right.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            double a = 0.0;
            double b = 0.0;
            l.process(0.5 * std::sin(kTwoPi * 4000.0 * i / kSr), a, b);
            left.push_back(a);
            right.push_back(b);
        }

        const auto dl = demodulate(left, 4000.0, kSr, 400.0, 48);
        const auto dr = demodulate(right, 4000.0, kSr, 400.0, 48);
        const auto el = remove_mean(dl.envelope);
        const auto er = remove_mean(dr.envelope);
        const std::size_t skip = el.size() / 5;
        double phase = std::arg(coherent(er, Leslie::kHornFastHz, dr.rate_hz, skip)) -
                       std::arg(coherent(el, Leslie::kHornFastHz, dl.rate_hz, skip));
        while (phase > M_PI) phase -= kTwoPi;
        while (phase < -M_PI) phase += kTwoPi;
        return std::abs(phase) * 180.0 / M_PI;
    };

    // The default: the two tremolos are the mic angle apart in rotor phase.
    REQUIRE_THAT(measured_offset_deg(Leslie::kMicAngleDeg),
                 WithinAbs(Leslie::kMicAngleDeg, 2.0));
    // Quadrature at the wide end — the "two mics on the cabinet" setup.
    REQUIRE_THAT(measured_offset_deg(90.0), WithinAbs(90.0, 2.0));
    // And it collapses when the mics are told to share an angle, which proves
    // the width comes from the geometry rather than from a fixed widener.
    REQUIRE_THAT(measured_offset_deg(0.0), WithinAbs(0.0, 2.0));

    // The collapse is total: at 0° the two mics are the same signal.
    Leslie mono = make_leslie();
    mono.set_mic_angle_deg(0.0);
    mono.reset();
    for (int i = 0; i < 4800; ++i) {
        double a = 0.0;
        double b = 0.0;
        mono.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);
        REQUIRE_THAT(a, WithinAbs(b, 1e-12));
    }
}

// ── 6. Scanner vibrato ────────────────────────────────────────────────────

TEST_CASE("the scanner shifts pitch by the slope of its own sweep",
          "[scanner][vibrato]") {
    // A linearly ramping delay is a CONSTANT pitch shift, so the depth follows
    // from the ramp's slope and nothing else: the scanner crosses `depth·line`
    // seconds of delay in each half period. The module publishes that
    // prediction and the render has to match it.
    const auto measure = [](ScannerMode mode) {
        Scanner s;
        s.prepare(kSr);
        s.set_mode(mode);
        s.reset();

        constexpr double kCarrierHz = 1000.0;
        const int n = static_cast<int>(kSr * 3.0);
        std::vector<double> rendered;
        rendered.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            rendered.push_back(s.process(0.5 * std::sin(kTwoPi * kCarrierHz * i / kSr)));

        // 120 Hz is deliberate: wide enough to pass a 6.9 Hz square and its
        // first dozen harmonics, narrow enough that the image at 2 kHz cannot
        // put a ripple on the frequency trace. At 300 Hz that image alone reads
        // as 11 % of extra depth in the shallowest mode.
        const auto d = demodulate(rendered, kCarrierHz, kSr, 120.0, 24);
        return median_deviation(d.freq_hz, kCarrierHz) / kCarrierHz;
    };

    const double v1 = measure(ScannerMode::v1);
    const double v2 = measure(ScannerMode::v2);
    const double v3 = measure(ScannerMode::v3);

    Scanner reference;
    reference.prepare(kSr);
    for (auto mode : {ScannerMode::v1, ScannerMode::v2, ScannerMode::v3}) {
        reference.set_mode(mode);
        const double predicted = reference.peak_pitch_shift_ratio();
        const double got = mode == ScannerMode::v1 ? v1 : (mode == ScannerMode::v2 ? v2 : v3);
        REQUIRE_THAT(got, WithinRel(predicted, 0.05));
    }

    // The switch is a depth ladder in the V positions.
    REQUIRE(v1 < v2);
    REQUIRE(v2 < v3);

    // And the depths are the shipped fractions of one line, not three
    // independently tuned curves.
    REQUIRE_THAT(v1 / v3, WithinRel(Scanner::kV1 / Scanner::kV3, 0.05));
    REQUIRE_THAT(v2 / v3, WithinRel(Scanner::kV2 / Scanner::kV3, 0.05));
}

TEST_CASE("the chorus positions are a delay comb, not a level blend",
          "[scanner][chorus]") {
    // V and C are DIFFERENT EFFECTS, not two depths of one. A V position moves
    // the pitch and leaves the level alone; a C position adds the dry back, so
    // a stationary-pitch copy interferes with a moving-pitch one and the result
    // is a comb. Freezing the scanner mid-sweep makes that comb stand still
    // where its null can be located and compared with the delay that must have
    // produced it.
    const double mid_delay_s = Scanner::kV3 * Scanner::kLineDelayMs * 0.001 * 0.5;
    const double expected_null_hz = 1.0 / (2.0 * mid_delay_s);

    const auto magnitude_at = [](double hz) {
        Scanner s;
        s.prepare(kSr);
        s.set_mode(ScannerMode::c3);
        s.set_scan_hz(0.0);  // frozen at mid-sweep: phase 0 is the triangle's centre
        s.reset();
        const int n = static_cast<int>(kSr * 0.5);
        const int skip = n / 2;
        double re = 0.0;
        double im = 0.0;
        for (int i = 0; i < n; ++i) {
            const double y = s.process(std::sin(kTwoPi * hz * i / kSr));
            if (i >= skip) {
                re += y * std::cos(kTwoPi * hz * i / kSr);
                im += y * std::sin(kTwoPi * hz * i / kSr);
            }
        }
        return 2.0 * std::hypot(re, im) / static_cast<double>(n - skip);
    };

    double null_hz = 0.0;
    double null_mag = 1e9;
    for (double hz = expected_null_hz * 0.8; hz <= expected_null_hz * 1.2; hz += 5.0) {
        const double m = magnitude_at(hz);
        if (m < null_mag) {
            null_mag = m;
            null_hz = hz;
        }
    }
    REQUIRE_THAT(null_hz, WithinRel(expected_null_hz, 0.02));

    // A genuine cancellation, not a dip: the null is far below the passband
    // peaks either side of it. A level blend could not do this at all.
    const double away = magnitude_at(expected_null_hz * 0.5);
    REQUIRE(null_mag < 0.1 * away);

    // The matching V position has no comb at all — the pitch moves, the
    // magnitude does not.
    Scanner vibrato;
    vibrato.prepare(kSr);
    vibrato.set_mode(ScannerMode::v3);
    vibrato.set_scan_hz(0.0);
    vibrato.reset();
    double re = 0.0;
    double im = 0.0;
    const int n = static_cast<int>(kSr * 0.5);
    for (int i = 0; i < n; ++i) {
        const double y = vibrato.process(std::sin(kTwoPi * expected_null_hz * i / kSr));
        if (i >= n / 2) {
            re += y * std::cos(kTwoPi * expected_null_hz * i / kSr);
            im += y * std::sin(kTwoPi * expected_null_hz * i / kSr);
        }
    }
    const double vibrato_mag = 2.0 * std::hypot(re, im) / static_cast<double>(n - n / 2);
    REQUIRE(vibrato_mag > 0.9);
}

TEST_CASE("the scanner's off position is a bit-exact bypass", "[scanner]") {
    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::off);
    s.reset();
    for (int i = 0; i < 4800; ++i) {
        const double x = 0.7 * std::sin(kTwoPi * 220.0 * i / kSr);
        REQUIRE(s.process(x) == x);
    }
}

TEST_CASE("the 50 Hz-mains rate is derived from the 60 Hz one", "[scanner]") {
    // Rather than inventing a second unverified number: the scanner is geared
    // off the mains-synchronous motor, so the European rate is the same gearing
    // at a different mains frequency.
    REQUIRE_THAT(Scanner::kScanHz50, WithinRel(Scanner::kScanHz * 50.0 / 60.0, 1e-12));
}

// ── 7. The feedforward gain bound ─────────────────────────────────────────

TEST_CASE("the constructive-sum bound holds across the parameter space",
          "[leslie][scanner][gain]") {
    // Neither engine has a feedback path, so there is no loop gain to bound —
    // series law 1 is satisfied by structure, not by compensation. What the
    // registry needs instead is the worst constructive sum, and law 8 says it
    // must be a TESTED invariant. This is that test; the registry cites its
    // measured maximum, with the shipped constant as the budget.
    double leslie_max = 0.0;
    for (auto speed : {LeslieSpeed::stop, LeslieSpeed::chorale, LeslieSpeed::tremolo}) {
        for (double radius : {0.10, 0.25}) {
            for (double am : {0.0, 0.9}) {
                for (double dir : {0.0, 12.0}) {
                    for (double refl : {-60.0, -6.0}) {
                        for (double mix : {0.5, 1.0}) {
                            Leslie l;
                            l.prepare(kSr);
                            l.set_speed(speed);
                            l.set_horn_radius_m(radius);
                            l.set_drum_radius_m(std::min(radius, 0.18));
                            l.set_am_depth(am);
                            l.set_dir_depth_db(dir);
                            l.set_drum_dir_depth_db(std::min(dir, 6.0));
                            l.set_reflection_db(refl);
                            l.set_num_reflections(Leslie::kMaxReflections);
                            l.set_mix(mix);
                            l.reset();

                            double peak_in = 0.0;
                            double peak_out = 0.0;
                            for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i) {
                                // An impulse to excite every tap, then a
                                // full-scale sweep to find where they align.
                                double x = 0.0;
                                if (i < 64) {
                                    x = i == 0 ? 1.0 : 0.0;
                                } else {
                                    const double t = (i - 64) / kSr;
                                    x = std::sin(kTwoPi * 20.0 * std::pow(1000.0, t / 0.9) * t);
                                }
                                double a = 0.0;
                                double b = 0.0;
                                l.process(x, a, b);
                                peak_in = std::max(peak_in, std::abs(x));
                                peak_out = std::max({peak_out, std::abs(a), std::abs(b)});
                            }
                            leslie_max = std::max(leslie_max, peak_out / peak_in);
                        }
                    }
                }
            }
        }
    }
    INFO("Forge registry worst_case_gain for LeslieRotary: " << leslie_max);
    REQUIRE(leslie_max <= Leslie::kWorstCaseGain);
    // Not a vacuous ceiling — the sweep gets within reach of it.
    REQUIRE(leslie_max > 0.5 * Leslie::kWorstCaseGain);

    double scanner_max = 0.0;
    for (auto mode : {ScannerMode::off, ScannerMode::v3, ScannerMode::c1, ScannerMode::c3}) {
        for (double hz : {0.0, 7.5}) {
            for (double line : {0.6, 1.4}) {
                for (double chorus : {0.0, 0.5, 1.0}) {
                    Scanner s;
                    s.prepare(kSr);
                    s.set_mode(mode);
                    s.set_scan_hz(hz);
                    s.set_line_ms(line);
                    s.set_chorus_mix(chorus);
                    s.reset();
                    double peak_in = 0.0;
                    double peak_out = 0.0;
                    for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i) {
                        double x = 0.0;
                        if (i < 64) {
                            x = i == 0 ? 1.0 : 0.0;
                        } else {
                            const double t = (i - 64) / kSr;
                            x = std::sin(kTwoPi * 20.0 * std::pow(1000.0, t / 0.9) * t);
                        }
                        peak_in = std::max(peak_in, std::abs(x));
                        peak_out = std::max(peak_out, std::abs(s.process(x)));
                    }
                    scanner_max = std::max(scanner_max, peak_out / peak_in);
                }
            }
        }
    }
    INFO("Forge registry worst_case_gain for ScannerVibrato: " << scanner_max);
    REQUIRE(scanner_max <= Scanner::kWorstCaseGain);
    REQUIRE(scanner_max > 0.5 * Scanner::kWorstCaseGain);
}

// ── 8. Determinism, latency, sizing, parity ───────────────────────────────

TEST_CASE("render, reset, re-render is bit-identical", "[leslie][scanner][determinism]") {
    // Including the seeded drift path, which is the only randomness in either
    // engine and is rewound by `reset()` (series law 2).
    for (double drift : {0.0, Leslie::kDriftCents}) {
        Leslie l = make_leslie();
        l.set_drift_cents(drift);
        l.set_seed(20260725u);
        l.reset();

        const auto render = [&](int n) {
            std::vector<double> out;
            out.reserve(static_cast<std::size_t>(2 * n));
            for (int i = 0; i < n; ++i) {
                // Program material, plus a param move partway through, so the
                // determinism claim covers automation and not just a static
                // render.
                if (i == n / 2) l.set_speed(LeslieSpeed::chorale);
                const double t = i / kSr;
                double a = 0.0;
                double b = 0.0;
                l.process(0.4 * std::sin(kTwoPi * 220.0 * t) + 0.3 * std::sin(kTwoPi * 1310.0 * t),
                          a, b);
                out.push_back(a);
                out.push_back(b);
            }
            return out;
        };

        const auto first = render(static_cast<int>(kSr * 2));
        l.set_speed(LeslieSpeed::tremolo);
        l.reset();
        const auto second = render(static_cast<int>(kSr * 2));
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
    }

    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::c2);
    s.reset();
    const auto render = [&](int n) {
        std::vector<double> out;
        for (int i = 0; i < n; ++i)
            out.push_back(s.process(0.5 * std::sin(kTwoPi * 330.0 * i / kSr)));
        return out;
    };
    const auto a = render(static_cast<int>(kSr * 2));
    s.reset();
    const auto b = render(static_cast<int>(kSr * 2));
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("both engines report zero latency and align sample-for-sample",
          "[leslie][scanner][latency]") {
    // Zero, and honestly so: every delay inside is a modelled acoustic or
    // electrical path, not processing latency. The claim is checked against the
    // bypass alignment rather than trusted.
    REQUIRE(Leslie::latency_samples() == 0);
    REQUIRE(Scanner::latency_samples() == 0);

    Leslie l = make_leslie();
    l.set_mix(0.0);
    l.reset();
    for (int i = 0; i < 512; ++i) {
        const double x = i == 7 ? 1.0 : 0.0;
        double a = 0.0;
        double b = 0.0;
        l.process(x, a, b);
        REQUIRE_THAT(a, WithinAbs(x, 1e-12));
        REQUIRE_THAT(b, WithinAbs(x, 1e-12));
    }

    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::off);
    s.reset();
    for (int i = 0; i < 512; ++i) {
        const double x = i == 7 ? 1.0 : 0.0;
        REQUIRE(s.process(x) == x);
    }
}

TEST_CASE("buffers are sized from the parameter ranges, not the current setting",
          "[leslie][scanner][sizing]") {
    // Series law 6, applied to storage: `prepare` must size for the WORST case
    // any later `set_*` can ask for, because a `set_*` is not allowed to
    // allocate. The Leslie's worst rotor read is at maximum radius and is
    // independent of mic distance — `L_max − L_min = (d+r) − (d−r) = 2r`, so
    // the distance cancels — while its longest read overall is the last
    // reflection tap.
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const double doppler_ms =
            2.0 * Leslie::kMaxRadiusM / 343.0 * 1000.0 + Leslie::kMaxDBiasMs;
        const double reflection_ms =
            Leslie::kMaxReflDelayMs +
            static_cast<double>(Leslie::kMaxReflections - 1) * Leslie::kMaxReflSpacingMs;
        const double longest_ms = std::max(doppler_ms, reflection_ms);
        REQUIRE(static_cast<double>(Leslie::worst_case_delay_samples(sr)) >=
                longest_ms * 0.001 * sr);
        REQUIRE(static_cast<double>(Scanner::worst_case_delay_samples(sr)) >=
                Scanner::kMaxLineDelayMs * 0.001 * sr);
    }

    // And the sizing survives contact with the extremes: prepared at defaults,
    // then pushed to every maximum without a re-prepare, the cabinet still
    // produces finite, bounded output.
    Leslie l;
    l.prepare(kSr);
    l.set_horn_radius_m(Leslie::kMaxRadiusM);
    l.set_drum_radius_m(Leslie::kMaxRadiusM);
    l.set_d_bias_ms(Leslie::kMaxDBiasMs);
    l.set_mic_distance_m(0.3);
    l.set_refl_delay_ms(Leslie::kMaxReflDelayMs);
    l.set_refl_spacing_ms(Leslie::kMaxReflSpacingMs);
    l.set_num_reflections(Leslie::kMaxReflections);
    l.set_reflection_db(-6.0);
    l.reset();
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(std::sin(kTwoPi * 100.0 * i / kSr), a, b);
        REQUIRE(std::isfinite(a));
        REQUIRE(std::isfinite(b));
        REQUIRE(std::abs(a) <= Leslie::kWorstCaseGain);
        REQUIRE(std::abs(b) <= Leslie::kWorstCaseGain);
    }
}

TEST_CASE("the float and double instantiations agree", "[leslie][scanner]") {
    // Guards against a `SampleType`-dependent constant leaking in — the
    // modulation maths is deliberately all in double so the two differ only by
    // the audio path's own precision.
    LeslieRotaryT<float> single;
    LeslieRotaryT<double> dbl;
    single.prepare(kSr);
    dbl.prepare(kSr);
    single.reset();
    dbl.reset();
    for (int i = 0; i < 24000; ++i) {
        const double x = 0.6 * std::sin(kTwoPi * 330.0 * i / kSr);
        float fa = 0.0f;
        float fb = 0.0f;
        double da = 0.0;
        double db = 0.0;
        single.process(static_cast<float>(x), fa, fb);
        dbl.process(x, da, db);
        REQUIRE_THAT(static_cast<double>(fa), WithinAbs(da, 1e-3));
        REQUIRE_THAT(static_cast<double>(fb), WithinAbs(db, 1e-3));
    }

    ScannerVibratoT<float> s_single;
    ScannerVibratoT<double> s_dbl;
    s_single.prepare(kSr);
    s_dbl.prepare(kSr);
    s_single.set_mode(ScannerMode::c3);
    s_dbl.set_mode(ScannerMode::c3);
    s_single.reset();
    s_dbl.reset();
    for (int i = 0; i < 24000; ++i) {
        const double x = 0.6 * std::sin(kTwoPi * 330.0 * i / kSr);
        REQUIRE_THAT(static_cast<double>(s_single.process(static_cast<float>(x))),
                     WithinAbs(s_dbl.process(x), 1e-3));
    }
}

// ── 9. RT allocation probe ────────────────────────────────────────────────

TEST_CASE("neither engine allocates on the audio thread", "[leslie][scanner][rt-safety]") {
    LeslieRotaryT<float> leslie_f;
    LeslieRotaryT<double> leslie_d;
    ScannerVibratoT<float> scanner_f;
    ScannerVibratoT<double> scanner_d;
    leslie_f.prepare(kSr);
    leslie_d.prepare(kSr);
    scanner_f.prepare(kSr);
    scanner_d.prepare(kSr);
    leslie_f.set_drift_cents(LeslieRotaryT<float>::kDriftCents);
    leslie_d.set_drift_cents(LeslieRotaryT<double>::kDriftCents);

    std::vector<float> in_f(256, 0.1f);
    std::vector<float> out_a(256, 0.0f);
    std::vector<float> out_b(256, 0.0f);
    std::vector<double> in_d(256, 0.1);
    std::vector<double> out_c(256, 0.0);
    std::vector<double> out_e(256, 0.0);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 32; ++i) {
            const auto speed = static_cast<LeslieSpeed>(i % 3);
            leslie_f.set_speed(speed);
            leslie_f.set_horn_fast_hz(5.5 + 0.05 * i);
            leslie_f.set_drum_fast_hz(4.5 + 0.05 * i);
            leslie_f.set_horn_accel_s(0.3 + 0.05 * i);
            leslie_f.set_drum_accel_s(1.0 + 0.1 * i);
            leslie_f.set_crossover_hz(700.0 + 5.0 * i);
            leslie_f.set_horn_radius_m(0.10 + 0.004 * i);
            leslie_f.set_drum_radius_m(0.08 + 0.003 * i);
            leslie_f.set_mic_distance_m(0.3 + 0.08 * i);
            leslie_f.set_mic_angle_deg(1.0 * i);
            leslie_f.set_am_depth(0.02 * i);
            leslie_f.set_dir_depth_db(0.3 * i);
            leslie_f.set_drum_dir_depth_db(0.15 * i);
            leslie_f.set_dir_corner_hz(1000.0 + 90.0 * i);
            leslie_f.set_d_bias_ms(0.2 + 0.02 * i);
            leslie_f.set_reflection_db(-60.0 + 1.6 * i);
            leslie_f.set_num_reflections(1 + i % 4);
            leslie_f.set_refl_delay_ms(2.5 + 0.1 * i);
            leslie_f.set_refl_spacing_ms(1.0 + 0.06 * i);
            leslie_f.set_refl_corner_hz(1000.0 + 200.0 * i);
            leslie_f.set_drift_cents(0.3 * i);
            leslie_f.set_mix(0.03 * i);
            leslie_d.set_speed(speed);
            leslie_d.set_mix(0.03 * i);

            scanner_f.set_mode(static_cast<ScannerMode>(i % 7));
            scanner_f.set_scan_hz(6.0 + 0.04 * i);
            scanner_f.set_line_ms(0.6 + 0.02 * i);
            scanner_f.set_v1_frac(0.1 + 0.01 * i);
            scanner_f.set_v2_frac(0.4 + 0.01 * i);
            scanner_f.set_v3_frac(0.7 + 0.009 * i);
            scanner_f.set_chorus_mix(0.03 * i);
            scanner_d.set_mode(static_cast<ScannerMode>(i % 7));

            float la = 0.0f;
            float lb = 0.0f;
            leslie_f.process(0.5f, la, lb);
            leslie_f.process_block(in_f.data(), out_a.data(), out_b.data(),
                                   static_cast<int>(in_f.size()));
            double da = 0.0;
            double db = 0.0;
            leslie_d.process(0.5, da, db);
            leslie_d.process_block(in_d.data(), out_c.data(), out_e.data(),
                                   static_cast<int>(in_d.size()));
            (void)scanner_f.process(0.5f);
            scanner_f.process_block(in_f.data(), out_a.data(), static_cast<int>(in_f.size()));
            (void)scanner_d.process(0.5);
            scanner_d.process_block(in_d.data(), out_c.data(), static_cast<int>(in_d.size()));
        }
        leslie_f.reset();
        leslie_d.reset();
        scanner_f.reset();
        scanner_d.reset();
    });
}
TEST_CASE("Leslie and scanner recover from non-finite audio with controls retained",
          "[signal][leslie][nonfinite]") {
    for(double bad:{NAN,INFINITY,-INFINITY}){
        Leslie a,b;for(auto* x:{&a,&b}){x->prepare(kSr);x->set_horn_fast_hz(7);x->set_drum_fast_hz(5);x->set_mic_distance_m(1.7);x->set_mix(.73);x->reset();}
        a.set_horn_fast_hz(bad);a.set_drum_fast_hz(bad);a.set_mic_distance_m(bad);a.set_mix(bad);double l=1,r=1;a.process(bad,.2,l,r);REQUIRE(l==0);REQUIRE(r==0);b.reset();for(int i=0;i<64;++i){double bl=0,br=0;a.process(.2,.2,l,r);b.process(.2,.2,bl,br);REQUIRE(l==bl);REQUIRE(r==br);}
        Scanner sa,sb;for(auto* x:{&sa,&sb}){x->prepare(kSr);x->set_scan_hz(7.1);x->set_line_ms(1.3);x->set_v1_frac(.2);x->set_v2_frac(.5);x->set_v3_frac(.8);x->set_chorus_mix(.4);x->reset();}sa.set_scan_hz(bad);sa.set_line_ms(bad);sa.set_v1_frac(bad);sa.set_v2_frac(bad);sa.set_v3_frac(bad);sa.set_chorus_mix(bad);REQUIRE(sa.process(bad)==0);sb.reset();for(int i=0;i<64;++i)REQUIRE(sa.process(.2)==sb.process(.2));
    }
}
