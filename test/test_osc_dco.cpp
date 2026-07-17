// OSC-DCO: the divider-clocked timing front-end over the shared bandlimited
// shaper. A DCO's defining imperfection is pitch QUANTIZATION (larger at high
// notes, exact at low ones), not drift — its clock is crystal-stable. This suite
// proves that imperfection twice: on the derived integer divider N and the
// rational period (the equality-test domain that survives cross-platform), AND on
// the rendered pitch measured by the shipped sub-cent pitch tracker. The
// fractional-N scheme's accuracy-vs-jitter trade, the shared-path alias rejection,
// the absence of drift, and determinism are each gated with a number and, where a
// floor is involved, a negative control.
//
// ── Measurement discipline (same as the VA / VCO suites) ──────────────────────
//
// Alias numbers come from `measure_aliasing` — band-qualified to 20 kHz with a
// floor PROVEN by a negative control (a bandlimited saw with zero alias content
// must read collapsed; an injected alias of known level must be recovered) rather
// than read off the analyzer's own white-residual bound. Pitch numbers come from
// the shipped `estimate_pitch` / `track_pitch`, never a hand-rolled estimator.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/analysis/pitch_track.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/dco.hpp>
#include <pulp/signal/osc/va.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::osc::DcoDivider;
using pulp::signal::osc::DcoOscillator;
using pulp::signal::osc::DcoProfile;
using pulp::signal::osc::VaOscillator;
using pulp::signal::osc::VaShape;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::cents_between;
using pulp::test::audio::estimate_pitch;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;
using pulp::test::audio::PitchEstimate;
using pulp::test::audio::PitchOptions;
using pulp::test::audio::PitchTrackOptions;
using pulp::test::audio::track_pitch;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kRenderLength = 8192;
constexpr double kBandHz = 20000.0;

// An illustrative clock chosen so the quantization is clearly measurable across
// the range (the real per-profile value is data). At 1 MHz the worst-case
// error spans ~0.05 cents at A1 to several cents at the top octave — an octave-
// doubling the sub-cent pitch tracker resolves plainly.
constexpr double kClockHz = 1'000'000.0;
// A faster clock for the alias / shape work, where a musical-range fundamental is
// what matters and a fine pitch grid keeps the saw's harmonics clean.
constexpr double kFastClockHz = 8'000'000.0;

DcoProfile integer_profile(double clock = kClockHz) {
    DcoProfile p;
    p.master_clock_hz = clock;
    p.divider_scheme = DcoDivider::integer_n;
    return p;
}

DcoProfile fractional_profile(int bits = 24, double clock = kClockHz) {
    DcoProfile p;
    p.master_clock_hz = clock;
    p.divider_scheme = DcoDivider::fractional_n;
    p.accumulator_bits = bits;
    return p;
}

// A note whose ideal divider N* = f_clk / f_note sits near K + 0.5, i.e. the
// worst case of the rounding — so the realised pitch error is close to the bound
// rather than an incidental near-integer that happens to land dead on. K small is
// a high note (few clocks per cycle), K large a low one.
double worst_case_note(long k, double clock = kClockHz) {
    return clock / (static_cast<double>(k) + 0.5);
}

std::vector<double> render_dco(const DcoProfile& profile, VaShape shape, double note,
                               int length = kRenderLength, double start_phase = 0.0) {
    DcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_profile(profile);
    osc.set_shape(shape);
    osc.set_note_hz(note);
    osc.reset(start_phase);
    std::vector<double> out(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = osc.next();
    return out;
}

std::vector<float> render_dco_f(const DcoProfile& profile, VaShape shape, double note,
                                int length, double start_phase = 0.0) {
    DcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_profile(profile);
    osc.set_shape(shape);
    osc.set_note_hz(note);
    osc.reset(start_phase);
    std::vector<float> out(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = static_cast<float>(osc.next());
    return out;
}

// A trivial (uncorrected) saw at f0 — the alias baseline the shared correction
// must improve on.
std::vector<double> render_trivial_saw(double f0) {
    std::vector<double> out(static_cast<std::size_t>(kRenderLength));
    const double increment = f0 / kSampleRate;
    double p = 0.0;
    for (int i = 0; i < kRenderLength; ++i) {
        out[static_cast<std::size_t>(i)] = 2.0 * p - 1.0;
        p += increment;
        p -= std::floor(p);
    }
    return out;
}

// An exactly bandlimited saw carrying zero alias energy by construction — the
// negative control the alias floor is proven with; optionally injects a known
// tone on a fold site.
std::vector<double> bandlimited_saw(double f0, double alias_hz = 0.0, double alias_amplitude = 0.0) {
    const int max_h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0));
    std::vector<double> out(static_cast<std::size_t>(kRenderLength), 0.0);
    for (int i = 0; i < kRenderLength; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double v = 0.0;
        for (int h = 1; h <= max_h; ++h) {
            const double sign = (h % 2 == 1) ? 1.0 : -1.0;
            v += sign * std::sin(2.0 * std::numbers::pi * h * f0 * t + 0.3) / h;
        }
        v *= 2.0 / std::numbers::pi;
        if (alias_amplitude > 0.0)
            v += alias_amplitude * std::sin(2.0 * std::numbers::pi * alias_hz * t + 0.9);
        out[static_cast<std::size_t>(i)] = v;
    }
    return out;
}

constexpr double kBandlimitedFundamental = 2.0 / std::numbers::pi;

pulp::audio::Buffer<float> to_buffer(const std::vector<double>& signal) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int i = 0; i < static_cast<int>(signal.size()); ++i)
        buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);
    return buffer;
}

int harmonics_for(double f0) { return static_cast<int>(std::ceil(3.0 * kSampleRate / f0)); }

AliasReport analyze(const std::vector<double>& signal, double f0) {
    auto buffer = to_buffer(signal);
    AliasOptions options;
    options.num_harmonics = harmonics_for(f0);
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1; h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

double peak(const std::vector<double>& v) {
    double m = 0.0;
    for (const double x : v) m = std::fmax(m, std::fabs(x));
    return m;
}

// Recover the per-cycle reset intervals, in samples, from a rendered saw by
// locating each divider reset. The saw ramps −1 → +1 (crossing zero UPWARD once,
// mid-cycle) then wraps +1 → −1 at the reset — the only DOWNWARD zero crossing in
// the cycle. Detecting that crossing is robust to the BLEP edge's exact slope /
// amplitude (unlike a raw sample-diff threshold). A refractory gap shorter than a
// cycle rejects any BLEP ringing near the edge. The first and last (partial)
// cycles fall out of the interval list. Used to prove the reset SCHEDULE actually
// reaches the audio, not just the integer-domain `reset_intervals` view.
std::vector<int> reset_intervals_from_audio(const std::vector<double>& y,
                                            int refractory = 32) {
    std::vector<int> resets;
    int last = -1 << 20;
    for (std::size_t i = 1; i < y.size(); ++i) {
        const bool downward = y[i - 1] > 0.0 && y[i] <= 0.0;
        if (downward && static_cast<int>(i) - last > refractory) {
            resets.push_back(static_cast<int>(i));
            last = static_cast<int>(i);
        }
    }
    std::vector<int> intervals;
    for (std::size_t j = 1; j < resets.size(); ++j)
        intervals.push_back(resets[j] - resets[j - 1]);
    return intervals;
}

// The realised pitch a long held sine settles to, in Hz, via the shipped
// estimator (sub-cent on a clean tone).
double measured_pitch_hz(const DcoProfile& profile, double note, int length) {
    const auto sig = render_dco_f(profile, VaShape::sine, note, length);
    PitchOptions options;
    const PitchEstimate est = estimate_pitch(std::span<const float>(sig), kSampleRate, options);
    REQUIRE(est.voiced);
    return est.hz;
}

} // namespace

// ── The DCO shapes nothing: it composes the shared bandlimited core ───────────

TEST_CASE("an integer-N DCO is the shared core driven at f_clk/N", "[signal][osc][dco]") {
    // Integer-N is the free-running path: the increment is (f_clk/N)/f_s and the
    // natural wrap IS the reset. So the DCO's output must be bit-for-bit a
    // VaOscillator advanced at exactly that increment — proof that OSC-DCO adds
    // the divider timing and NOTHING to the waveform (it re-implements no shaping).
    const DcoProfile profile = integer_profile();
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square, VaShape::triangle}) {
        for (const long k : {73L, 227L, 941L, 3187L}) {
            const double note = kClockHz / (static_cast<double>(k) + 0.37);

            DcoOscillator dco;
            dco.prepare(kSampleRate);
            dco.set_profile(profile);
            dco.set_shape(shape);
            dco.set_note_hz(note);
            dco.reset(0.19);
            const double realized = dco.realized_hz();
            REQUIRE(realized > 0.0);

            VaOscillator core;
            core.set_shape(shape);
            core.reset(0.19);
            const double increment = realized / kSampleRate;

            int differing = 0;
            for (int i = 0; i < kRenderLength; ++i)
                if (dco.next() != core.next(increment)) ++differing;
            INFO("shape " << static_cast<int>(shape) << " k=" << k << ": " << differing
                          << " samples differ from the shared core");
            CHECK(differing == 0);
        }
    }
}

// ── Pitch quantization: the defining imperfection ─────────────────────────────

TEST_CASE("integer-N pitch is quantized to f_clk/N, on the integer/rational domain",
          "[signal][osc][dco][quantization]") {
    // The exact equality-test domain: the divider N is round(f_clk/f_note_eff) and
    // the realised frequency is EXACTLY the rational f_clk/N. Asserted on the
    // integers, which are bit-identical across platforms, not on float audio.
    const DcoProfile profile = integer_profile();
    for (const long k : {80L, 400L, 2000L, 10000L}) {
        const double note = worst_case_note(k);
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(note);

        const long long n = osc.divider_n();
        REQUIRE(n == static_cast<long long>(std::llround(kClockHz / osc.effective_note_hz())));
        REQUIRE(n >= 1);
        // The realised frequency is the rational f_clk/N to the bit.
        REQUIRE(osc.realized_hz() == kClockHz / static_cast<double>(n));
        // And the detune is that rational's cents distance from the note.
        const double bound = osc.quantization_bound_cents();
        INFO("k=" << k << " note " << note << " Hz: N=" << n << ", detune "
                  << osc.detune_cents() << " cents, bound " << bound);
        CHECK(std::fabs(osc.detune_cents()) <= bound + 1e-9);
        // The worst-case construction lands the error near the bound, not on an
        // incidental near-integer divider.
        CHECK(std::fabs(osc.detune_cents()) > 0.35 * bound);
    }
}

TEST_CASE("the quantization bound holds on the round-DOWN side too, where the "
          "linear form was violated",
          "[signal][osc][dco][quantization]") {
    // The half-LSB rounding error is ASYMMETRIC in cents. `worst_case_note`
    // (clock/(k+0.5)) always rounds the divider UP — realized pitch a hair LOW —
    // the favourable side, where the old linear bound 865.617·f_note/f_clk held.
    // A note whose ideal divider rounds DOWN (realized pitch HIGHER) detunes MORE
    // for the same half LSB, because log2(x/(x−½)) > log2((x+½)/x). The old linear
    // bound understated exactly this side; a note that rounds down slipped past it.
    // The exact bound must cover it, and we prove the old linear form did not.
    //
    // High notes (small N* = few clocks per cycle) are used because that is where
    // the ½-LSB error is a large fraction of the divider and the second-order gap
    // between the exact log bound and its linear tangent is resolvable — the exact
    // regime the finding is about (a DCO's tuning is imperfect up top, exact at the
    // bottom). At a low note N* is huge and the gap is sub-micro-cent, below double
    // precision, so a per-note violation is not observable there (nor audible).
    const DcoProfile profile = integer_profile();
    for (const long k : {8L, 16L, 24L, 32L, 48L}) {
        // Ideal divider N* = k + 0.4999 → rounds DOWN to k (δ ≈ −0.5, essentially
        // the worst case), realized f_clk/k above the note. Just inside the tie
        // (which rounds away, to k+1) so the rounding is unambiguous.
        const double note = kClockHz / (static_cast<double>(k) + 0.4999);
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(note);

        REQUIRE(osc.divider_n() == k);          // rounded down, as constructed.
        REQUIRE(osc.detune_cents() > 0.0);       // realized pitch is HIGHER.

        const double detune = std::fabs(osc.detune_cents());
        const double exact_bound = osc.quantization_bound_cents();
        const double linear_bound =
            865.617 * osc.effective_note_hz() / kClockHz; // the old, false bound.
        INFO("k=" << k << " note " << note << " Hz: N=" << osc.divider_n()
                  << ", detune " << detune << " cents, exact bound " << exact_bound
                  << ", old linear bound " << linear_bound);

        // The exact bound is a true upper bound on this side.
        CHECK(detune <= exact_bound + 1e-12);
        // The exact bound is strictly larger than the old linear form everywhere...
        CHECK(exact_bound > linear_bound);
        // ...and here the ACTUAL round-down error exceeds the old bound — a note
        // the old "upper bound" failed to bound, which is the bug the fix closes.
        CHECK(detune > linear_bound);
    }
}

TEST_CASE("the quantization error is larger at high notes than low ones",
          "[signal][osc][dco][quantization]") {
    // |e|_max ≈ 1200·log2(N*/(N*−½)) with N* = f_clk/f_note — it roughly DOUBLES
    // per octave up (the ½-LSB envelope scales with the note). A model that plays
    // every note on-pitch has idealized the DCO away. Both the bound and the
    // realised worst-case error scale with the note, so the top octave is where a
    // DCO's tuning is audibly imperfect and the bottom octaves are effectively
    // exact.
    const DcoProfile profile = integer_profile();

    auto detune_at = [&](long k) {
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(worst_case_note(k));
        return std::fabs(osc.detune_cents());
    };
    auto bound_at = [&](long k) {
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(worst_case_note(k));
        return osc.quantization_bound_cents();
    };

    // A high note (k=64, ~15.6 kHz) against a low one (k=8192, ~122 Hz): seven
    // octaves apart, so the error should differ by ~2^7 = 128x.
    const double hi = detune_at(64);
    const double lo = detune_at(8192);
    INFO("worst-case detune: high note " << hi << " cents, low note " << lo
                                         << " cents (ratio " << hi / lo << ")");
    CHECK(hi > 40.0 * lo);           // dramatically larger up top.
    CHECK(bound_at(64) > 40.0 * bound_at(8192));

    // The bound is monotone in the note across the range.
    double prev = 0.0;
    for (const long k : {8192L, 2048L, 512L, 128L, 64L}) { // rising pitch.
        const double b = bound_at(k);
        CHECK(b > prev);
        prev = b;
    }
}

TEST_CASE("the rendered pitch carries the quantization the analyzer can see",
          "[signal][osc][dco][quantization]") {
    // The quantization is not just an arithmetic claim: the shipped sub-cent pitch
    // estimator, run on the rendered sine, recovers the QUANTIZED pitch f_clk/N —
    // detuned from the commanded note by exactly the derived amount, and larger at
    // the high note. A faithful DCO renders the note off-pitch on purpose.
    const DcoProfile profile = integer_profile();
    constexpr int kLen = static_cast<int>(0.75 * kSampleRate);

    for (const long k : {96L, 6000L}) { // one high, one low.
        const double note = worst_case_note(k);
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(note);
        const double realized = osc.realized_hz();
        const double predicted = osc.detune_cents();

        const double measured = measured_pitch_hz(profile, note, kLen);
        const double measured_cents = cents_between(measured, osc.effective_note_hz());
        INFO("k=" << k << ": realised " << realized << " Hz, measured " << measured
                  << " Hz; detune predicted " << predicted << " cents, measured "
                  << measured_cents << " cents");
        // The tracker sees f_clk/N, not the commanded note.
        CHECK_THAT(measured, WithinRel(realized, 5e-5));
        // And the measured detune matches the derived one to well under a cent.
        CHECK_THAT(measured_cents, WithinAbs(predicted, 0.2));
    }
}

// ── Integer-N is perfectly periodic (no jitter) ───────────────────────────────

TEST_CASE("integer-N resets on a constant interval with no jitter",
          "[signal][osc][dco][integer]") {
    // The reset interval is exactly N master clocks — perfectly periodic in
    // continuous time. On the integer domain every reported interval is N; in the
    // audio the pitch is dead stationary (the opposite of the fractional jitter
    // below, and of a VCO's drift).
    const DcoProfile profile = integer_profile();
    const double note = worst_case_note(300);
    DcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_profile(profile);
    osc.set_note_hz(note);

    std::array<long long, 64> intervals{};
    const int got = osc.reset_intervals(std::span<long long>(intervals));
    REQUIRE(got == static_cast<int>(intervals.size()));
    const long long n = osc.divider_n();
    for (const long long m : intervals) CHECK(m == n);
}

// ── Fractional-N: accurate average, bounded ±1-clock period jitter ────────────

TEST_CASE("fractional-N cuts the average detuning but injects bounded jitter",
          "[signal][osc][dco][fractional]") {
    // Fractional-N places the AVERAGE pitch arbitrarily close to the note (the
    // frequency step is a constant f_clk/2^B, so its residual error shrinks with B
    // and is largest at LOW notes — the opposite note-dependence of integer-N),
    // bought with a deterministic period jitter: the reset interval alternates
    // floor / ceil master clocks, a ±1-clock deviation, and no more.
    const long k = 96; // a high note where integer-N is appreciably off.
    const double note = worst_case_note(k);

    DcoOscillator integer;
    integer.prepare(kSampleRate);
    integer.set_profile(integer_profile());
    integer.set_note_hz(note);

    DcoOscillator fractional;
    fractional.prepare(kSampleRate);
    fractional.set_profile(fractional_profile(24));
    fractional.set_note_hz(note);

    INFO("note " << note << " Hz: integer detune " << integer.detune_cents()
                 << " cents, fractional detune " << fractional.detune_cents() << " cents");

    SECTION("the average detuning is far smaller than integer-N") {
        CHECK(std::fabs(fractional.detune_cents()) < 0.1 * std::fabs(integer.detune_cents()));
        // Bounded by the scheme's own exact quantization bound (not a hand-rolled
        // linearization that the asymmetric error can slip past).
        CHECK(std::fabs(fractional.detune_cents()) <=
              fractional.quantization_bound_cents() + 1e-12);
    }

    SECTION("the reset interval alternates floor/ceil — a ±1-clock jitter, deterministic") {
        std::array<long long, 256> intervals{};
        const int got = fractional.reset_intervals(std::span<long long>(intervals));
        REQUIRE(got == static_cast<int>(intervals.size()));

        long long lo = intervals[0], hi = intervals[0];
        double sum = 0.0;
        for (const long long m : intervals) {
            lo = std::min(lo, m);
            hi = std::max(hi, m);
            sum += static_cast<double>(m);
        }
        const double n_avg = static_cast<double>(std::uint64_t{1} << 24) /
                             static_cast<double>(fractional.tuning_word());
        const double measured_avg = sum / static_cast<double>(intervals.size());
        INFO("intervals in [" << lo << ", " << hi << "], mean " << measured_avg
                              << " vs N_avg " << n_avg);
        // The jitter is present (two distinct interval lengths appear)...
        CHECK(hi > lo);
        // ...and bounded to exactly one clock.
        CHECK(hi - lo == 1);
        // ...around the correct average period. The residue starts at 0 and lands
        // in [0, Δ) after M resets, so the mean interval is exact to within 1/M
        // clocks — asserting that, not a whole clock (a ~256x-slack tolerance that
        // would wave through a mis-scaled tuning word half a clock off centre).
        const double kMeanTol = 1.0 / static_cast<double>(intervals.size());
        CHECK_THAT(measured_avg, WithinAbs(n_avg, kMeanTol));

        // Deterministic: the same schedule twice is identical.
        std::array<long long, 256> again{};
        fractional.reset_intervals(std::span<long long>(again));
        CHECK(again == intervals);
    }

    SECTION("the rendered average pitch is closer to the note than integer-N") {
        constexpr int kLen = static_cast<int>(1.0 * kSampleRate);
        const double f_int = measured_pitch_hz(integer_profile(), note, kLen);
        const double f_frac = measured_pitch_hz(fractional_profile(24), note, kLen);
        const double eff = integer.effective_note_hz();
        INFO("measured: integer " << f_int << " Hz (" << cents_between(f_int, eff)
                                  << " cents), fractional " << f_frac << " Hz ("
                                  << cents_between(f_frac, eff) << " cents)");
        CHECK(std::fabs(cents_between(f_frac, eff)) < std::fabs(cents_between(f_int, eff)));
    }
}

TEST_CASE("the fractional-N jitter is carried into the rendered audio, not just "
          "the schedule",
          "[signal][osc][dco][fractional]") {
    // The schedule test above asserts the ±1-clock jitter on `reset_intervals`, a
    // pure integer-domain view. This proves the RENDER path (`next()`) actually
    // applies it: the reset intervals recovered from the rendered saw match the
    // schedule, and integer-N renders dead-periodic by contrast.
    //
    // At the realistic 1 MHz clock one clock is sr/f_clk = 0.048 sample — the
    // jitter is real but sub-sample, so it cannot be read off the sample grid. This
    // test therefore uses a clock EQUAL to the sample rate, where one clock is one
    // sample and each reset lands on an integer sample, making the scheduled
    // jitter directly observable in the audio without changing the model. f0 is
    // chosen so the ideal divider is distinctly non-integer (jitter guaranteed).
    const double f_clk = kSampleRate;             // one clock == one sample.
    const double note = 370.0;                    // ideal divider ~129.73 (non-integer).
    constexpr int kLen = 8192;                    // ~60 cycles.

    const auto frac_audio = render_dco(fractional_profile(24, f_clk), VaShape::saw, note, kLen);
    const auto int_audio = render_dco(integer_profile(f_clk), VaShape::saw, note, kLen);

    const auto frac_iv = reset_intervals_from_audio(frac_audio);
    const auto int_iv = reset_intervals_from_audio(int_audio);
    REQUIRE(frac_iv.size() > 20);
    REQUIRE(int_iv.size() > 20);

    // Integer-N renders dead-periodic: every recovered cycle is the same length.
    const int int_lo = *std::min_element(int_iv.begin(), int_iv.end());
    const int int_hi = *std::max_element(int_iv.begin(), int_iv.end());
    INFO("integer-N audio intervals in [" << int_lo << ", " << int_hi << "]");
    CHECK(int_lo == int_hi);

    // Fractional-N renders the jitter: exactly two cycle lengths, one clock (== one
    // sample here) apart, both present.
    const int frac_lo = *std::min_element(frac_iv.begin(), frac_iv.end());
    const int frac_hi = *std::max_element(frac_iv.begin(), frac_iv.end());
    INFO("fractional-N audio intervals in [" << frac_lo << ", " << frac_hi << "]");
    CHECK(frac_hi - frac_lo == 1);

    // The intervals recovered FROM THE AUDIO match the divider's own schedule,
    // clock-for-clock (one clock == one sample), aligned on the shared floor/ceil
    // pair — the render carries exactly the scheduled reset pattern.
    DcoOscillator sched;
    sched.prepare(kSampleRate);
    sched.set_profile(fractional_profile(24, f_clk));
    sched.set_note_hz(note);
    std::array<long long, 512> schedule{};
    const int got = sched.reset_intervals(std::span<long long>(schedule));
    REQUIRE(got == static_cast<int>(schedule.size()));

    // Find where the audio's interval stream first appears in the schedule (the
    // audio drops a partial lead-in cycle, so it may start a phase into the pattern)
    // and confirm a long run matches exactly.
    const int probe = frac_iv[0];
    std::size_t offset = 0;
    bool found = false;
    for (std::size_t s = 0; s + frac_iv.size() < schedule.size(); ++s) {
        if (static_cast<int>(schedule[s]) != probe) continue;
        bool all = true;
        for (std::size_t j = 0; j < frac_iv.size(); ++j) {
            if (static_cast<int>(schedule[s + j]) != frac_iv[j]) { all = false; break; }
        }
        if (all) { offset = s; found = true; break; }
    }
    INFO("matched audio intervals against schedule at offset " << offset
         << " over " << frac_iv.size() << " cycles");
    CHECK(found);
}

TEST_CASE("fractional-N output stays bounded and finite through the jittered resets",
          "[signal][osc][dco][fractional]") {
    // The grid-snapped sync resets could compose a natural wrap and a forced sync
    // in one sample; the shared corrector sums coincident steps, so a saw must stay
    // inside its own range with no blow-up.
    for (const long k : {80L, 512L, 4096L}) {
        const auto render = render_dco(fractional_profile(24), VaShape::saw, worst_case_note(k));
        bool finite = true;
        for (const double v : render) finite = finite && std::isfinite(v);
        INFO("k=" << k << ": peak " << peak(render));
        CHECK(finite);
        CHECK(peak(render) <= 1.0 + 1e-6);
    }
}

// ── Alias rejection through the shared BLEP path ──────────────────────────────

TEST_CASE("DCO alias measurement separates clean from known-bad", "[signal][osc][dco][alias]") {
    // The negative control the DCO alias gate is read through: the analyzer's own
    // floor assumes a white residual (false — aliases are discrete tones), so proof
    // is by construction. Read at the DCO's REALISED fundamental, since that is the
    // quantized pitch its harmonics actually sit on.
    const DcoProfile profile = integer_profile(kFastClockHz);
    for (const double note : {1103.0, 2153.0, 4100.0, 6301.0}) {
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(note);
        const double f0 = osc.realized_hz();

        const int h = first_in_band_alias_harmonic(f0);
        REQUIRE(h > 0);
        const double site_hz = fold_frequency(h * f0, kSampleRate);

        SECTION("alias removed: the reading collapses") {
            const auto report = analyze(bandlimited_saw(f0), f0);
            INFO("note " << note << " (f0 " << f0 << "): clean saw reads "
                         << report.worst_alias_db << " dBc");
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            CHECK(report.worst_alias_db < -140.0);
        }

        SECTION("alias injected at a known level: it is recovered") {
            const double truth_db = -100.0;
            const double amplitude = std::pow(10.0, truth_db / 20.0) * kBandlimitedFundamental;
            const auto report = analyze(bandlimited_saw(f0, site_hz, amplitude), f0);
            INFO("note " << note << ": injected " << truth_db << " dBc at " << site_hz
                         << " Hz; read " << report.worst_alias_db << " dBc");
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            CHECK(report.worst_alias_index == h);
            CHECK_THAT(report.worst_alias_db, WithinAbs(truth_db, 1.0));
        }
    }
}

TEST_CASE("the DCO saw improves alias rejection below 20 kHz", "[signal][osc][dco][alias]") {
    // The DCO saw rides the shared BLEP path, so it inherits VaOscillator's
    // polyBLEP rejection: the real per-note number is logged, and the gate is a
    // regression tripwire well below it (and far above 0 dB = did nothing).
    const DcoProfile profile = integer_profile(kFastClockHz);
    for (const double note : {1103.0, 2153.0, 4100.0, 6301.0}) {
        DcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_profile(profile);
        osc.set_note_hz(note);
        const double f0 = osc.realized_hz();

        const auto trivial = analyze(render_trivial_saw(f0), f0);
        const auto corrected = analyze(render_dco(profile, VaShape::saw, note), f0);
        const double improvement = trivial.worst_alias_db - corrected.worst_alias_db;

        INFO("note " << note << " (f0 " << f0 << "): worst in-band alias "
                     << trivial.worst_alias_db << " dBc trivial -> " << corrected.worst_alias_db
                     << " dBc corrected = " << improvement << " dB improvement");
        REQUIRE_FALSE(corrected.has_unresolved_in_band_alias);
        REQUIRE(corrected.worst_alias_db > corrected.detection_floor_db);
        REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
        CHECK(improvement >= 8.0);
        CHECK(corrected.worst_alias_db > -120.0);
    }
}

// ── No drift: the DCO pitch is stationary ─────────────────────────────────────

TEST_CASE("a DCO has no drift: its held pitch is stationary over a long render",
          "[signal][osc][dco][drift]") {
    // The crystal clock has no wander, so the held pitch does not drift — the exact
    // opposite of the VCO's drift test, and the reason a DCO profile carries no
    // drift knob (there is none to set on DcoOscillator by construction). Over 8 s
    // the f0(t) trajectory is flat: tiny frame-to-frame RMS and, crucially, NO
    // trend between the first and second half.
    constexpr int kLen = static_cast<int>(8.0 * kSampleRate);
    constexpr int kWin = 8192;
    constexpr int kHop = 8192;
    const double note = worst_case_note(600); // ~1.66 kHz.

    for (const DcoProfile& profile : {integer_profile(), fractional_profile(24)}) {
        const auto sig = render_dco_f(profile, VaShape::sine, note, kLen);
        PitchTrackOptions options;
        options.window_length = kWin;
        options.hop_length = kHop;
        const auto track = track_pitch(std::span<const float>(sig), kSampleRate, options);
        const std::vector<double> hz = track.voiced_hz();
        REQUIRE(hz.size() > 40);

        double log_mean = 0.0;
        for (const double f : hz) log_mean += std::log2(f);
        log_mean /= static_cast<double>(hz.size());

        double sumsq = 0.0, first = 0.0, second = 0.0;
        int nf = 0, ns = 0;
        for (std::size_t i = 0; i < hz.size(); ++i) {
            const double c = 1200.0 * (std::log2(hz[i]) - log_mean);
            sumsq += c * c;
            if (i < hz.size() / 2) { first += c; ++nf; }
            else { second += c; ++ns; }
        }
        const double rms_cents = std::sqrt(sumsq / static_cast<double>(hz.size()));
        const double drift_between_halves = std::fabs(second / ns - first / nf);
        const bool is_fractional = profile.divider_scheme == DcoDivider::fractional_n;
        INFO((is_fractional ? "fractional" : "integer")
             << ": f0(t) RMS " << rms_cents << " cents, half-to-half shift "
             << drift_between_halves << " cents over " << hz.size() << " frames");
        CHECK(rms_cents < 0.5);            // stationary, no wander.
        CHECK(drift_between_halves < 0.15); // and no slow trend either way.
    }
}

// ── Determinism ───────────────────────────────────────────────────────────────

TEST_CASE("DCO output is deterministic for both divider schemes", "[signal][osc][dco]") {
    for (const DcoProfile& profile : {integer_profile(), fractional_profile(24)}) {
        for (const VaShape shape : {VaShape::saw, VaShape::square}) {
            const double note = worst_case_note(211);
            const auto a = render_dco(profile, shape, note, kRenderLength, 0.11);
            const auto b = render_dco(profile, shape, note, kRenderLength, 0.11);
            int differing = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i]) ++differing;
            INFO((profile.divider_scheme == DcoDivider::fractional_n ? "fractional" : "integer")
                 << " shape " << static_cast<int>(shape) << ": " << differing << " differ");
            CHECK(differing == 0);
        }
    }
}

TEST_CASE("footage and fine-tune shift the derived divider exactly",
          "[signal][osc][dco][tuning]") {
    // Footage is an exact power-of-two octave scaling, and fine-tune folds cleanly
    // into the note before N is derived — so the effective note, and hence the
    // realised pitch one octave up, tracks 2x to the bit-limited divider.
    const double note = 440.0;

    DcoOscillator base;
    base.prepare(kSampleRate);
    base.set_profile(integer_profile());
    base.set_note_hz(note);

    DcoProfile up = integer_profile();
    up.octave_shift = 1;
    DcoOscillator octave;
    octave.prepare(kSampleRate);
    octave.set_profile(up);
    octave.set_note_hz(note);
    INFO("effective note base " << base.effective_note_hz() << " Hz, +1 octave "
                                << octave.effective_note_hz() << " Hz");
    CHECK_THAT(octave.effective_note_hz(), WithinRel(2.0 * base.effective_note_hz(), 1e-12));

    DcoProfile tuned = integer_profile();
    tuned.fine_tune_cents = 25.0;
    DcoOscillator detuned;
    detuned.prepare(kSampleRate);
    detuned.set_profile(tuned);
    detuned.set_note_hz(note);
    CHECK_THAT(detuned.effective_note_hz(),
               WithinRel(note * std::exp2(25.0 / 1200.0), 1e-12));
}
