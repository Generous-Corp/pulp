// ChorusEnsembleT — the chorus family's acceptance suite.
//
// The spec's tests 1–11 (chorus-pulp-module-prompt.md, "Acceptance tests").
// Every expected value is computed from the shipped calibration table at test
// time; nothing is restated as a bare literal, so retuning a constant moves the
// test that documents it instead of silently disagreeing with it.
//
// ── How this suite measures, and why ──────────────────────────────────────
//
// The load-bearing measurements are all about a signal the module does not
// output: the instantaneous delay of each tap. Two instruments are used, and
// the order they appear in matters.
//
//   1. **The click train** (test 1). A unit impulse every 50 ms, mix = 1, and
//      the arrival of each impulse's delayed copy located to sub-sample
//      precision. This measures the delay actually applied to AUDIO, through
//      the real interpolator, and it is the only instrument that can.
//
//   2. **`current_delay_ms()`** (tests 2–5). Exact, cheap, and sampled per
//      sample rather than per click. On its own it would be an accessor
//      agreeing with itself, which is why test 1 spends its budget proving the
//      accessor and the click train agree before anything else relies on it.
//
// The click train alone cannot carry tests 2–5: it samples the delay trace at
// 20 Hz, and taking a min or max of a 20 Hz-sampled TRIANGLE under-reads its
// corner by up to `4 · (1 / (2 · samples_per_cycle))` of the depth — 12 % at
// the CE-2's 1.2 Hz, 5.1 % at the Juno's 0.513 Hz. Both dwarf the ±2 % the
// delay-range criterion allows. So test 1 does not take a min or max of the
// raw trace either: it fits the trace against the independently written
// reference LFO by least squares, which uses every sample and is unbiased.
// The residual of that fit is itself asserted, so a wrong SHAPE fails the test
// rather than hiding inside a two-parameter fit.
//
// The same reasoning kills naive peak-picking everywhere else in this file:
// frequency-response points are coherent DFTs over a whole number of periods,
// never the largest sample of a rendered sine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/chorus_family.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Chorus = ChorusEnsembleT<double>;
using Voicing = Chorus::Voicing;
using JunoMode = Chorus::JunoMode;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// ── Reference modulation, written out independently of the header ─────────
//
// `LfoT` advances before it reads, so the i-th processed sample (i counted from
// 1) sees phase `i · rate / fs + offset`. Both shapes are transcribed from the
// LFO contract, not called through it: a reference that called the shipped LFO
// would agree with the module by construction.

double ref_wrap(double cycles) {
    cycles -= std::floor(cycles);
    return cycles < 1.0 ? cycles : 0.0;
}

double ref_triangle(double phi) {
    phi = ref_wrap(phi);
    if (phi < 0.25) return 4.0 * phi;
    if (phi < 0.75) return 2.0 - 4.0 * phi;
    return 4.0 * phi - 4.0;
}

double ref_sine(double phi) { return std::sin(2.0 * kPi * ref_wrap(phi)); }

/// The modulation the spec says voice `k` of a voicing should carry at sample
/// index `i` (1-based), in `[-1, +1]`.
double reference_modulation(Voicing v, JunoMode mode, int k, long long i, double rate_hz) {
    const auto cal = Chorus::calibration(v);
    const double offset = static_cast<double>(k) / static_cast<double>(cal.voices);
    const double n = static_cast<double>(i);

    if (v == Voicing::juno_ensemble) {
        const auto spec = Chorus::juno_spec(mode);
        const double a = ref_triangle(n * spec.rate_a_hz / kSr + offset);
        if (!spec.dual) return a;
        const double b = ref_triangle(n * spec.rate_b_hz / kSr + offset);
        return std::clamp(0.5 * (a + b), -1.0, 1.0);
    }
    if (v == Voicing::dimension_d)
        return std::clamp(Chorus::kTrapK * ref_triangle(n * rate_hz / kSr + offset), -1.0, 1.0);
    if (cal.wave == LfoWave::sine) return ref_sine(n * rate_hz / kSr + offset);
    return ref_triangle(n * rate_hz / kSr + offset);
}

/// Centre delay and full-depth excursion the shipped tables give a voicing.
struct Window {
    double center_ms;
    double depth_ms;
    double rate_hz;
};

Window shipped_window(Voicing v, JunoMode mode) {
    const auto cal = Chorus::calibration(v);
    if (v == Voicing::juno_ensemble) {
        const auto spec = Chorus::juno_spec(mode);
        return {spec.center_ms, spec.depth_ms, spec.rate_a_hz};
    }
    return {cal.center_ms, cal.depth_ms, cal.rate_hz};
}

// ── Engine construction ───────────────────────────────────────────────────

struct Config {
    Voicing voicing = Voicing::ce2;
    JunoMode mode = JunoMode::mode_I;
    double depth = 1.0;
    double mix = 1.0;
    double width = 0.0;
    bool bbd = false;
};

/// Order matters: `set_voicing` adopts the voicing's shipped rate, so every
/// other setter runs after it.
void configure(Chorus& c, const Config& cfg) {
    c.set_voicing(cfg.voicing);
    c.set_juno_mode(cfg.mode);
    c.set_depth(cfg.depth);
    c.set_mix(cfg.mix);
    c.set_stereo_width(cfg.width);
    c.set_bbd_color(cfg.bbd);
    c.reset();
}

// ── Signal helpers ────────────────────────────────────────────────────────

/// Coherent DFT magnitude and phase at exactly `hz`, over a whole number of
/// periods. The only frequency-domain instrument in this file: the peak sample
/// of a rendered sine under-reads whenever no sample lands on the crest (six
/// samples per cycle at 8 kHz / 48 kHz, none of them on a crest — a −1.25 dB
/// error that looks exactly like a filter that is not flat).
std::complex<double> coherent_bin(const std::vector<double>& x, std::size_t begin,
                                  std::size_t count, double hz) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = 0; n < count; ++n) {
        const double theta = -2.0 * kPi * hz * static_cast<double>(n) / kSr;
        acc += x[begin + n] * std::complex<double>(std::cos(theta), std::sin(theta));
    }
    return acc * (2.0 / static_cast<double>(count));
}

std::vector<double> sine(std::size_t n, double hz, double amplitude) {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr);
    return out;
}

std::vector<double> seeded_noise(std::size_t n, double amplitude, std::uint32_t seed) {
    Xorshift32 rng{seed};
    std::vector<double> out(n);
    for (auto& v : out) v = amplitude * rng.next_bipolar<double>();
    return out;
}

struct Stereo {
    std::vector<double> left;
    std::vector<double> right;
};

Stereo render(const Config& cfg, const std::vector<double>& in_l, const std::vector<double>& in_r) {
    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    Stereo out{in_l, in_r};
    c.process(out.left.data(), out.right.data(), static_cast<int>(out.left.size()));
    return out;
}

/// Response of the WET path alone, extracted linearly. Every voicing's mix
/// matrix carries its own dry term, so `mix = 1` is matrix and `mix = 0` is the
/// bare input; for the mono-source voicings the matrix's dry term IS the input
/// when the input is mono, and the difference is the tap and nothing else.
std::vector<double> wet_only(Config cfg, const std::vector<double>& mono) {
    cfg.mix = 1.0;
    const auto wet = render(cfg, mono, mono);
    cfg.mix = 0.0;
    const auto dry = render(cfg, mono, mono);
    std::vector<double> out(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i) out[i] = wet.left[i] - dry.left[i];
    return out;
}

// ── Instrument 1: click-train delay tracking ──────────────────────────────

struct TrackedDelay {
    std::vector<long long> arrival_index;  ///< sample index the echo peaked at
    std::vector<double> delay_samples;
};

/// Locates each impulse's delayed copy in `channel` and returns the delay it
/// arrived with. The peak is refined by a parabolic fit over the three samples
/// around the largest one — the Lagrange kernel's crest sits between samples
/// whenever the fractional delay does.
TrackedDelay track_clicks(const std::vector<double>& channel, long long click_period,
                          double search_lo, double search_hi) {
    TrackedDelay out;
    const auto lo = static_cast<long long>(std::floor(search_lo));
    const auto hi = static_cast<long long>(std::ceil(search_hi));
    for (long long p = 0; p + hi + 2 < static_cast<long long>(channel.size()); p += click_period) {
        long long best = p + lo;
        double best_mag = -1.0;
        for (long long q = p + lo; q <= p + hi; ++q) {
            const double mag = std::abs(channel[static_cast<std::size_t>(q)]);
            if (mag > best_mag) {
                best_mag = mag;
                best = q;
            }
        }
        if (best <= 0 || best + 1 >= static_cast<long long>(channel.size())) continue;
        const double y0 = std::abs(channel[static_cast<std::size_t>(best - 1)]);
        const double y1 = std::abs(channel[static_cast<std::size_t>(best)]);
        const double y2 = std::abs(channel[static_cast<std::size_t>(best + 1)]);
        const double denom = y0 - 2.0 * y1 + y2;
        const double shift = std::abs(denom) > 1e-12 ? 0.5 * (y0 - y2) / denom : 0.0;
        out.arrival_index.push_back(best);
        out.delay_samples.push_back(static_cast<double>(best - p) + std::clamp(shift, -1.0, 1.0));
    }
    return out;
}

/// Two-parameter least squares of `y ≈ center + depth · m`, plus the RMS
/// residual. Fitting rather than min/max-picking is what makes a 20 Hz trace
/// able to resolve a ±2 % criterion on a triangle's corner.
struct Fit {
    double center;
    double depth;
    double residual_rms;
};

Fit fit_modulation(const std::vector<double>& y, const std::vector<double>& m) {
    const auto n = static_cast<double>(y.size());
    const double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / n;
    const double mean_m = std::accumulate(m.begin(), m.end(), 0.0) / n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        sxy += (m[i] - mean_m) * (y[i] - mean_y);
        sxx += (m[i] - mean_m) * (m[i] - mean_m);
    }
    const double depth = sxx > 0.0 ? sxy / sxx : 0.0;
    const double center = mean_y - depth * mean_m;
    double residual = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double e = y[i] - (center + depth * m[i]);
        residual += e * e;
    }
    return {center, depth, std::sqrt(residual / n)};
}

/// Collects `current_delay_ms(voice)` per sample over a silent render.
std::vector<double> delay_trace(const Config& cfg, int voice, std::size_t samples) {
    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    std::vector<double> trace(samples);
    double l = 0.0;
    double r = 0.0;
    for (std::size_t i = 0; i < samples; ++i) {
        l = 0.0;
        r = 0.0;
        c.process(&l, &r, 1);
        trace[i] = c.current_delay_ms(voice);
    }
    return trace;
}

std::string voicing_name(Voicing v) {
    switch (v) {
        case Voicing::ce2: return "ce2";
        case Voicing::juno_ensemble: return "juno_ensemble";
        case Voicing::dimension_d: return "dimension_d";
        case Voicing::tri_chorus: return "tri_chorus";
    }
    return "?";
}

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// 1. Delay-range accuracy — and the calibration of the accessor
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, documented rather than papered over: the criterion's second
// clause asks the Dimension D and TriChorus delays to "exactly match the
// shipped constants, since there is no external reference to fall short of".
// A MEASUREMENT cannot match anything exactly — the click tracker's own
// resolution is a fraction of a sample — so all four voicings are held to the
// same ±2 % band. Series law 6: acceptance criteria must be physically
// achievable.

TEST_CASE("chorus delay range matches the shipped calibration", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
        int voice;
        bool right_channel;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, 1, true},
        {Voicing::juno_ensemble, JunoMode::mode_II, 0, false},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II, 0, false},
        {Voicing::dimension_d, JunoMode::mode_I, 0, false},
        {Voicing::dimension_d, JunoMode::mode_I, 1, true},
        {Voicing::tri_chorus, JunoMode::mode_I, 0, false},
        {Voicing::tri_chorus, JunoMode::mode_I, 2, true},
    };

    constexpr long long kClickPeriod = 2400;  // 50 ms — the spec's recipe
    constexpr std::size_t kSamples = 20 * static_cast<std::size_t>(kSr);

    for (const auto& c : cases) {
        const Window w = shipped_window(c.voicing, c.mode);
        const double lo_ms = w.center_ms - w.depth_ms;
        const double hi_ms = w.center_ms + w.depth_ms;

        std::vector<double> in(kSamples, 0.0);
        for (std::size_t i = 0; i < kSamples; i += static_cast<std::size_t>(kClickPeriod))
            in[i] = 1.0;

        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        cfg.depth = 1.0;
        cfg.mix = 1.0;
        cfg.width = 0.0;
        const auto out = render(cfg, in, in);
        const auto& channel = c.right_channel ? out.right : out.left;

        // Search window: the shipped range plus a few samples of slack, never
        // reaching back to the dry impulse at lag 0.
        const double lo_samples = lo_ms * kSr * 0.001 - 8.0;
        const double hi_samples = hi_ms * kSr * 0.001 + 8.0;
        REQUIRE(lo_samples > 4.0);
        const auto tracked = track_clicks(channel, kClickPeriod, lo_samples, hi_samples);
        REQUIRE(tracked.delay_samples.size() > 100);

        std::vector<double> measured_ms;
        std::vector<double> reference_m;
        measured_ms.reserve(tracked.delay_samples.size());
        reference_m.reserve(tracked.delay_samples.size());
        for (std::size_t j = 0; j < tracked.delay_samples.size(); ++j) {
            measured_ms.push_back(tracked.delay_samples[j] * 1000.0 / kSr);
            // The echo carries the delay in force when it ARRIVED, so the
            // reference LFO is evaluated at the arrival index.
            reference_m.push_back(reference_modulation(c.voicing, c.mode, c.voice,
                                                       tracked.arrival_index[j] + 1, w.rate_hz));
        }

        const Fit fit = fit_modulation(measured_ms, reference_m);
        const double measured_lo = fit.center - std::abs(fit.depth);
        const double measured_hi = fit.center + std::abs(fit.depth);

        INFO(voicing_name(c.voicing) << " voice " << c.voice << ": fit centre " << fit.center
                                     << " ms (table " << w.center_ms << "), depth "
                                     << std::abs(fit.depth) << " ms (table " << w.depth_ms
                                     << "), residual " << fit.residual_rms << " ms");

        // A wrong SHAPE would still fit two parameters; the residual is what
        // makes this a test of the modulation and not just of its extremes.
        REQUIRE(fit.residual_rms < 0.02 * w.depth_ms + 0.01);
        REQUIRE_THAT(measured_lo, WithinRel(lo_ms, 0.02));
        REQUIRE_THAT(measured_hi, WithinRel(hi_ms, 0.02));

        // The accessor is now calibrated against the audio path, so tests 2–5
        // may rely on it.
        const auto trace = delay_trace(cfg, c.voice, 4096);
        for (std::size_t i = 0; i < trace.size(); ++i) {
            const double expected =
                w.center_ms + w.depth_ms * reference_modulation(c.voicing, c.mode, c.voice,
                                                                static_cast<long long>(i) + 1,
                                                                w.rate_hz);
            REQUIRE_THAT(trace[i], WithinAbs(expected, 1e-9));
        }
    }
}

TEST_CASE("chorus taps clear the interpolation guard band", "[signal][chorus][chorus-family]") {
    // The guard analysis in the header, asserted rather than asserted-in-prose.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        const Window w = shipped_window(voicing, mode);
        const double min_samples = (w.center_ms - w.depth_ms) * kSr * 0.001;
        INFO(voicing_name(voicing) << " minimum instantaneous delay " << min_samples
                                   << " samples against a " << Chorus::kGuardSamples
                                   << "-sample guard");
        REQUIRE(min_samples > static_cast<double>(Chorus::kGuardSamples));
        // With the BBD colour stage engaged the line keeps `kBbdColorGuardMs`
        // and the stage carries the rest, so the guard holds there too.
        REQUIRE(Chorus::kBbdColorGuardMs * kSr * 0.001 > static_cast<double>(Chorus::kGuardSamples));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 2. LFO rate accuracy
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, fixed with a documented reason: the recipe says "zero-crossing
// COUNT over 100 s ... within ±0.01 %". A count is an integer. The CE-2's
// 1.2 Hz gives 240 half-cycles in 100 s, so one count is 0.42 % — forty times
// coarser than the criterion, which no implementation could ever pass. What
// resolves ±0.01 % is crossing TIMING: the first and last upward crossings,
// each linearly interpolated to sub-sample precision, divided by the number of
// whole cycles between them. That instrument resolves ~2·10⁻⁷ here, and it
// still measures exactly what the criterion is about.

TEST_CASE("chorus LFO rate is exact through the tap", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kSamples = 100 * static_cast<std::size_t>(kSr);

    for (const auto& c : cases) {
        const Window w = shipped_window(c.voicing, c.mode);
        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        const auto trace = delay_trace(cfg, 0, kSamples);

        double first = -1.0;
        double last = -1.0;
        long long cycles = 0;
        for (std::size_t i = 1; i < trace.size(); ++i) {
            const double a = trace[i - 1] - w.center_ms;
            const double b = trace[i] - w.center_ms;
            if (!(a <= 0.0 && b > 0.0)) continue;  // upward crossing of centre
            const double frac = b != a ? -a / (b - a) : 0.0;
            const double t = static_cast<double>(i - 1) + frac;
            if (first < 0.0) {
                first = t;
            } else {
                last = t;
                ++cycles;
            }
        }
        REQUIRE(cycles > 10);
        const double measured_hz = static_cast<double>(cycles) * kSr / (last - first);
        INFO(voicing_name(c.voicing) << " mode " << static_cast<int>(c.mode) << ": measured "
                                     << measured_hz << " Hz against " << w.rate_hz << " Hz over "
                                     << cycles << " cycles");
        REQUIRE_THAT(measured_hz, WithinRel(w.rate_hz, 1e-4));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 3. Phase relationships — what actually distinguishes the voicings
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("chorus inverted-phase pairs are exact inversions", "[signal][chorus][chorus-family]") {
    // Juno and Dimension D: L and R modulators half a cycle apart. For an
    // odd-symmetric shape that is an exact inversion, so the two delay traces
    // sum to twice the centre at EVERY sample — a stronger statement than the
    // spec's "near-zero correlation at zero lag", and it also holds for the
    // Dimension D's trapezoid, whose clamp is odd and therefore inversion-safe.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        const Window w = shipped_window(voicing, mode);
        Config cfg;
        cfg.voicing = voicing;
        cfg.mode = mode;
        constexpr std::size_t kSamples = 300000;
        const auto left = delay_trace(cfg, 0, kSamples);
        const auto right = delay_trace(cfg, 1, kSamples);

        double sxy = 0.0;
        double sxx = 0.0;
        double syy = 0.0;
        for (std::size_t i = 0; i < kSamples; ++i) {
            REQUIRE_THAT(left[i] + right[i], WithinAbs(2.0 * w.center_ms, 1e-9));
            const double a = left[i] - w.center_ms;
            const double b = right[i] - w.center_ms;
            sxy += a * b;
            sxx += a * a;
            syy += b * b;
        }
        const double correlation = sxy / std::sqrt(sxx * syy);
        INFO(voicing_name(voicing) << " L/R zero-lag correlation " << correlation);
        REQUIRE(correlation < -0.9999);

        // ... and the spec's other half: peak correlation at half-period lag.
        const auto half_period = static_cast<std::size_t>(0.5 * kSr / w.rate_hz);
        double lagged = 0.0;
        double lag_xx = 0.0;
        double lag_yy = 0.0;
        for (std::size_t i = 0; i + half_period < kSamples; ++i) {
            const double a = left[i] - w.center_ms;
            const double b = right[i + half_period] - w.center_ms;
            lagged += a * b;
            lag_xx += a * a;
            lag_yy += b * b;
        }
        if (voicing == Voicing::juno_ensemble && mode == JunoMode::mode_I_plus_II) continue;
        REQUIRE(lagged / std::sqrt(lag_xx * lag_yy) > 0.999);
    }
}

TEST_CASE("chorus tri-voice modulators sit 120 degrees apart", "[signal][chorus][chorus-family]") {
    const Window w = shipped_window(Voicing::tri_chorus, JunoMode::mode_I);
    // A whole number of LFO periods, so the coherent bin sees no leakage from
    // the trace's large DC term (the centre delay).
    const auto period = static_cast<std::size_t>(std::llround(kSr / w.rate_hz));
    REQUIRE_THAT(static_cast<double>(period), WithinAbs(kSr / w.rate_hz, 1e-9));
    const std::size_t samples = period * 4;

    Config cfg;
    cfg.voicing = Voicing::tri_chorus;
    std::array<double, 3> phase_deg{};
    for (int k = 0; k < 3; ++k) {
        const auto trace = delay_trace(cfg, k, samples);
        const auto bin = coherent_bin(trace, 0, samples, w.rate_hz);
        phase_deg[static_cast<std::size_t>(k)] = std::arg(bin) * 180.0 / kPi;
    }

    auto wrapped = [](double deg) {
        while (deg > 180.0) deg -= 360.0;
        while (deg < -180.0) deg += 360.0;
        return deg;
    };
    const double d01 = wrapped(phase_deg[1] - phase_deg[0]);
    const double d12 = wrapped(phase_deg[2] - phase_deg[1]);
    const double d20 = wrapped(phase_deg[0] - phase_deg[2]);
    INFO("tri_chorus pairwise phase: 0->1 " << d01 << " deg, 1->2 " << d12 << " deg, 2->0 " << d20
                                            << " deg");
    REQUIRE_THAT(std::abs(d01), WithinAbs(120.0, 1.0));
    REQUIRE_THAT(std::abs(d12), WithinAbs(120.0, 1.0));
    REQUIRE_THAT(std::abs(d20), WithinAbs(120.0, 1.0));
    // Same sign for all three: the voices step around the circle in one
    // direction rather than two of them collapsing onto each other.
    REQUIRE(d01 * d12 > 0.0);
    REQUIRE(d12 * d20 > 0.0);
}

// ─────────────────────────────────────────────────────────────────────────
// 4. Juno I+II combination law
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, fixed with a documented reason: the criterion asks for 1000
// sample points across the 2.857 s beat period — one point every 2.857 ms —
// while the recipe's own click train resolves the delay trace at 20 Hz, one
// point every 50 ms. Fifty-seven points is all that instrument can deliver over
// a beat period, and raising the click rate to 350 Hz would space the clicks
// 2.86 ms apart, closer together than the 3.3–3.7 ms delay being measured, so
// each echo would land after the next click. The 1000-point check therefore
// runs on `current_delay_ms` (calibrated against the audio in test 1), and the
// click train's coarser trace cross-checks it there.

TEST_CASE("chorus juno I+II combines two triangles", "[signal][chorus][chorus-family]") {
    const auto spec = Chorus::juno_spec(JunoMode::mode_I_plus_II);
    const double beat_period_s = 1.0 / std::abs(spec.rate_b_hz - spec.rate_a_hz);
    INFO("beat period " << beat_period_s << " s");
    REQUIRE_THAT(beat_period_s, WithinRel(2.857142857, 1e-6));

    const auto samples = static_cast<std::size_t>(std::ceil(beat_period_s * kSr));
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_I_plus_II;
    const auto trace = delay_trace(cfg, 0, samples);

    double worst_relative = 0.0;
    double largest_combined = 0.0;
    for (int j = 0; j < 1000; ++j) {
        const auto i = static_cast<std::size_t>(
            static_cast<double>(j) * static_cast<double>(samples - 1) / 999.0);
        const double n = static_cast<double>(i) + 1.0;
        const double a = ref_triangle(n * spec.rate_a_hz / kSr);
        const double b = ref_triangle(n * spec.rate_b_hz / kSr);
        largest_combined = std::max(largest_combined, std::abs(0.5 * (a + b)));
        const double expected =
            spec.center_ms + spec.depth_ms * std::clamp(0.5 * (a + b), -1.0, 1.0);
        worst_relative = std::max(worst_relative, std::abs(trace[i] - expected) / expected);
    }
    INFO("worst relative error " << worst_relative << " over 1000 points");
    REQUIRE(worst_relative < 0.01);

    // The clamp in the combination law is belt-and-braces, not a shaper:
    // |0.5·(a + b)| ≤ 1 for any two bipolar triangles. Recorded so a future
    // change to the law that makes it bite is visible rather than silent.
    INFO("largest |0.5(tri_I + tri_II)| " << largest_combined);
    REQUIRE(largest_combined <= 1.0);

    // The measured window is the narrower published one, not mode I's.
    const auto lo = *std::min_element(trace.begin(), trace.end());
    const auto hi = *std::max_element(trace.begin(), trace.end());
    REQUIRE(lo >= spec.center_ms - spec.depth_ms - 1e-9);
    REQUIRE(hi <= spec.center_ms + spec.depth_ms + 1e-9);
}

TEST_CASE("chorus juno I+II has no third oscillator", "[signal][chorus][chorus-family]") {
    // §4.2's other normative claim: the ~9–10 Hz structure that short-window
    // analysis reports in the I+II position is the BEAT of the two component
    // rates, not an oscillator. A separately implemented 9.75 Hz LFO would put
    // real energy in a 9.75 Hz bin; a beat does not.
    //
    // (The obvious check — "the trace returns to its start after one beat
    // period" — is wrong and was removed after it failed: one beat period
    // returns the two triangles' relative PHASE, not their absolute phases.
    // 0.513 Hz advances 1.466 cycles over 2.857 s, so the combined value at the
    // end of a beat period has no reason to equal its value at the start.)
    const auto spec = Chorus::juno_spec(JunoMode::mode_I_plus_II);
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_I_plus_II;
    constexpr std::size_t kSamples = 60 * static_cast<std::size_t>(kSr);
    const auto trace = delay_trace(cfg, 0, kSamples);

    const double component_a = std::abs(coherent_bin(trace, 0, kSamples, spec.rate_a_hz));
    const double component_b = std::abs(coherent_bin(trace, 0, kSamples, spec.rate_b_hz));
    const double beat_report = std::abs(coherent_bin(trace, 0, kSamples, 9.75));
    const double reference = std::max(component_a, component_b);
    INFO("component bins " << component_a << " / " << component_b << " ms; 9.75 Hz bin "
                           << beat_report << " ms");
    REQUIRE(component_a > 0.1 * spec.depth_ms);
    REQUIRE(component_b > 0.1 * spec.depth_ms);
    REQUIRE(beat_report < 0.01 * reference);
}

// ─────────────────────────────────────────────────────────────────────────
// 5. Trapezoid dwell fraction
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("chorus dimension D dwells at the modulation extremes", "[signal][chorus][chorus-family]") {
    const Window w = shipped_window(Voicing::dimension_d, JunoMode::mode_I);
    const auto period = static_cast<std::size_t>(std::llround(kSr / w.rate_hz));
    REQUIRE_THAT(static_cast<double>(period), WithinAbs(kSr / w.rate_hz, 1e-9));

    Config cfg;
    cfg.voicing = Voicing::dimension_d;
    const auto trace = delay_trace(cfg, 0, period * 4);

    std::size_t at_rail = 0;
    for (const double d : trace) {
        const double m = (d - w.center_ms) / w.depth_ms;
        if (std::abs(m) >= 0.99) ++at_rail;
    }
    const double fraction = static_cast<double>(at_rail) / static_cast<double>(trace.size());
    const double expected = 1.0 - 1.0 / Chorus::kTrapK;
    INFO("dwell fraction " << fraction << " against 1 - 1/k = " << expected);
    // The 1 %-of-full-scale acceptance band admits a sliver either side of the
    // clamp (|tri| between 0.99/k and 1/k), worth 0.01/k = 0.56 points here —
    // inside the ±2-point tolerance, which is why the criterion is stated that
    // way rather than as an exact equality.
    REQUIRE_THAT(fraction, WithinAbs(expected, 0.02));
}

// ─────────────────────────────────────────────────────────────────────────
// 6. Worst-case gain
// ─────────────────────────────────────────────────────────────────────────
//
// Two spec defects here, both found by measurement and both recorded with
// numbers rather than smoothed over.
//
//   * §1.3's `gain ≤ (1 + N)` is **not an upper bound**. It treats a modulated
//     tap as unity gain, but the tap is a delay line read through the 4-point
//     Lagrange kernel, whose coefficients at a half-sample offset are
//     (−1/16, 9/16, 9/16, −1/16) — absolute sum 1.25. The CE-2 measured 2.078
//     against the spec's ceiling of 2 on a full-scale noise probe, which is how
//     this was found. The correct closed form replaces each tap's 1 with
//     `kTapL1`, and the first assertion below recomputes `kTapL1` from the
//     shipped kernel so the constant cannot drift away from the interpolator.
//   * The criterion also asks for "≥10 % headroom margin" against `(1 + N)`.
//     Unachievable, and for a reason worth stating: `(1 + N)` is *attained
//     exactly* by a DC input, because a delay line is transparent to DC and so
//     the dry term and every feedforward tap sit at +1 together. Zero headroom,
//     asserted below. Series law 6.
//   * §4.3's Dimension D bound of 1.41 + 1 + 1 ≈ 3.41 takes the cross-feed
//     high-pass as "≤ 1 (unity passband)". Passband gain is a steady-state
//     sinusoidal figure and not a peak bound; a first-order high-pass has L1
//     norm 2/(1 + tan(π f_c/f_s)) = 1.974 at the shipped 200 Hz and 48 kHz.

TEST_CASE("chorus tap gain matches the shipped interpolation kernel",
          "[signal][chorus][chorus-family]") {
    // `kTapL1` recomputed from `Interpolator::lagrange` itself, by probing it
    // with the four unit basis vectors — no algebra restated.
    double worst = 0.0;
    double worst_at = 0.0;
    for (int i = 0; i <= 20000; ++i) {
        const double frac = static_cast<double>(i) / 20000.0;
        double l1 = 0.0;
        for (int j = 0; j < 4; ++j) {
            double y[4] = {0.0, 0.0, 0.0, 0.0};
            y[j] = 1.0;
            l1 += std::abs(Interpolator::lagrange(frac, y[0], y[1], y[2], y[3]));
        }
        if (l1 > worst) {
            worst = l1;
            worst_at = frac;
        }
    }
    INFO("Lagrange kernel L1 norm peaks at " << worst << " at fractional offset " << worst_at);
    REQUIRE_THAT(worst, WithinRel(Chorus::kTapL1, 1e-9));
    REQUIRE_THAT(worst_at, WithinAbs(0.5, 1e-4));
}

TEST_CASE("chorus worst-case gain stays inside its closed-form bound",
          "[signal][chorus][chorus-family]") {
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kSamples = 96000;
    const std::size_t skip = static_cast<std::size_t>(Chorus::kMaxDelayMs * kSr * 0.001) + 64;

    for (const auto& [voicing, mode] : configs) {
        Chorus reference;
        reference.prepare(kSr);
        Config cfg;
        cfg.voicing = voicing;
        cfg.mode = mode;
        cfg.depth = 1.0;
        cfg.mix = 1.0;
        cfg.width = 1.0;
        configure(reference, cfg);
        const double bound = reference.worst_case_gain();
        // §1.3's (1 + N) counted PER CHANNEL, which is how the spec states it:
        // the Juno's N = 2 is one tap per channel, so its per-channel bound is
        // 2, not 3. The TriChorus's centre voice arrives at half weight.
        const double spec_bound = voicing == Voicing::dimension_d
                                      ? units::db_to_linear(3.0) + 2.0  // §4.3's 3.41
                                      : (voicing == Voicing::tri_chorus ? 2.5 : 2.0);

        // Three maximal-crest-factor probes, each reproducible from the shipped
        // constants rather than found by search: DC (every feedforward tap
        // constructive by construction, since a delay is transparent at DC),
        // Nyquist alternation, and full-scale seeded noise (which is what
        // exercises the interpolation kernel's sign pattern).
        auto peak_of = [&](const std::vector<double>& probe) {
            const auto out = render(cfg, probe, probe);
            double peak = 0.0;
            for (std::size_t i = skip; i < kSamples; ++i)
                peak = std::max({peak, std::abs(out.left[i]), std::abs(out.right[i])});
            return peak;
        };
        std::vector<double> alternating(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i) alternating[i] = (i % 2 == 0) ? 1.0 : -1.0;

        const double dc_peak = peak_of(std::vector<double>(kSamples, 1.0));
        const double worst = std::max(
            {dc_peak, peak_of(alternating), peak_of(seeded_noise(kSamples, 1.0, 0x5A17u))});

        INFO(voicing_name(voicing) << ": peak " << worst << " against the L1 bound " << bound
                                   << " (headroom " << (bound - worst) / bound * 100.0
                                   << " %); DC probe alone reaches " << dc_peak
                                   << " against the spec's " << spec_bound);
        REQUIRE(worst <= bound * (1.0 + 1e-9));

        // The spec's ceiling is attained exactly at DC, so the "≥10 % headroom"
        // clause is unachievable — recorded here as the measurement that shows
        // it, not argued in prose.
        if (voicing != Voicing::dimension_d) REQUIRE_THAT(dc_peak, WithinRel(spec_bound, 1e-9));
        REQUIRE(bound > spec_bound);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 7. Dimension D cross-mix
// ─────────────────────────────────────────────────────────────────────────

// Spec defect, and the reason this test measures the cross-mix TERM instead of
// the criterion's "mono sum" difference. As written the criterion says the mono
// sum changes by < 0.5 dB everywhere below `f_c` and by ≥ 1 dB everywhere above
// `2 f_c`. Both halves are false, and neither because the cross-mix misbehaves:
//
//   * Below `f_c` the specified one-pole high-pass still passes 70.7 % of the
//     cross-feed AT `f_c` itself, so a blanket "< f_c ⇒ inaudible" cannot hold
//     for the topology §4.3 specifies. Measured: −0.94 dB at 40 Hz, and +1.5 to
//     +1.8 dB at 80–120 Hz.
//   * Those positive numbers expose the other problem: the mono sum is a 6 ms
//     COMB (dry plus a 6 ms tap), so subtracting part of the wet term can raise
//     the sum as easily as lower it, and the criterion is really measuring the
//     comb. Above `2 f_c` the same comb gives only −0.67 dB at 400 Hz while
//     giving −5.5 dB at 1 kHz. Series law 6.
//
// What is comb-immune, and is what the criterion is actually about, is the
// difference signal `mono(width = 1) − mono(width = 0)`, which is exactly
// `−hpf(wet)`. Its normalised magnitude is asserted against the first-order
// high-pass law computed from the shipped corner. The criterion's own two
// clauses are then kept in the achievable form the measurement supports.

TEST_CASE("chorus dimension D cross-mix engages only above its corner",
          "[signal][chorus][chorus-family]") {
    // Depth is parked at 0 so both taps sit at a fixed delay: a modulated tap
    // spreads a probe tone's energy across sidebands, which would make a
    // single-bin magnitude a measurement of the modulation rather than of the
    // cross-mix.
    constexpr std::size_t kWindow = 65536;
    constexpr std::size_t kSettle = 32768;
    constexpr double kAmplitude = 0.5;
    const double corner = Chorus::kDimCornerHz;

    Config cfg;
    cfg.voicing = Voicing::dimension_d;
    cfg.depth = 0.0;
    cfg.mix = 1.0;

    // Bin-exact probe frequencies: `hz(p)` completes exactly p cycles in the
    // analysis window, so the coherent bin is leakage-free at any resolution.
    auto hz = [&](int p) { return kSr * static_cast<double>(p) / static_cast<double>(kWindow); };

    auto mono_bin = [&](double f, double width) {
        cfg.width = width;
        const auto in = sine(kSettle + kWindow, f, kAmplitude);
        const auto out = render(cfg, in, in);
        std::vector<double> mono(out.left.size());
        for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5 * (out.left[i] + out.right[i]);
        return coherent_bin(mono, kSettle, kWindow, f);
    };
    auto mono_delta_db = [&](double f) {
        return units::linear_to_db(std::abs(mono_bin(f, 1.0))) -
               units::linear_to_db(std::abs(mono_bin(f, 0.0)));
    };

    // The first-order high-pass the topology specifies, at the shipped corner,
    // in the prewarped form the TPT section actually realises.
    auto highpass_law = [&](double f) {
        const double w = std::tan(kPi * f / kSr);
        const double wc = std::tan(kPi * corner / kSr);
        return w / std::sqrt(w * w + wc * wc);
    };

    SECTION("the cross-feed follows the shipped first-order high-pass") {
        for (int p : {7, 14, 27, 55, 109, 164, 273, 546, 1366, 2731, 5461}) {
            const double f = hz(p);
            // width = 1 minus width = 0 removes the dry, the shelf and both
            // wet taps, leaving exactly the cross term: −hpf(wet), and `wet` is
            // the input delayed, so its magnitude is the input's.
            const double term = std::abs(mono_bin(f, 1.0) - mono_bin(f, 0.0)) / kAmplitude;
            INFO("probe " << f << " Hz: cross term " << term << " against the law "
                          << highpass_law(f));
            REQUIRE_THAT(term, WithinAbs(highpass_law(f), 0.02));
        }
    }

    SECTION("bass stays out of the cross-mix and the top end does not") {
        // The criterion's LF clause, restated at a frequency a one-pole can
        // deliver it at: a decade below the corner.
        const double deep = hz(static_cast<int>(std::llround(
            corner / 10.0 * static_cast<double>(kWindow) / kSr)));
        REQUIRE(deep < corner / 8.0);
        INFO("a decade below the corner (" << deep << " Hz): " << mono_delta_db(deep) << " dB");
        REQUIRE(std::abs(mono_delta_db(deep)) < 0.5);

        // The criterion's HF clause, at frequencies where the 6 ms comb is not
        // sitting on a null. 400 Hz is also above 2·f_c and yields only
        // −0.67 dB, which is why the clause cannot be stated for every such
        // frequency.
        for (int p : {1366, 2731, 5461}) {
            const double f = hz(p);
            REQUIRE(f > 2.0 * corner);
            INFO("probe " << f << " Hz: mono sum changes by " << mono_delta_db(f) << " dB");
            REQUIRE(std::abs(mono_delta_db(f)) >= 1.0);
        }
        INFO("recorded counterexample at 400 Hz (> 2 f_c): " << mono_delta_db(hz(546)) << " dB");
        REQUIRE(std::abs(mono_delta_db(hz(546))) < 1.0);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 8. BBD colour composition
// ─────────────────────────────────────────────────────────────────────────

// Spec defect, and the reason this test does not assert a −3 dB point against
// the bandwidth law. The criterion asks the wet path's −3 dB point to "match
// Prompt 2's bandwidth law ... ±15 % (same tolerance Prompt 2 itself uses for
// this law — inherited, not loosened)". That inheritance does not survive: the
// sibling suite (`test/test_character_delay.cpp`, "BBD bandwidth follows the
// clock rate") applies its ±15 % to `bbd_bandwidth_hz()`, the reported COEFFICIENT,
// not to a measured audio −3 dB point. `bandwidth_hz` names the corner of each
// of two cascaded 2-pole sections, and the composite path adds a linear
// interpolation stage and a clock-rate resampler on top of them, so its −3 dB
// point necessarily lands well below the nominal corner. Measured here: 6.39 kHz
// against a 10 kHz law — 0.64×, and no correct implementation of the cited
// topology can be closer. Series law 6.
//
// So the coefficient is asserted exactly (1e-9, tighter than the ±15 %), and
// the audio claim — "the wet path narrows to the composed bandwidth" — is
// asserted as three structural facts computed from that same coefficient:
// flat well inside it, far down at it, and monotone in between.

TEST_CASE("chorus BBD colour narrows the wet path to the composed bandwidth law",
          "[signal][chorus][chorus-family]") {
    constexpr std::size_t kWindow = 65536;
    constexpr std::size_t kSettle = 32768;

    Config cfg;
    cfg.voicing = Voicing::ce2;
    cfg.depth = 0.0;  // matched depth, and a static delay so the probe is clean
    cfg.mix = 1.0;

    Chorus probe;
    probe.prepare(kSr);
    Config with_bbd = cfg;
    with_bbd.bbd = true;
    configure(probe, with_bbd);
    const double bandwidth = probe.bbd_bandwidth_hz();
    const double stage_ms = probe.bbd_stage_delay_ms();

    // The law itself, recomputed from the composed module's own published
    // constants rather than read back from it.
    const double stages = chardelay::kBbdStages[2];  // kBbdColorCharacter = 1 → the 1024-stage knot
    const double expected_bandwidth =
        std::clamp(stages / (stage_ms * 0.001) / chardelay::kBbdBandwidthDivisor,
                   chardelay::kBbdBandwidthMinHz, chardelay::kBbdBandwidthMaxHz);
    INFO("BBD stage delay " << stage_ms << " ms, bandwidth law " << expected_bandwidth << " Hz");
    REQUIRE_THAT(bandwidth, WithinRel(expected_bandwidth, 1e-9));

    // Recorded, because it changes what §6 can promise: with the 1024-stage
    // knot the clock term is 62 kHz at the CE-2's sub-delay and 19 kHz even at
    // the longest chorus-scale delay in the family, so the law sits on its
    // 10 kHz ceiling everywhere here. The colour's bandwidth is CONSTANT across
    // the sweep — it does not "narrow with depth" at chorus delays.
    REQUIRE_THAT(bandwidth, WithinRel(chardelay::kBbdBandwidthMaxHz, 1e-9));
    REQUIRE(stages / (stage_ms * 0.001) / chardelay::kBbdBandwidthDivisor >
            chardelay::kBbdBandwidthMaxHz);

    auto hz = [&](int p) { return kSr * static_cast<double>(p) / static_cast<double>(kWindow); };
    auto wet_response_db = [&](bool bbd, double f) {
        Config c = cfg;
        c.bbd = bbd;
        const auto wet = wet_only(c, sine(kSettle + kWindow, f, 0.1));
        return units::linear_to_db(std::abs(coherent_bin(wet, kSettle, kWindow, f)));
    };

    // Without the colour the wet path is a pure integer-sample delay (12 ms is
    // 576 samples exactly at 48 kHz, so the Lagrange stencil is exact) — flat
    // to Nyquist, which is what makes the colour's narrowing attributable.
    const double flat_reference = wet_response_db(false, hz(683));  // ~500 Hz
    for (int p : {683, 5461, 10923, 21845}) {                       // 0.5, 4, 8, 16 kHz
        INFO("colour off at " << hz(p) << " Hz: " << wet_response_db(false, hz(p)) << " dB");
        REQUIRE_THAT(wet_response_db(false, hz(p)) - flat_reference, WithinAbs(0.0, 0.2));
    }

    const double plateau = wet_response_db(true, hz(683));
    auto relative = [&](double f) { return wet_response_db(true, f) - plateau; };

    // 1. Flat well inside the bandwidth. A fifth of the corner is where a
    //    4-pole cascade has spent 0.1 dB, so 0.5 dB is a real ceiling.
    const double inside = 0.2 * bandwidth;
    INFO("at 0.2x the bandwidth (" << inside << " Hz): " << relative(inside) << " dB");
    REQUIRE(relative(inside) > -0.5);

    // 2. Well down AT the bandwidth — the "narrowing" the toggle exists for.
    INFO("at the bandwidth (" << bandwidth << " Hz): " << relative(bandwidth) << " dB");
    REQUIRE(relative(bandwidth) < -10.0);

    // 3. Monotone through the transition, so the narrowing is a rolloff and not
    //    a resonance or an aliasing artefact.
    double previous = 1.0;
    for (int p : {683, 2731, 4096, 5461, 6827, 8192, 9557, 13653, 16384}) {
        const double db = relative(hz(p));
        INFO("colour on at " << hz(p) << " Hz: " << db << " dB");
        REQUIRE(db < previous + 0.05);
        previous = db;
    }

    // Recorded: the composite −3 dB point against the law, the number the
    // criterion asks to be within ±15 % and structurally cannot be.
    int lo_bin = 4096;   // 3 kHz
    int hi_bin = 16384;  // 12 kHz
    while (hi_bin - lo_bin > 4) {
        const int mid = (lo_bin + hi_bin) / 2;
        (relative(hz(mid)) > -3.0 ? lo_bin : hi_bin) = mid;
    }
    INFO("composite −3 dB near " << hz((lo_bin + hi_bin) / 2) << " Hz, i.e. "
                                 << hz((lo_bin + hi_bin) / 2) / bandwidth
                                 << "x the bandwidth law");
    REQUIRE(hz(lo_bin) < bandwidth);
}

// ─────────────────────────────────────────────────────────────────────────
// 9. Latency
// ─────────────────────────────────────────────────────────────────────────

// A note on what "zero latency" is allowed to mean, after the first draft of
// this test failed on the Dimension D. That draft asserted the output was
// silent between the impulse and the first wet arrival, which the Dimension D
// broke with 0.00481 at sample 1 — exactly `(A − 1) · h_lp[1]` of its dry
// low-shelf, reproduced by hand to six figures. A causal filter's TAIL is not
// lookahead, so that assertion was testing the shelf, not the latency. What
// zero latency actually claims is causality plus an unshifted wet arrival, and
// that is what is asserted below: nothing at all before the input (exact zero,
// not a threshold), and the wet peak inside the calibration window.

TEST_CASE("chorus reports zero latency and never reads ahead", "[signal][chorus][chorus-family]") {
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_II},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    constexpr std::size_t kImpulseAt = 4096;

    for (const auto& [voicing, mode] : configs) {
        // The colour-off wet arrival, measured first so the colour-on pass has
        // a reference to size "an early arrival would be this big" against.
        double clean_peak = 0.0;
        for (bool bbd : {false, true}) {
            const Window w = shipped_window(voicing, mode);
            Config cfg;
            cfg.voicing = voicing;
            cfg.mode = mode;
            cfg.mix = 1.0;
            cfg.width = 0.0;
            cfg.bbd = bbd;

            Chorus c;
            c.prepare(kSr);
            configure(c, cfg);
            REQUIRE(c.latency_samples() == 0);

            std::vector<double> in(12288, 0.0);
            in[kImpulseAt] = 1.0;
            auto left = in;
            auto right = in;
            c.process(left.data(), right.data(), static_cast<int>(left.size()));

            INFO(voicing_name(voicing) << " mode " << static_cast<int>(mode) << " bbd=" << bbd);
            // Causality, exactly: a module claiming zero latency may not put a
            // single non-zero sample ahead of its input.
            for (std::size_t i = 0; i < kImpulseAt; ++i) {
                REQUIRE(left[i] == 0.0);
                REQUIRE(right[i] == 0.0);
            }

            // ... and the wet arrival sits inside the calibration window, with
            // the interpolation stencil's footprint as the only slack.
            const auto lo = kImpulseAt +
                            static_cast<std::size_t>((w.center_ms - w.depth_ms) * kSr * 0.001) -
                            Chorus::kGuardSamples;
            const auto hi = kImpulseAt +
                            static_cast<std::size_t>((w.center_ms + w.depth_ms) * kSr * 0.001) +
                            Chorus::kGuardSamples;
            std::size_t peak_at = lo;
            double peak = 0.0;
            for (std::size_t i = kImpulseAt + 1; i < left.size(); ++i) {
                if (std::abs(left[i]) > peak) {
                    peak = std::abs(left[i]);
                    peak_at = i;
                }
            }
            // Everything between the input and the window's opening: a filter
            // tail belongs here (the Dimension D's dry shelf leaves 0.0048 at
            // the very next sample), an early wet arrival would not.
            double before_window = 0.0;
            for (std::size_t i = kImpulseAt + 1; i < lo; ++i)
                before_window = std::max(before_window, std::abs(left[i]));

            if (!bbd) clean_peak = peak;
            INFO("wet peak " << peak << " at sample " << peak_at - kImpulseAt << ", window ["
                             << lo - kImpulseAt << ", " << hi - kImpulseAt
                             << "], largest sample before the window " << before_window
                             << ", colour-off arrival " << clean_peak);

            // An early wet arrival would be the same tap at the wrong time, so
            // it would be of the same order as the real one. Two percent of the
            // colour-off arrival is comfortably below that and comfortably
            // above what a causal filter tail leaves here — the Dimension D's
            // dry low-shelf puts 0.00481 at the very next sample, which is
            // (A − 1)·h_lp[1] reproduced to six figures by hand.
            REQUIRE(clean_peak > 0.1);
            REQUIRE(before_window < 0.02 * clean_peak);

            // Where the arrival LANDS is asserted only with the colour off.
            // The compander's expander multiplies by an envelope that is still
            // climbing when the arrival gets there, so the peak of an impulse
            // response through it sits later than the arrival — measured at
            // 8.1 ms for the Juno's 3.5 ms tap — and the compressor's
            // divide-by-the-floor scales the whole thing to ~0.004. Both are
            // amplitude-domain, and a late peak cannot indicate negative
            // latency, which is the only thing this test exists to exclude.
            // The colour's effect on the actual delay is measured properly by
            // the group-delay case below.
            if (!bbd) {
                REQUIRE(peak_at >= lo);
                REQUIRE(peak_at <= hi);
            }
        }
    }
}

TEST_CASE("chorus BBD colour does not move the tap it colours",
          "[signal][chorus][chorus-family]") {
    // The load-bearing claim of the §6 substitution: the colour stage is a
    // clocked delay, so it is given a fixed sub-delay and the Lagrange line
    // carries the remainder, leaving the TOTAL on the calibration table. That
    // arithmetic is asserted here rather than assumed — a sign error in the
    // split would move every voicing's delay and nothing else in the suite
    // would notice, because every other test runs with the colour off.
    //
    // Noise at a working level, not an impulse: a compander's response to an
    // impulse says more about the compander than about the delay.
    for (auto voicing : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                         Voicing::tri_chorus}) {
        Config cfg;
        cfg.voicing = voicing;
        cfg.depth = 0.0;  // static delay, so a single lag is well defined
        cfg.mix = 1.0;
        cfg.width = 0.0;
        const auto source = seeded_noise(131072, 0.3, 0x4B1Du);

        Config off = cfg;
        Config on = cfg;
        on.bbd = true;
        const auto wet_off = wet_only(off, source);
        const auto wet_on = wet_only(on, source);

        constexpr std::size_t kBegin = 65536;
        constexpr std::size_t kCount = 32768;
        constexpr int kSearch = 48;
        int best_lag = 0;
        double best = -1.0;
        for (int lag = -kSearch; lag <= kSearch; ++lag) {
            double acc = 0.0;
            for (std::size_t i = 0; i < kCount; ++i)
                acc += wet_off[kBegin + i] * wet_on[static_cast<std::size_t>(
                                                 static_cast<long long>(kBegin + i) + lag)];
            if (std::abs(acc) > best) {
                best = std::abs(acc);
                best_lag = lag;
            }
        }
        INFO(voicing_name(voicing) << ": colour adds " << best_lag << " samples of group delay");
        // The colour stage adds a few samples of fixed group delay — its
        // band-limiting pair (a 2-pole Butterworth at 10 kHz is √2/(2π·10 kHz)
        // = 22.5 µs ≈ 1.1 samples at 48 kHz) plus the clocked core's write-side
        // ramp and read-position alignment. Measured: 3 samples, identically
        // for all four voicings. The tolerance bounds that, and nothing wider:
        // the sub-delays being split off here run from 56 to 528 samples, so a
        // sign or scale error in the split would land far outside the ±48
        // sample search window rather than inside this bound.
        REQUIRE(std::abs(best_lag) <= 4);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 10. Determinism
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("chorus renders are bit-identical after reset", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
        bool bbd;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, false},
        {Voicing::juno_ensemble, JunoMode::mode_II, false},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II, false},
        {Voicing::dimension_d, JunoMode::mode_I, false},
        {Voicing::tri_chorus, JunoMode::mode_I, false},
        {Voicing::ce2, JunoMode::mode_I, true},
        {Voicing::tri_chorus, JunoMode::mode_I, true},
    };
    const auto source = seeded_noise(10 * static_cast<std::size_t>(kSr), 0.5, 0x51F0u);

    for (const auto& c : cases) {
        Chorus engine;
        engine.prepare(kSr);
        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        cfg.width = 1.0;
        cfg.bbd = c.bbd;
        configure(engine, cfg);

        auto run = [&] {
            engine.reset();
            std::vector<double> left = source;
            std::vector<double> right = source;
            engine.process(left.data(), right.data(), static_cast<int>(left.size()));
            return std::pair{left, right};
        };
        const auto first = run();
        const auto second = run();
        INFO(voicing_name(c.voicing) << " bbd=" << c.bbd);
        REQUIRE(first.first == second.first);
        REQUIRE(first.second == second.second);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// 11. RT allocation
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("chorus process and reset allocate nothing", "[signal][chorus][chorus-family]") {
    // The probe's silence only means something if the probe can speak, so every
    // engine below is first run through a KNOWN-allocating call — `prepare`,
    // which sizes the delay lines and is the one function in the class allowed
    // to allocate. That doubles as the positive half of the RT contract.
    //
    // A synthetic control (a local `std::vector` inside a probe scope) does NOT
    // work here and was tried first: at -O3 clang stack-promotes it under the
    // C++14 allocation-elision rule, and the probe correctly reports zero for an
    // allocation that no longer happens. Anchoring the data pointer in a
    // `volatile` did not stop it either. Using the real call avoids the whole
    // question.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        for (bool bbd : {false, true}) {
            auto engine = std::make_unique<Chorus>();
            {
                pulp::test::RtAllocationProbe control;
                engine->prepare(kSr);
                INFO(voicing_name(voicing) << " bbd=" << bbd << ": prepare allocated "
                                           << control.allocated_bytes() << " bytes in "
                                           << control.allocation_count() << " calls");
                REQUIRE(control.allocation_count() > 0);
            }
            Config cfg;
            cfg.voicing = voicing;
            cfg.mode = mode;
            cfg.bbd = bbd;
            configure(*engine, cfg);

            std::vector<double> left(512, 0.25);
            std::vector<double> right(512, -0.25);
            engine->process(left.data(), right.data(), 512);  // warm any lazy state

            require_allocates_no_memory([&] {
                engine->process(left.data(), right.data(), 512);
                engine->reset();
                engine->set_rate_hz(2.0);
                engine->set_depth(0.75);
                engine->set_mix(0.4);
                engine->set_stereo_width(0.6);
                engine->set_voicing(voicing);
                engine->set_juno_mode(mode);
                engine->set_bbd_color(bbd);
                engine->process(left.data(), right.data(), 512);
            });
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Contract checks the acceptance list implies but does not enumerate
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("chorus juno ignores the rate parameter", "[signal][chorus][chorus-family]") {
    // Closed decision, §4.2: the Juno's fixed per-mode rates ARE the Juno
    // sound, so the shared rate control is deliberately inert there. A test
    // that only checked "rate_hz() changed" would pass for a voicing that
    // silently honoured it, so this measures the delay trace.
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_II;
    const auto before = delay_trace(cfg, 0, 4096);

    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    c.set_rate_hz(7.5);
    REQUIRE_THAT(c.rate_hz(), WithinRel(7.5, 1e-12));
    std::vector<double> after(4096);
    for (std::size_t i = 0; i < after.size(); ++i) {
        double l = 0.0;
        double r = 0.0;
        c.process(&l, &r, 1);
        after[i] = c.current_delay_ms(0);
    }
    REQUIRE(before == after);

    // ... while every other voicing does honour it.
    Config ce2 = cfg;
    ce2.voicing = Voicing::ce2;
    const auto default_trace = delay_trace(ce2, 0, 4096);
    Chorus fast;
    fast.prepare(kSr);
    configure(fast, ce2);
    fast.set_rate_hz(5.0);
    double l = 0.0;
    double r = 0.0;
    for (int i = 0; i < 4096; ++i) fast.process(&l, &r, 1);
    REQUIRE(fast.current_delay_ms(0) != default_trace.back());
}

TEST_CASE("chorus mix crossfades bypass against the whole circuit",
          "[signal][chorus][chorus-family]") {
    // §5: the blend is applied AFTER the voicing matrix, and every matrix
    // carries its own dry term, so mix = 0 must be an exact bypass.
    const auto source = seeded_noise(4096, 0.5, 0x0C0Fu);
    for (auto voicing : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                         Voicing::tri_chorus}) {
        Config cfg;
        cfg.voicing = voicing;
        cfg.mix = 0.0;
        cfg.width = 1.0;
        const auto out = render(cfg, source, source);
        INFO(voicing_name(voicing));
        REQUIRE(out.left == source);
        REQUIRE(out.right == source);
    }
}

TEST_CASE("chorus ce2 is mono on both outputs", "[signal][chorus][chorus-family]") {
    // §4.1: the real pedal is mono in and out on both jacks.
    const auto left_in = seeded_noise(4096, 0.5, 0x11u);
    const auto right_in = seeded_noise(4096, 0.5, 0x22u);
    Config cfg;
    cfg.voicing = Voicing::ce2;
    cfg.mix = 1.0;
    const auto out = render(cfg, left_in, right_in);
    REQUIRE(out.left == out.right);
}

TEST_CASE("chorus float and double instantiations agree", "[signal][chorus][chorus-family]") {
    // The read position and phase accumulators are `double` in both
    // instantiations by construction; only the delay-line storage narrows. This
    // pins that the narrowing costs precision and nothing else.
    const auto source = seeded_noise(8192, 0.5, 0x2B1Du);
    ChorusEnsembleT<float> narrow;
    narrow.prepare(kSr);
    narrow.set_voicing(ChorusEnsembleT<float>::Voicing::tri_chorus);
    narrow.set_depth(1.0f);
    narrow.set_mix(1.0f);
    narrow.set_stereo_width(1.0f);
    narrow.reset();

    std::vector<float> fl(source.size());
    std::vector<float> fr(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        fl[i] = static_cast<float>(source[i]);
        fr[i] = fl[i];
    }
    narrow.process(fl.data(), fr.data(), static_cast<int>(fl.size()));

    Config cfg;
    cfg.voicing = Voicing::tri_chorus;
    cfg.mix = 1.0;
    cfg.width = 1.0;
    const auto wide = render(cfg, source, source);

    double worst = 0.0;
    for (std::size_t i = 0; i < source.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(fl[i]) - wide.left[i]));
    INFO("largest float/double divergence " << worst);
    REQUIRE(worst < 1e-4);
}
