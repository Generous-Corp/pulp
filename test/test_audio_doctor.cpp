// test_audio_doctor.cpp — offline Audio Doctor analyzers:
// magnitude/frequency response and THD/THD+N.
//
// ── Analyzer Determinism Contract (uniform across this file) ───────────────
// All renders are deterministic: sine/impulse stimulus (no seed, no clock),
// the headless block-loop render, and pure-arithmetic FFT analysis. dB facts
// are RATIOS (response = output/input, THD = harmonics/fundamental), so the
// FFT backend's absolute normalization cancels and the asserted numbers are
// backend-stable. Per-test specifics — window, FFT length, analysis offset,
// tone coherence, sample rate — are stated on each TEST_CASE and echoed into
// the curve artifact. Tolerance class is "numeric" unless noted otherwise.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "support/audio_doctor.hpp"
#include <pulp/audio/analysis/audio_doctor_artifacts.hpp>

#include "pulp_effect.hpp"

#include <choc/text/choc_JSON.h>

#include <pulp/format/processor.hpp>
#include <pulp/signal/halfband_iir.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace pulp::test::audio;
using namespace pulp::examples;

namespace {

constexpr double kSampleRate = 48000.0;

// ── Test-only processors for THD discrimination ────────────────────────────
// A clean unity passthrough and a hard clipper. Both stereo in/out, no
// parameters — the smallest processors that prove the THD analyzer
// discriminates a clean tone from a distorted one. Test-side only.

class PassthroughProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorPassthrough",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.passthrough",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels();
             ++ch)
            std::copy(in.channel(ch).begin(), in.channel(ch).end(),
                      out.channel(ch).begin());
    }
};

// Symmetric hard clipper at ±0.4 — a 0.5-amplitude sine is driven well into
// the clip, generating strong odd harmonics (the canonical "clearly higher
// THD" reference).
class HardClipProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorHardClip",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.hardclip",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        constexpr float kClip = 0.4f;
        for (std::size_t ch = 0; ch < out.num_channels() && ch < in.num_channels();
             ++ch) {
            auto i = in.channel(ch);
            auto o = out.channel(ch);
            for (std::size_t n = 0; n < o.size(); ++n)
                o[n] = std::clamp(i[n], -kClip, kClip);
        }
    }
};

// ── Test-only processors with a known-truth group delay ────────────────────
// Two processors whose group delay is known analytically, and one wrapping the
// shipped half-band design. They are the calibration standards for the
// group-delay analyzer: if it cannot recover a textbook delay it cannot be
// trusted on a real filter.

// A pure delay line. h[n] = delta[n - kDelay], so |H| = 1 at every frequency
// and the group delay is exactly kDelay samples, flat across the whole
// spectrum — the simplest possible ground truth.
class DelayLineProcessor : public pulp::format::Processor {
public:
    static constexpr int kDelay = 17;

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorDelayLine",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.delayline",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {
        for (auto& ch : history_) ch.fill(0.0f);
        pos_ = 0;
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const std::size_t channels =
            std::min(out.num_channels(), in.num_channels());
        if (channels == 0)
            return;
        for (std::size_t n = 0; n < out.channel(0).size(); ++n) {
            for (std::size_t ch = 0; ch < channels && ch < history_.size(); ++ch) {
                out.channel(ch)[n] = history_[ch][pos_];
                history_[ch][pos_] = in.channel(ch)[n];
            }
            pos_ = (pos_ + 1) % kDelay;
        }
    }

private:
    std::array<std::array<float, kDelay>, 2> history_{};
    std::size_t pos_ = 0;
};

// An N-tap linear-phase FIR: the length-65 moving average. All taps equal, so
// the impulse response is symmetric — the textbook Type-I linear-phase case,
// whose group delay is exactly (N-1)/2 = 32 samples at EVERY frequency,
// independent of the (sinc-shaped) magnitude response. Its magnitude nulls
// double as a stopband probe.
class LinearPhaseFirProcessor : public pulp::format::Processor {
public:
    static constexpr int kTaps = 65;
    static constexpr double kExpectedGroupDelay = (kTaps - 1) / 2.0; // 32.0

    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorLinearPhaseFir",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.linearphasefir",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {
        for (auto& ch : history_) ch.fill(0.0f);
        pos_ = 0;
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const std::size_t channels =
            std::min(out.num_channels(), in.num_channels());
        if (channels == 0)
            return;
        for (std::size_t n = 0; n < out.channel(0).size(); ++n) {
            for (std::size_t ch = 0; ch < channels && ch < history_.size(); ++ch) {
                history_[ch][pos_] = in.channel(ch)[n];
                // Accumulate in double: the analyzer is calibrating against an
                // analytic delay, so the reference must not carry avoidable
                // summation error of its own.
                double sum = 0.0;
                for (float v : history_[ch]) sum += v;
                out.channel(ch)[n] = static_cast<float>(sum / kTaps);
            }
            pos_ = (pos_ + 1) % kTaps;
        }
    }

private:
    std::array<std::array<float, kTaps>, 2> history_{};
    std::size_t pos_ = 0;
};

// The shipped default half-band design, wired as the round trip it is deployed
// as: upsample 2x, then immediately downsample 2x. Both ends run at the
// scenario's rate, so the measured group delay is expressed in samples at the
// half-band's input rate — the rate its documented delay figure is quoted at.
class HalfBandRoundTripProcessor : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "DoctorHalfBandRoundTrip",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.doctor.halfband",
                .version = "1.0.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {
        for (auto& u : up_) u.reset();
        for (auto& d : down_) d.reset();
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const std::size_t channels =
            std::min(out.num_channels(), in.num_channels());
        for (std::size_t ch = 0; ch < channels && ch < up_.size(); ++ch) {
            auto i = in.channel(ch);
            auto o = out.channel(ch);
            for (std::size_t n = 0; n < o.size(); ++n) {
                float lo = 0.0f, hi = 0.0f;
                up_[ch].process(i[n], lo, hi);
                o[n] = down_[ch].process(lo, hi);
            }
        }
    }

private:
    std::array<pulp::signal::HalfBandUpsampler2x, 2> up_{};
    std::array<pulp::signal::HalfBandDownsampler2x, 2> down_{};
};

std::unique_ptr<pulp::format::Processor> create_passthrough() {
    return std::make_unique<PassthroughProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_hard_clip() {
    return std::make_unique<HardClipProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_delay_line() {
    return std::make_unique<DelayLineProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_linear_phase_fir() {
    return std::make_unique<LinearPhaseFirProcessor>();
}
std::unique_ptr<pulp::format::Processor> create_halfband_round_trip() {
    return std::make_unique<HalfBandRoundTripProcessor>();
}

// Closed-form group delay at DC of a cascade of first-order allpass sections
// H(z) = (a + z^-1) / (1 + a*z^-1), whose delay at DC is (1-a)/(1+a). Lets the
// half-band cases predict their own answer from the shipped coefficients
// instead of asserting a magic number.
double allpass_dc_group_delay(std::span<const float> coefficients) {
    double tau = 0.0;
    for (float a : coefficients)
        tau += (1.0 - static_cast<double>(a)) / (1.0 + static_cast<double>(a));
    return tau;
}

// Worst deviation of the measured group delay from `expected` over every
// defined bin in [lo_hz, hi_hz]. Undefined bins are skipped — that is the
// stopband contract, not a gap in the check.
struct DelaySpread {
    double worst_deviation = 0.0;
    double worst_hz = 0.0;
    int defined_bins = 0;
};

DelaySpread delay_spread(const PhaseCurve& curve, double expected, double lo_hz,
                         double hi_hz) {
    DelaySpread s;
    for (const auto& p : curve.full) {
        if (!p.defined || p.hz < lo_hz || p.hz > hi_hz)
            continue;
        ++s.defined_bins;
        const double dev = std::abs(p.group_delay_samples - expected);
        if (dev > s.worst_deviation) {
            s.worst_deviation = dev;
            s.worst_hz = p.hz;
        }
    }
    return s;
}

RenderScenario lowpass_scenario(const char* name, float cutoff_hz) {
    // Input/duration are overridden by the analyzer (it drives the impulse).
    return RenderScenario(create_pulp_effect)
        .name(name)
        .sample_rate(kSampleRate)
        .block_size(256)
        .set_param(kFrequency, cutoff_hz)
        .set_param(kResonance, 0.707f)
        .set_param(kFilterType, 0.0f) // lowpass
        .set_param(kMix, 100.0f);
}

// A bin-coherent fundamental for the THD analyzer: k cycles in fft_length.
double coherent_tone(int k, int fft_length, double sample_rate) {
    return static_cast<double>(k) * sample_rate / fft_length;
}

// ── Window-leakage fixtures ────────────────────────────────────────────────

constexpr int kLeakFft = 16384;
constexpr double kLeakBin = kSampleRate / kLeakFft; // ≈ 2.93 Hz

// Sum of up to two sines with a DOUBLE-precision phase, narrowed once on store.
// The shared make_sine helpers accumulate phase in float, which smears a
// −100 dBc component beyond recognition over 16384 samples — this suite needs
// the stimulus itself to be cleaner than the thing it measures.
pulp::audio::Buffer<float> make_two_tone(int samples, double sample_rate,
                                         double loud_hz, double loud_amp,
                                         double quiet_hz, double quiet_amp) {
    pulp::audio::Buffer<float> buf(1, samples);
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        double v = loud_amp * std::sin(2.0 * std::numbers::pi * loud_hz * t);
        if (quiet_amp > 0.0) {
            // Arbitrary fixed phase offset: the quiet tone must not be
            // phase-locked to the loud one, or leakage could cancel it by
            // construction and flatter the result.
            v += quiet_amp *
                 std::sin(2.0 * std::numbers::pi * quiet_hz * t + 0.7);
        }
        buf.channel(0)[i] = static_cast<float>(v);
    }
    return buf;
}

// dB the analyzer reports at `probe_hz` for a 0 dB tone at `loud_hz` plus an
// optional quiet tone at `probe_hz`. With quiet_amp = 0 this is the window's
// pure leakage at that bin — the control that proves whether a reading is
// signal or self-noise.
double reading_at(Window w, double loud_hz, double probe_hz, double quiet_amp,
                  double kaiser_beta = kDefaultKaiserBeta) {
    const auto sig = make_two_tone(kLeakFft, kSampleRate, loud_hz, 1.0,
                                   probe_hz, quiet_amp);
    ResponseOptions opts;
    opts.fft_length = kLeakFft;
    opts.window = w;
    opts.kaiser_beta = kaiser_beta;
    const auto curve = magnitude_spectrum_curve(std::as_const(sig).view(),
                                                kSampleRate, {}, opts);
    return curve.magnitude_db_at(probe_hz);
}

// ── Alias-analyzer fixtures ────────────────────────────────────────────────

constexpr int kAliasFft = 16384;      // Fit length; not required to be a power
                                      // of two — the projection has no FFT.
constexpr double kAliasF0 = 4100.0;   // Chosen so no fold site lands near DC,
                                      // near Nyquist, or on another (verified
                                      // by the fold table in the tests below).

struct ToneSpec {
    double hz = 0.0;
    double amplitude = 0.0;
};

// Sum of arbitrarily many sines with DOUBLE-precision phase, narrowed once on
// store — the same reasoning as make_two_tone: a stimulus meant to prove a
// −100 dBc measurement must itself be cleaner than the thing being measured,
// and float phase accumulation is not.
//
// Each tone gets a distinct, non-zero phase offset so nothing is phase-locked
// to the fundamental. A ground-truth fixture whose alias happened to be in
// quadrature with the tone that is supposed to swamp it would flatter the
// analyzer for free.
pulp::audio::Buffer<float> make_series(int samples, double sample_rate,
                                       std::span<const ToneSpec> tones) {
    pulp::audio::Buffer<float> buf(1, samples);
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        double v = 0.0;
        for (std::size_t k = 0; k < tones.size(); ++k) {
            const double phase_offset = 0.31 * static_cast<double>(k) + 0.7;
            v += tones[k].amplitude *
                 std::sin(2.0 * std::numbers::pi * tones[k].hz * t + phase_offset);
        }
        buf.channel(0)[i] = static_cast<float>(v);
    }
    return buf;
}

// A bandlimited "saw-ish" base: fundamental plus the four harmonics that fit
// under Nyquist at kAliasF0. Every one of these is legitimate content — the
// analyzer must never call any of it an alias. Tests append their own injected
// alias tones to this.
std::vector<ToneSpec> bandlimited_base() {
    return {{kAliasF0, 1.0},
            {kAliasF0 * 2, 0.50},
            {kAliasF0 * 3, 0.33},
            {kAliasF0 * 4, 0.25},
            {kAliasF0 * 5, 0.20}};
}

// The component the report attributed to harmonic `h`.
const AliasComponent& component(const AliasReport& report, int h) {
    REQUIRE(static_cast<int>(report.components.size()) >= h);
    const auto& c = report.components[static_cast<std::size_t>(h - 1)];
    REQUIRE(c.index == h);
    return c;
}

// Worst (highest) side lobe within ±`span` bins of `centre_bin`, skipping the
// main lobe (|bin − center| ≤ `main_lobe_radius`). The scan is bounded to the
// tone's neighborhood on purpose: DC removal of a non-integer-cycle tone leaves
// a pedestal near bin 0 that is louder than every far side lobe and would win
// an unbounded scan while saying nothing about leakage.
double worst_side_lobe_db(const ResponseCurve& curve, double centre_bin,
                          int main_lobe_radius, int span = 300) {
    double worst = -1e9;
    const int lo = std::max(1, static_cast<int>(centre_bin) - span);
    const int hi = std::min(static_cast<int>(curve.full.size()) - 1,
                            static_cast<int>(centre_bin) + span);
    for (int i = lo; i <= hi; ++i) {
        if (std::abs(i - centre_bin) <= main_lobe_radius)
            continue;
        worst = std::max(worst, curve.full[static_cast<std::size_t>(i)].magnitude_db);
    }
    return worst;
}

} // namespace

TEST_CASE("Doctor: PulpEffect lowpass magnitude response", "[audio][doctor]") {
    // Determinism contract: unit-impulse stimulus at frame 0, rectangular
    // window over a 16384-sample segment at offset 0, 48 kHz, response =
    // output/input dB. bin resolution = 48000/16384 ≈ 2.93 Hz. Tolerance
    // class: numeric — the passband flatness and the 8 kHz attenuation use
    // generous dB margins; the impulse spectrum is flat so the ratio is a
    // true transfer response (FFT-backend scale cancels).
    auto scenario = lowpass_scenario("doctor.lowpass-response", 200.0f);
    ResponseOptions opts;
    opts.fft_length = 16384;
    opts.window = Window::rectangular;
    const double checkpoints[] = {50.0, 8000.0};
    const auto curve =
        response_relative_to_input(scenario, checkpoints, opts);

    INFO("DC bin " << curve.magnitude_db_at(0.0) << " dB; 50 Hz "
                   << curve.magnitude_db_at(50.0) << " dB; 8 kHz "
                   << curve.magnitude_db_at(8000.0) << " dB; attenuation "
                   << curve.attenuation_db_at(8000.0) << " dB");

    // Passband (well below the 200 Hz cutoff) is ~flat near unity: within
    // 3 dB of 0 dB at 50 Hz.
    CHECK(std::abs(curve.magnitude_db_at(50.0)) < 3.0);

    // The golden contract: an 8 kHz tone drops by >= 20 dB through a 200 Hz
    // lowpass. This is now a direct response measurement, not a folded RMS
    // bound. (Measured headroom is far larger; 20 dB is the golden floor.)
    CHECK(curve.attenuation_db_at(8000.0) >= 20.0);

    // Curve carries the determinism-contract fields for review.
    CHECK(curve.fft_length == 16384);
    CHECK(curve.window == Window::rectangular);
    CHECK(curve.bin_hz > 2.9);
    CHECK(curve.bin_hz < 3.0);
    REQUIRE(curve.full.size() == 16384 / 2 + 1);
}

TEST_CASE("Doctor: group delay recovers a pure delay line",
          "[audio][doctor][group-delay]") {
    // Ground truth #1. h[n] = delta[n-17]: |H| = 1 everywhere, group delay
    // exactly 17 samples at every frequency. Nothing is undefined, so the flat
    // delay must hold across the entire spectrum. Determinism contract:
    // unit-impulse stimulus at frame 0, rectangular window, 16384-sample
    // segment at offset 0, 48 kHz. Tolerance class: numeric — the estimator is
    // analytically exact for a signal wholly inside the window, so the margin
    // is numerical only.
    auto scenario = RenderScenario(create_delay_line)
                        .name("doctor.group-delay-pure-delay")
                        .sample_rate(kSampleRate)
                        .block_size(256);
    GroupDelayOptions opts;
    opts.fft_length = 16384;
    const double checkpoints[] = {100.0, 1000.0, 10000.0};
    const auto curve = measure_group_delay(scenario, checkpoints, opts);

    const auto spread = delay_spread(curve, DelayLineProcessor::kDelay, 0.0,
                                     kSampleRate / 2.0);
    INFO("expected " << DelayLineProcessor::kDelay << " samples; worst "
                     << "deviation " << spread.worst_deviation << " at "
                     << spread.worst_hz << " Hz over " << spread.defined_bins
                     << " defined bins");

    // An allpass delay is defined at every bin — no stopband anywhere.
    REQUIRE(spread.defined_bins == 16384 / 2 + 1);
    CHECK(spread.worst_deviation < 0.01);

    // The ergonomic accessors report the same fact, in both units.
    CHECK(curve.defined_at(1000.0));
    CHECK_THAT(curve.group_delay_samples_at(1000.0),
               Catch::Matchers::WithinAbs(DelayLineProcessor::kDelay, 0.01));
    CHECK_THAT(curve.group_delay_seconds_at(1000.0),
               Catch::Matchers::WithinAbs(DelayLineProcessor::kDelay / kSampleRate,
                                          1e-7));

    // Curve carries the determinism-contract fields for review.
    CHECK(curve.analyzer == "group_delay");
    CHECK(curve.fft_length == 16384);
    CHECK(curve.window == Window::rectangular);
    REQUIRE(curve.checkpoints.size() == 3);
}

TEST_CASE("Doctor: group delay recovers a linear-phase FIR",
          "[audio][doctor][group-delay]") {
    // Ground truth #2 and the estimator's real calibration. A 65-tap
    // linear-phase FIR has group delay exactly (65-1)/2 = 32 samples, FLAT at
    // every frequency — including across its magnitude nulls, where the phase
    // steps by pi but the delay does not change. Recovering a flat 32 from a
    // sinc-shaped magnitude response is what proves the estimator measures
    // delay rather than magnitude. Determinism contract: as above.
    auto scenario = RenderScenario(create_linear_phase_fir)
                        .name("doctor.group-delay-linear-phase-fir")
                        .sample_rate(kSampleRate)
                        .block_size(256);
    GroupDelayOptions opts;
    opts.fft_length = 16384;
    const double checkpoints[] = {100.0, 500.0};
    const auto curve = measure_group_delay(scenario, checkpoints, opts);

    constexpr double kExpected = LinearPhaseFirProcessor::kExpectedGroupDelay;
    const auto spread = delay_spread(curve, kExpected, 0.0, kSampleRate / 2.0);
    INFO("expected " << kExpected << " samples; worst deviation "
                     << spread.worst_deviation << " at " << spread.worst_hz
                     << " Hz over " << spread.defined_bins << " defined bins");

    // Flat (N-1)/2 across every bin the magnitude gate admits — the whole
    // sinc, main lobe and side lobes alike, not just the passband.
    REQUIRE(spread.defined_bins > 1000);
    CHECK(spread.worst_deviation < 0.01);

    // Passband spot check via the accessor.
    CHECK(curve.defined_at(100.0));
    CHECK_THAT(curve.group_delay_samples_at(100.0),
               Catch::Matchers::WithinAbs(kExpected, 0.01));
}

TEST_CASE("Doctor: half-band round-trip group delay", "[audio][doctor][group-delay]") {
    // The shipped default half-band design (six allpass sections per path),
    // measured as the 2x-up -> 2x-down round trip it is deployed as. Both ends
    // run at 48 kHz so the delay is expressed in samples at the half-band's
    // input rate. Unlike the two calibration cases this is a minimum-phase
    // IIR: its group delay is frequency-DEPENDENT, so the check is the
    // passband value plus the shape, not a single flat number.
    //
    // Determinism contract: unit impulse at frame 0, rectangular window,
    // 16384-sample segment at offset 0, 48 kHz. The IIR's response decays far
    // inside the window, so estimator truncation is not a factor. Tolerance
    // class: numeric.
    auto scenario = RenderScenario(create_halfband_round_trip)
                        .name("doctor.group-delay-halfband")
                        .sample_rate(kSampleRate)
                        .block_size(256);
    GroupDelayOptions opts;
    opts.fft_length = 16384;
    // The design's stated passband is up to 0.4 * Nyquist of the half-band's
    // input rate (9.6 kHz at 48 kHz); probe across it and beyond.
    const double checkpoints[] = {100.0, 1000.0, 5000.0, 9600.0};
    const auto curve = measure_group_delay(scenario, checkpoints, opts);

    // Independent ground truth from the shipped coefficients themselves, not a
    // magic number: an oversampling round trip cascades both allpass paths
    // (the two paths are LTI and commute, so up-then-down collapses to
    // A(z)*B(z) at the input rate), and a first-order allpass (a + z^-1) /
    // (1 + a*z^-1) has group delay (1-a)/(1+a) at DC. Summing that over both
    // paths predicts the low-frequency delay in closed form.
    const double expected_dc =
        allpass_dc_group_delay(pulp::signal::kDefaultCoefficientsA) +
        allpass_dc_group_delay(pulp::signal::kDefaultCoefficientsB);

    INFO("half-band round-trip group delay (samples at 48 kHz): 100 Hz "
         << curve.group_delay_samples_at(100.0) << "; 1 kHz "
         << curve.group_delay_samples_at(1000.0) << "; 5 kHz "
         << curve.group_delay_samples_at(5000.0) << "; 9.6 kHz "
         << curve.group_delay_samples_at(9600.0)
         << "; closed-form DC prediction " << expected_dc
         << "; magnitude at 9.6 kHz " << curve.magnitude_db_at(9600.0) << " dB");

    // The passband is defined — the round trip is allpass (unity magnitude at
    // every frequency), so there is real signal to measure a phase from
    // everywhere, and nothing is gated out.
    REQUIRE(curve.defined_at(100.0));
    REQUIRE(curve.defined_at(9600.0));

    // The measurement matches the closed-form prediction near DC. This is the
    // check that ties the analyzer to the real shipped coefficients.
    CHECK_THAT(curve.group_delay_samples_at(100.0),
               Catch::Matchers::WithinAbs(expected_dc, 0.01));

    // Group delay of a minimum-phase IIR RISES toward the transition band —
    // the defining property that makes a scalar latency claim unsafe and this
    // curve necessary. Flat-delay assumptions do not hold here.
    CHECK(curve.group_delay_samples_at(9600.0) >
          curve.group_delay_samples_at(100.0) + 0.5);
}

TEST_CASE("Doctor: half-band filter group delay at its own rate",
          "[audio][doctor][group-delay]") {
    // The half-band filter itself, rather than the oversampling round trip.
    // The 2x upsampler realises H(z) = A(z^2) + z^-1 * B(z^2) at its OUTPUT
    // rate: zero-stuffing an impulse leaves an impulse, so driving the
    // upsampler with a unit impulse at the input rate and reading its
    // interleaved output against an impulse reference at 2x gives H's transfer
    // function directly. Buffer-level analyzer — no processor needed, because
    // the stage changes rate and a Processor cannot express that.
    //
    // Determinism contract: impulse at frame 0, rectangular window, 16384-
    // sample segment at offset 0, analyzed at 96 kHz (the 2x rate). Tolerance
    // class: numeric.
    constexpr int kFft = 16384;
    constexpr double kUpRate = kSampleRate * 2.0;

    auto input = make_impulse(/*channels=*/1, kFft / 2, 1.0f, /*position=*/0);
    // Reference at the 2x rate: the zero-stuffed impulse is still a unit
    // impulse at frame 0, which is exactly what H(z) is driven by.
    auto reference = make_impulse(/*channels=*/1, kFft, 1.0f, /*position=*/0);
    pulp::audio::Buffer<float> upsampled(1, kFft);
    pulp::signal::HalfBandUpsampler2x up;
    up.process_block(std::as_const(input).view().channel(0),
                     upsampled.view().channel(0));

    GroupDelayOptions opts;
    opts.fft_length = kFft;
    const double checkpoints[] = {100.0, 9600.0, 40000.0};
    const auto curve = measure_group_delay(
        std::as_const(reference).view(), std::as_const(upsampled).view(),
        kUpRate, checkpoints, opts);

    // Closed form for H(z) = A(z^2) + z^-1 * B(z^2) at DC, in samples at the
    // 2x rate: substituting z^2 doubles each path's delay, the z^-1 adds one
    // sample to the B branch, and in the passband the two branches are in
    // phase so the sum's delay is their average.
    const double tau_paths =
        allpass_dc_group_delay(pulp::signal::kDefaultCoefficientsA) +
        allpass_dc_group_delay(pulp::signal::kDefaultCoefficientsB);
    const double expected_dc_at_2x = (2.0 * tau_paths + 1.0) / 2.0;

    const double measured_2x = curve.group_delay_samples_at(100.0);
    INFO("half-band filter group delay at DC: " << measured_2x
         << " samples at the 2x rate (closed form " << expected_dc_at_2x
         << "), i.e. " << measured_2x / 2.0
         << " samples at the half-band's input rate. Stopband probe at 40 kHz: "
         << curve.magnitude_db_at(40000.0) << " dB, defined="
         << curve.defined_at(40000.0));

    REQUIRE(curve.defined_at(100.0));
    CHECK_THAT(measured_2x,
               Catch::Matchers::WithinAbs(expected_dc_at_2x, 0.01));

    CHECK(curve.defined_at(9600.0));

    // Above the half-band's Fs/4 cutoff (24 kHz at the 2x rate) the design
    // attenuates, but only moderately — this is the documented "Tier-2"
    // trade-off, ~-40 dB at 40 kHz. That is still well ABOVE the -60 dB gate,
    // so these bins stay defined and their group delay is a real measurement.
    // The gate is a signal-present test, not a passband test: a filter with a
    // shallow stopband is measurable there, and this analyzer says so rather
    // than discarding data it does have.
    CHECK(curve.magnitude_db_at(40000.0) < -20.0);
    CHECK(curve.defined_at(40000.0));
    CHECK(std::isfinite(curve.group_delay_samples_at(40000.0)));
}

TEST_CASE("Doctor: THD discriminates clean vs clipped sine", "[audio][doctor]") {
    // Determinism contract: bin-coherent 1 kHz-ish tone (exactly k cycles in
    // 16384 samples at 48 kHz → rectangular window, zero leakage), amplitude
    // 0.5, 8 harmonics summed, analysis offset 0. THD is harmonics/fundamental
    // (ratio). Tolerance class: numeric — clean THD is bounded well below the
    // clipped THD with a wide separation margin.
    constexpr int kFft = 16384;
    const double tone = coherent_tone(341, kFft, kSampleRate); // ~999 Hz, coherent
    ThdOptions opts;
    opts.fft_length = kFft;
    opts.num_harmonics = 8;
    opts.amplitude = 0.5f;

    auto clean =
        measure_thd(RenderScenario(create_passthrough)
                        .name("doctor.thd-clean")
                        .sample_rate(kSampleRate)
                        .block_size(256),
                    tone, opts);
    auto clipped =
        measure_thd(RenderScenario(create_hard_clip)
                        .name("doctor.thd-clipped")
                        .sample_rate(kSampleRate)
                        .block_size(256),
                    tone, opts);

    INFO("clean THD " << clean.thd_percent() << "% (" << clean.thd_db()
                      << " dB); clipped THD " << clipped.thd_percent() << "% ("
                      << clipped.thd_db() << " dB)");

    // The coherent tone must be recognized as coherent (no leakage path).
    CHECK(clean.coherent);
    CHECK(clipped.coherent);

    // A pure passthrough sine has essentially no harmonic content: THD well
    // below 0.1% (−60 dB). The hard clipper generates strong odd harmonics:
    // THD clearly above 1% (−40 dB). The separation is the discrimination.
    CHECK(clean.thd < 0.001);
    CHECK(clipped.thd > 0.05);
    CHECK(clipped.thd > clean.thd * 50.0);

    // THD+N >= THD (it includes everything non-fundamental), and the clipped
    // case is dominated by harmonics so they track closely.
    CHECK(clipped.thd_plus_n >= clipped.thd);

    // Harmonic breakdown present: [0] fundamental, then 2nd..9th.
    REQUIRE(clipped.harmonics.size() >= 2);
    CHECK(clipped.harmonics.front().index == 1);
}

TEST_CASE("Doctor: curve artifact round-trips", "[audio][doctor]") {
    // Determinism contract: same as the response test; this asserts the
    // artifact schema, not DSP. Tolerance class: exact (field presence).
    auto scenario = lowpass_scenario("doctor.artifact-roundtrip", 1000.0f);
    const double checkpoints[] = {100.0, 5000.0};
    ResponseOptions opts;
    opts.fft_length = 8192;
    const auto curve = response_relative_to_input(scenario, checkpoints, opts);

    const auto path = write_response_artifact(curve, "doctor.artifact-roundtrip");
    REQUIRE(std::filesystem::exists(path));

    const std::string json = response_curve_to_json(curve, "doctor.artifact");
    const auto parsed = choc::json::parse(json);
    REQUIRE(parsed.isObject());
    CHECK(parsed["schema_version"].getWithDefault<int64_t>(-1) ==
          kDoctorCurveSchemaVersion);
    CHECK(parsed["analyzer"].getWithDefault<std::string>("") ==
          "magnitude_response");
    CHECK(parsed["window"].getWithDefault<std::string>("") == "rectangular");
    CHECK(parsed["fft_length"].getWithDefault<int64_t>(0) == 8192);
    CHECK(parsed["sample_rate"].getWithDefault<double>(0.0) == kSampleRate);
    REQUIRE(parsed["curve"].isArray());
    CHECK(parsed["curve"].size() == 8192 / 2 + 1);
    REQUIRE(parsed["checkpoints"].isArray());
    CHECK(parsed["checkpoints"].size() == 2);

    // THD artifact carries its own determinism fields + harmonic breakdown.
    constexpr int kFft = 8192;
    const double tone = coherent_tone(170, kFft, kSampleRate);
    ThdOptions topts;
    topts.fft_length = kFft;
    auto thd = measure_thd(RenderScenario(create_hard_clip)
                               .name("doctor.thd-artifact")
                               .sample_rate(kSampleRate)
                               .block_size(256),
                           tone, topts);
    const auto thd_path = write_thd_artifact(thd, "doctor.thd-artifact");
    REQUIRE(std::filesystem::exists(thd_path));
    const auto thd_parsed = choc::json::parse(thd_to_json(thd, "doctor.thd"));
    CHECK(thd_parsed["schema_version"].getWithDefault<int64_t>(-1) ==
          kDoctorCurveSchemaVersion);
    CHECK(thd_parsed["analyzer"].getWithDefault<std::string>("") == "thd");
    CHECK(thd_parsed["window"].getWithDefault<std::string>("") == "rectangular");
    CHECK(thd_parsed["coherent"].getWithDefault<bool>(false));
    REQUIRE(thd_parsed["harmonics"].isArray());
    CHECK(thd_parsed["harmonics"].size() >= 2);
}

TEST_CASE("Doctor: group delay reports a stopband as undefined",
          "[audio][doctor][group-delay]") {
    // The honesty contract. Phase is meaningless where the magnitude is at the
    // noise floor: atan2 of numerical dust still returns a plausible-looking
    // angle, and differentiating it yields a confident-looking group delay
    // that measures nothing. The analyzer must refuse to report it.
    //
    // A 200 Hz lowpass leaves 20 kHz ~80 dB down — far below the default
    // -60 dB gate — while its passband is untouched. Determinism contract: as
    // the other group-delay cases. Tolerance class: exact (the gate is a
    // boolean; NaN-ness is exact).
    auto scenario = lowpass_scenario("doctor.group-delay-stopband", 200.0f);
    GroupDelayOptions opts;
    opts.fft_length = 16384;
    const double checkpoints[] = {50.0, 20000.0};
    const auto curve = measure_group_delay(scenario, checkpoints, opts);

    INFO("passband 50 Hz: " << curve.magnitude_db_at(50.0) << " dB, defined="
                            << curve.defined_at(50.0) << "; stopband 20 kHz: "
                            << curve.magnitude_db_at(20000.0) << " dB, defined="
                            << curve.defined_at(20000.0) << "; floor "
                            << curve.magnitude_floor_db << " dB");

    // The passband carries signal, so its group delay is a real measurement.
    REQUIRE(curve.defined_at(50.0));
    CHECK(std::isfinite(curve.group_delay_samples_at(50.0)));

    // The deep stopband does not, and must be reported as undefined rather
    // than handed a number read out of the noise floor.
    CHECK(curve.magnitude_db_at(20000.0) < curve.magnitude_floor_db);
    CHECK_FALSE(curve.defined_at(20000.0));

    // Undefined means NaN, not a plausible zero: a caller who ignores the gate
    // gets a value that fails every comparison rather than a quiet lie.
    CHECK(std::isnan(curve.group_delay_samples_at(20000.0)));
    CHECK(std::isnan(curve.group_delay_seconds_at(20000.0)));
    CHECK(std::isnan(curve.phase_radians_at(20000.0)));

    // The gate is a magnitude decision, so magnitude stays readable either way
    // — it is the evidence for the verdict.
    CHECK(std::isfinite(curve.magnitude_db_at(20000.0)));

    // Every undefined bin in the curve obeys the same contract, and the gate
    // is driven by the stated floor rather than an ad-hoc rule.
    for (const auto& p : curve.full) {
        if (p.defined) {
            CHECK(p.magnitude_db >= curve.magnitude_floor_db);
        } else {
            CHECK(p.magnitude_db < curve.magnitude_floor_db);
            CHECK(std::isnan(p.group_delay_samples));
        }
    }
}

TEST_CASE("Doctor: group delay reports a silent output as undefined",
          "[audio][doctor][group-delay]") {
    // The degenerate case the relative magnitude gate cannot see. A silent
    // output has no peak to be relative TO — the peak collapses onto the
    // numerical floor, every bin reads 0 dB against it, and the curve would
    // claim a flat, fully-defined, zero-delay response for a processor that
    // emitted nothing. Nothing was measured here, so nothing may be reported.
    // Tolerance class: exact (the gate is a boolean).
    constexpr int kFft = 1024;
    auto impulse = make_impulse(/*channels=*/1, kFft, 1.0f, /*position=*/0);
    pulp::audio::Buffer<float> silent(1, kFft); // all zeros — emits nothing

    GroupDelayOptions opts;
    opts.fft_length = kFft;
    const double checkpoints[] = {1000.0};
    const auto curve =
        measure_group_delay(std::as_const(impulse).view(),
                            std::as_const(silent).view(), kSampleRate,
                            checkpoints, opts);

    // Not one bin may claim a measurement, and no accessor may hand back a
    // number — a silent processor is not a zero-latency processor.
    int defined_bins = 0;
    for (const auto& p : curve.full)
        if (p.defined)
            ++defined_bins;
    INFO("defined bins in a silent-output curve: " << defined_bins << " of "
                                                   << curve.full.size());
    CHECK(defined_bins == 0);
    CHECK_FALSE(curve.defined_at(1000.0));
    CHECK(std::isnan(curve.group_delay_samples_at(1000.0)));
    CHECK(std::isnan(curve.phase_radians_at(1000.0)));
}

TEST_CASE("Doctor: group-delay artifact round-trips",
          "[audio][doctor][group-delay]") {
    // Asserts the artifact schema, not DSP. A stopband point must omit the
    // delay fields entirely rather than serialize a placeholder — JSON has no
    // NaN, and a reader must never mistake a gated bin for a measurement.
    // Tolerance class: exact (field presence).
    auto scenario = lowpass_scenario("doctor.group-delay-artifact", 200.0f);
    GroupDelayOptions opts;
    opts.fft_length = 8192;
    const double checkpoints[] = {50.0, 20000.0};
    const auto curve = measure_group_delay(scenario, checkpoints, opts);

    const auto path = write_phase_artifact(curve, "doctor.group-delay-artifact");
    REQUIRE(std::filesystem::exists(path));

    const auto parsed =
        choc::json::parse(phase_curve_to_json(curve, "doctor.group-delay"));
    REQUIRE(parsed.isObject());
    CHECK(parsed["schema_version"].getWithDefault<int64_t>(-1) ==
          kDoctorCurveSchemaVersion);
    CHECK(parsed["analyzer"].getWithDefault<std::string>("") == "group_delay");
    CHECK(parsed["window"].getWithDefault<std::string>("") == "rectangular");
    CHECK(parsed["fft_length"].getWithDefault<int64_t>(0) == 8192);
    CHECK(parsed["magnitude_floor_db"].getWithDefault<double>(0.0) == -60.0);
    REQUIRE(parsed["curve"].isArray());
    CHECK(parsed["curve"].size() == 8192 / 2 + 1);
    REQUIRE(parsed["checkpoints"].isArray());
    REQUIRE(parsed["checkpoints"].size() == 2);

    // The passband checkpoint carries a measurement in both units.
    const auto passband = parsed["checkpoints"][0];
    REQUIRE(passband["defined"].getWithDefault<bool>(false));
    CHECK(passband.hasObjectMember("group_delay_samples"));
    CHECK(passband.hasObjectMember("group_delay_seconds"));
    CHECK(passband.hasObjectMember("phase_rad"));

    // The stopband checkpoint carries the verdict and its evidence, and no
    // delay number at all.
    const auto stopband = parsed["checkpoints"][1];
    REQUIRE_FALSE(stopband["defined"].getWithDefault<bool>(true));
    CHECK(stopband.hasObjectMember("magnitude_db"));
    CHECK_FALSE(stopband.hasObjectMember("group_delay_samples"));
    CHECK_FALSE(stopband.hasObjectMember("group_delay_seconds"));
    CHECK_FALSE(stopband.hasObjectMember("phase_rad"));
}

TEST_CASE("Doctor: response guards an empty impulse reference window",
          "[audio][doctor]") {
    // The default reference is an impulse at frame 0. At offset 0 the input
    // window contains it and the transfer curve is well-defined; at offset > 0
    // the window misses the impulse, in_mag ≈ 0, and the bin-by-bin division
    // would produce garbage. The buffer-level analyzer must reject that rather
    // than silently return a meaningless curve.
    constexpr int kFft = 1024;
    constexpr int kRenderLen = kFft * 2; // room for an offset window.
    auto impulse = make_impulse(/*channels=*/2, kRenderLen, 1.0f, /*position=*/0);
    const double checkpoints[] = {1000.0};

    ResponseOptions opts;
    opts.fft_length = kFft;

    // offset 0: the impulse is in the window, so the curve is computed.
    opts.analysis_offset = 0;
    REQUIRE_NOTHROW(response_relative_to_input(
        std::as_const(impulse).view(), std::as_const(impulse).view(),
        kSampleRate, checkpoints, opts));

    // offset > 0: the impulse has fallen out of the reference window → guard.
    opts.analysis_offset = kFft;
    REQUIRE_THROWS_AS(
        response_relative_to_input(std::as_const(impulse).view(),
                                   std::as_const(impulse).view(), kSampleRate,
                                   checkpoints, opts),
        std::invalid_argument);
}

TEST_CASE("Doctor: window coefficients are periodic and coherent-gain normalized",
          "[audio][doctor][window]") {
    // Tolerance class: EXACT for the two pre-existing windows — this is the
    // backward-compatibility pin. Everything else is numeric.
    constexpr int kN = 1024;

    SECTION("rectangular is exactly unity") {
        const auto w = window_coefficients(Window::rectangular, kN);
        REQUIRE(w.size() == static_cast<std::size_t>(kN));
        for (double c : w)
            REQUIRE(c == 1.0); // exact: coherent gain is exactly 1.0
    }

    SECTION("hann is bit-for-bit the periodic expression it has always been") {
        // The analyzer used to compute `0.5 - 0.5*cos(2π n / N)` inline. It now
        // delegates to core/signal and divides by the coherent gain (0.5). This
        // asserts BOTH halves of the compatibility claim at once: the delegated
        // coefficients are the same periodic window, and the normalizer is
        // exactly 2.0 — an exact power of two, so it scales the FFT input by a
        // factor that cancels bit-for-bit in every ratio the analyzers report.
        const auto w = window_coefficients(Window::hann, kN);
        REQUIRE(w.size() == static_cast<std::size_t>(kN));
        for (int n = 0; n < kN; ++n) {
            const double legacy =
                0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / kN);
            REQUIRE(w[static_cast<std::size_t>(n)] * 0.5 == legacy);
        }
        // Periodic, not symmetric: a symmetric Hann would put the second zero
        // at n = N-1. The periodic one is non-zero there.
        CHECK(w.front() == 0.0);
        CHECK(w.back() > 0.0);
    }

    SECTION("every window has a DC gain of exactly 1 after normalization") {
        // This is the point of the normalization: swapping windows must not
        // move the reference level a magnitude is quoted against.
        for (Window win : {Window::rectangular, Window::hann, Window::hamming,
                           Window::blackman, Window::flat_top, Window::kaiser}) {
            const auto w = window_coefficients(win, kN);
            const double mean = std::accumulate(w.begin(), w.end(), 0.0) / kN;
            INFO("window " << window_name(win) << " mean " << mean);
            CHECK(std::abs(mean - 1.0) < 1e-9);
        }
    }

    SECTION("coherent gain matches each window's a0") {
        // rectangular and hann are EXACT: their divisor is the analytic
        // constant, and being an exact power of two is what preserves their
        // pre-existing bit-for-bit results.
        CHECK(window_coherent_gain(Window::rectangular, kN) == 1.0);
        CHECK(window_coherent_gain(Window::hann, kN) == 0.5);

        // The rest measure the true mean of their coefficients rather than
        // trusting a textbook constant. They land NEAR the a0 figure but not
        // ON it, because core/signal spells its coefficients as float literals
        // — at double, its hamming a0 is double(0.54f) = 0.54000002…, ~2e-8
        // off the textbook 0.54. Measuring is what keeps the DC gain exactly 1
        // (asserted above) despite that. Tolerance 1e-6: comfortably tighter
        // than any real drift, looser than the float-literal wart.
        CHECK(std::abs(window_coherent_gain(Window::hamming, kN) - 0.54) < 1e-6);
        CHECK(std::abs(window_coherent_gain(Window::blackman, kN) - 0.42) < 1e-6);
        CHECK(std::abs(window_coherent_gain(Window::flat_top, kN) - 0.21557895) <
              1e-6);

        // Kaiser has no closed form at all; it is measured, and sits strictly
        // between "no taper" and "fully tapered away".
        const double kg = window_coherent_gain(Window::kaiser, kN);
        INFO("kaiser coherent gain " << kg);
        CHECK(kg > 0.0);
        CHECK(kg < 1.0);
    }

    SECTION("names round-trip for artifacts") {
        CHECK(window_name(Window::rectangular) == "rectangular");
        CHECK(window_name(Window::hann) == "hann");
        CHECK(window_name(Window::hamming) == "hamming");
        CHECK(window_name(Window::blackman) == "blackman");
        CHECK(window_name(Window::flat_top) == "flat_top");
        CHECK(window_name(Window::kaiser) == "kaiser");
    }

    SECTION("invalid arguments are rejected") {
        REQUIRE_THROWS_AS(window_coefficients(Window::hann, 0),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(window_coefficients(Window::kaiser, kN, 0.0),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(window_coherent_gain(Window::kaiser, kN, -1.0),
                          std::invalid_argument);
    }
}

TEST_CASE("Doctor: each window's first side lobe matches its design",
          "[audio][doctor][window]") {
    // Determinism contract: single 0.5-amplitude tone at bin 1000.5 (a
    // deliberate HALF-BIN offset — a bin-centered tone samples every cosine-sum
    // kernel at its nulls and leaks nothing, which would measure the float
    // noise floor instead of the window), 48 kHz, 16384-point FFT,
    // peak-normalized self-spectrum. Tolerance class: numeric — each band is
    // ±3 dB around the measured value, which sits within ~2 dB of the
    // textbook design figure for every window.
    //
    // This is the calibration behind the `Window` doc table: a window's first
    // side lobe IS the floor below which it cannot see anything near a loud
    // tone.
    const double loud_hz = 1000.5 * kLeakBin;

    struct Case {
        Window window;
        int main_lobe_radius; ///< Bins to skip either side of the tone.
        double min_db;        ///< Expected side lobe sits in [min_db, max_db].
        double max_db;
        const char* design;   ///< Textbook figure, for the failure message.
    };
    // Radii are the window's own main-lobe half-width: a smaller one would
    // measure the main lobe's shoulder rather than the first side lobe.
    const Case cases[] = {
        {Window::rectangular, 2, -16.0, -12.0, "-13.3 dB"},
        {Window::hann,        2, -33.0, -29.0, "-31.5 dB"},
        {Window::hamming,     3, -43.0, -39.0, "-43 dB"},
        {Window::blackman,    3, -59.0, -55.0, "-58.1 dB"},
        {Window::flat_top,    5, -96.0, -90.0, "-93 dB"},
    };

    for (const auto& c : cases) {
        const auto sig =
            make_two_tone(kLeakFft, kSampleRate, loud_hz, 0.5, 0.0, 0.0);
        ResponseOptions opts;
        opts.fft_length = kLeakFft;
        opts.window = c.window;
        const auto curve = magnitude_spectrum_curve(std::as_const(sig).view(),
                                                    kSampleRate, {}, opts);
        const double lobe = worst_side_lobe_db(curve, 1000.5, c.main_lobe_radius);
        INFO("window " << window_name(c.window) << ": first side lobe " << lobe
                       << " dB (design " << c.design << "), expected ["
                       << c.min_db << ", " << c.max_db << "]");
        CHECK(lobe >= c.min_db);
        CHECK(lobe <= c.max_db);
    }

    // Kaiser is the outlier worth stating separately: at the default β its
    // side lobes are below −110 dB, which is what makes the −100 dBc gate
    // below possible at all. (Asserted as a bound, not a band — a deeper
    // floor is never a regression.)
    const auto sig = make_two_tone(kLeakFft, kSampleRate, loud_hz, 0.5, 0.0, 0.0);
    ResponseOptions kopts;
    kopts.fft_length = kLeakFft;
    kopts.window = Window::kaiser;
    const auto kcurve =
        magnitude_spectrum_curve(std::as_const(sig).view(), kSampleRate, {}, kopts);
    const double klobe = worst_side_lobe_db(kcurve, 1000.5, /*main_lobe_radius=*/6);
    INFO("kaiser (beta=" << kDefaultKaiserBeta << ") first side lobe " << klobe
                         << " dB");
    CHECK(klobe <= -110.0);
}

TEST_CASE("Doctor: only a deep-side-lobe window can resolve a -100 dBc component",
          "[audio][doctor][window]") {
    // ── THIS TEST IS THE ENTIRE REASON THE WINDOW ENUM WAS WIDENED. ─────────
    //
    // The suite's headline oscillator gates claim alias components sit below
    // −100 dB relative to the fundamental. A claim like that is only meaningful
    // if the ANALYZER can see −100 dBc in the first place. Through a Hann
    // window it cannot: Hann's −31 dB first side lobe leaks a skirt off the
    // 0 dB fundamental that is louder than the thing being measured, so the
    // number reported is Hann's leakage, not the oscillator's aliasing. That is
    // a gate which passes because the measurement cannot see the failure.
    //
    // Fixture: a 0 dB fundamental at a HALF-BIN offset (bin 1000.5 — worst-case
    // leakage; a bin-coherent tone would leak nothing and prove nothing) plus a
    // −100 dBc tone 16 bins (≈ 47 Hz) away. 48 kHz, 16384-point FFT.
    //
    // The control is what makes this airtight: every window is measured TWICE,
    // once with the quiet tone present and once with it REMOVED. If both
    // readings agree, the analyzer never saw the tone — it was reporting its
    // own leakage both times.
    //
    // Tolerance class: numeric, with wide margins. Measured values on this
    // fixture (macOS/vDSP, and the reason for each threshold):
    //
    //   window       with tone   control    verdict
    //   rectangular    -30.4      -30.4     blind (69 dB high)
    //   hamming        -49.4      -49.4     blind (51 dB high)
    //   hann           -82.3      -81.5     blind (18 dB high)
    //   blackman       -91.4      -89.4     blind (9 dB high)
    //   flat_top       -96.2      -93.2     blind (4 dB high)
    //   kaiser        -100.1     -135.1     RESOLVED (0.1 dB error)
    constexpr double kTruthDb = -100.0;
    const double loud_hz = 1000.5 * kLeakBin;
    const double quiet_hz = (1000.5 + 16.0) * kLeakBin;
    constexpr double kQuietAmp = 1e-5; // 20*log10(1e-5/1.0) = -100 dB exactly.

    SECTION("hann CANNOT resolve it — it reports its own leakage") {
        const double measured =
            reading_at(Window::hann, loud_hz, quiet_hz, kQuietAmp);
        const double control = reading_at(Window::hann, loud_hz, quiet_hz, 0.0);
        INFO("hann: measured " << measured << " dB, leakage-only control "
                               << control << " dB, truth " << kTruthDb << " dB");

        // Reads far too loud: the skirt, not the tone.
        CHECK(measured > kTruthDb + 10.0);
        // And the proof it is the skirt: removing the tone entirely barely
        // moves the reading.
        CHECK(std::abs(measured - control) < 3.0);
    }

    SECTION("blackman and flat_top CANNOT resolve it either at this offset") {
        // Worth pinning explicitly, because both are the windows most likely to
        // be reached for as "the deep ones", and neither is deep enough here.
        // The decisive fact is the control: a window can only resolve a -100 dB
        // component if its own leakage floor is BELOW -100 dB. Measured at this
        // offset: blackman -89 dB, flat_top -93 dB. Both floors sit ABOVE the
        // component, so whatever they report is their own skirt.
        for (Window w : {Window::blackman, Window::flat_top}) {
            const double measured = reading_at(w, loud_hz, quiet_hz, kQuietAmp);
            const double control = reading_at(w, loud_hz, quiet_hz, 0.0);
            INFO(window_name(w) << ": measured " << measured
                                << " dB, leakage-only control " << control
                                << " dB, truth " << kTruthDb << " dB");
            // The leakage floor is not below the component at all → blind.
            CHECK(control > kTruthDb);
            // And so the reading comes back high, as leakage does.
            CHECK(measured > kTruthDb + 1.0);
        }
    }

    SECTION("flat_top's floor does not improve with distance — hann's does") {
        // This substantiates the "falloff" column of the Window doc table, and
        // it is the counter-intuitive part: hann's FIRST side lobe (-31 dB) is
        // 62 dB worse than flat_top's (-93 dB), yet hann falls off at
        // -18 dB/octave while flat_top is essentially flat. Far enough from the
        // tone, hann's floor therefore overtakes flat_top's outright.
        //
        // Measured 64 bins out: hann -117 dB, flat_top -101 dB. So flat_top is
        // the WRONG tool for a -100 dBc gate at any offset — near the tone its
        // main lobe swallows the component, and far from it hann is better.
        // flat_top's virtue is amplitude accuracy, not floor depth.
        const double far_hz = (1000.5 + 64.0) * kLeakBin;
        const double hann_floor = reading_at(Window::hann, loud_hz, far_hz, 0.0);
        const double flat_floor = reading_at(Window::flat_top, loud_hz, far_hz, 0.0);
        INFO("64 bins out: hann floor " << hann_floor << " dB, flat_top floor "
                                        << flat_floor << " dB");
        CHECK(hann_floor < flat_floor);
        // flat_top is still hovering around the -100 dB component even here.
        CHECK(flat_floor > kTruthDb - 10.0);
    }

    SECTION("kaiser DOES resolve it, with the tone well clear of the floor") {
        const double measured =
            reading_at(Window::kaiser, loud_hz, quiet_hz, kQuietAmp);
        const double control = reading_at(Window::kaiser, loud_hz, quiet_hz, 0.0);
        INFO("kaiser (beta=" << kDefaultKaiserBeta << "): measured " << measured
                             << " dB, leakage-only control " << control
                             << " dB, truth " << kTruthDb << " dB");

        // Reads the true level to within 1 dB (measured error: ~0.1 dB).
        CHECK(std::abs(measured - kTruthDb) < 1.0);
        // And the reading is trustworthy because the leakage floor is far below
        // the component rather than on top of it (measured: ~-135 dB, i.e. ~35
        // dB of headroom). -110 dB is the harness's stated detection-floor bar;
        // every gate threshold must sit above it.
        CHECK(control <= -110.0);
    }

    SECTION("the default kaiser beta is what buys the headroom") {
        // Guards the constant: a low beta silently reintroduces the failure
        // this whole change exists to remove. beta=6 is ~-59 dB side lobes.
        const double weak = reading_at(Window::kaiser, loud_hz, quiet_hz,
                                       kQuietAmp, /*kaiser_beta=*/6.0);
        INFO("kaiser beta=6 reads " << weak << " dB for a -100 dB component");
        CHECK(weak > kTruthDb + 2.0); // blind, as expected for a shallow beta
    }
}

TEST_CASE("Doctor: THD non-coherent window is selectable", "[audio][doctor][window]") {
    // A coherent tone always uses rectangular (it leaks nothing, so no taper
    // can improve on it) regardless of what the caller asks for; only the
    // non-coherent path honors `non_coherent_window`. The default stays `hann`,
    // which is what the analyzer has always used.
    constexpr int kFft = 16384;

    const double coherent = coherent_tone(341, kFft, kSampleRate);
    const double non_coherent = coherent + kSampleRate / kFft * 0.5; // half a bin

    ThdOptions opts;
    opts.fft_length = kFft;

    auto coh_sig = make_two_tone(kFft, kSampleRate, coherent, 0.5, 0.0, 0.0);
    auto non_sig = make_two_tone(kFft, kSampleRate, non_coherent, 0.5, 0.0, 0.0);

    SECTION("default non-coherent window is hann") {
        const auto t =
            measure_thd(std::as_const(non_sig).view(), non_coherent, kSampleRate, opts);
        CHECK_FALSE(t.coherent);
        CHECK(t.window == Window::hann);
    }

    SECTION("a coherent tone stays rectangular whatever the caller asks for") {
        opts.non_coherent_window = Window::kaiser;
        const auto t =
            measure_thd(std::as_const(coh_sig).view(), coherent, kSampleRate, opts);
        CHECK(t.coherent);
        CHECK(t.window == Window::rectangular);
    }

    SECTION("a non-coherent tone honors the requested window") {
        opts.non_coherent_window = Window::blackman;
        const auto t =
            measure_thd(std::as_const(non_sig).view(), non_coherent, kSampleRate, opts);
        CHECK_FALSE(t.coherent);
        CHECK(t.window == Window::blackman);
        // The selected window is what the artifact reports.
        CHECK(window_name(t.window) == "blackman");
    }

    SECTION("a deeper window does NOT rescue non-coherent THD+N") {
        // A trap worth pinning, because the intuition points the wrong way.
        //
        // `measure_thd` treats a SINGLE bin as the fundamental. A non-coherent
        // tone's energy is spread across its whole main lobe, so every main-lobe
        // bin except the peak is counted as "not the fundamental" — i.e. as
        // noise. THD+N is therefore dominated by MAIN-LOBE WIDTH here, not by
        // side lobes, and a deeper window (which is wider) measures MORE
        // apparent noise on the identical clean signal, not less.
        //
        // This is why `coherent` exists and why a non-coherent reading is
        // advisory only: the fix is a bin-coherent tone, never a better window.
        // Anyone tempted to "upgrade" the non-coherent default to a deep window
        // for accuracy should fail here first.
        opts.non_coherent_window = Window::hann;
        const auto with_hann =
            measure_thd(std::as_const(non_sig).view(), non_coherent, kSampleRate, opts);
        opts.non_coherent_window = Window::kaiser;
        const auto with_kaiser =
            measure_thd(std::as_const(non_sig).view(), non_coherent, kSampleRate, opts);
        INFO("THD+N of the SAME clean tone: hann "
             << with_hann.thd_plus_n_db() << " dB, kaiser "
             << with_kaiser.thd_plus_n_db() << " dB");

        // Both are unusable on a clean sine — a real THD+N would be far below
        // 1%, and these sit near unity (0 dB) purely from main-lobe spreading.
        CHECK(with_hann.thd_plus_n > 0.5);
        CHECK(with_kaiser.thd_plus_n > 0.5);
        // And the deeper/wider window is the WORSE of the two, not the better.
        CHECK(with_kaiser.thd_plus_n_db() > with_hann.thd_plus_n_db());
    }
}

TEST_CASE("Doctor: tone projection measures a residual no window could see",
          "[audio][doctor][projection]") {
    // The promoted single-tone primitive. Its whole value over the FFT
    // analyzers is that it has no leakage skirt, so it resolves a residual far
    // below any window's floor on a tone that is NOT bin-coherent and over a
    // segment that is NOT a power of two — two things the FFT path cannot do.
    //
    // Tolerance class: numeric, wide margins.
    constexpr int kLen = 10000;             // deliberately not a power of two
    const double f0 = 997.3;                // deliberately not bin-coherent
    const double cycles = f0 / kSampleRate;

    auto samples = [&](double impurity_amp, double impurity_hz) {
        std::vector<double> v(kLen);
        for (int i = 0; i < kLen; ++i) {
            const double t = static_cast<double>(i) / kSampleRate;
            v[static_cast<std::size_t>(i)] =
                0.7 * std::sin(2.0 * std::numbers::pi * f0 * t + 0.4) +
                impurity_amp *
                    std::sin(2.0 * std::numbers::pi * impurity_hz * t + 1.1);
        }
        return v;
    };

    SECTION("a pure tone's residual sits far below any window's floor") {
        const auto v = samples(0.0, 0.0);
        const double residual = tone_residual_db(v, cycles);
        INFO("pure-tone residual " << residual << " dB");
        // The measurement bottoms out at the library's own kSilenceFloorDb
        // clamp: subtracting the tone leaves nothing but double-precision
        // arithmetic, so the true residual is BELOW the smallest number this
        // analyzer reports. Contrast the window path, which on the identical
        // job cannot get past its leakage skirt — the deepest window in this
        // file measures -135 dB and hann only -82 dB.
        CHECK(residual == kSilenceFloorDb);
    }

    SECTION("amplitude is recovered regardless of the tone's phase") {
        const auto fit = fit_tone(samples(0.0, 0.0), cycles);
        INFO("fitted amplitude " << fit.amplitude);
        CHECK(std::abs(fit.amplitude - 0.7) < 1e-9);
        // Gain readout is the same fit, expressed against a reference.
        CHECK(std::abs(tone_gain_db(samples(0.0, 0.0), cycles, 0.7)) < 1e-6);
    }

    SECTION("the residual tracks a known impurity's level") {
        // Ground truth the helper cannot accidentally pass: -60 dB of impurity
        // relative to a 0.7 tone is a residual of -60 dB, and -100 dB is -100.
        // A stub returning a constant fails on the second row.
        for (const double truth_db : {-60.0, -100.0}) {
            const double amp = 0.7 * std::pow(10.0, truth_db / 20.0);
            const double residual = tone_residual_db(samples(amp, 3011.0), cycles);
            INFO("injected " << truth_db << " dBc impurity, residual measured "
                             << residual << " dB");
            CHECK(std::abs(residual - truth_db) < 0.1);
        }
    }

    SECTION("degenerate frequencies are rejected, not silently wrong") {
        // At DC and at exactly Nyquist the sin quadrature vanishes, so
        // amplitude and phase cannot be separated. Returning a number there
        // would be the silent-failure mode this analyzer exists to avoid.
        const auto v = samples(0.0, 0.0);
        REQUIRE_THROWS_AS(fit_tone(v, 0.0), std::invalid_argument);
        REQUIRE_THROWS_AS(fit_tone(v, 0.5), std::invalid_argument);
        REQUIRE_THROWS_AS(fit_tone(v, 1.0), std::invalid_argument); // folds to DC
        REQUIRE_THROWS_AS(tone_gain_db(v, cycles, 0.0), std::invalid_argument);
    }
}

TEST_CASE("Doctor: fold-back maps a component to where sampling puts it",
          "[audio][doctor][alias]") {
    // Pure arithmetic; tolerance class: exact-ish (1e-9).
    constexpr double fs = 48000.0;

    SECTION("below Nyquist a component stays where it is") {
        CHECK(fold_frequency(1000.0, fs) == 1000.0);
        CHECK(fold_frequency(23999.0, fs) == 23999.0);
    }

    SECTION("between Nyquist and the sample rate it mirrors down") {
        CHECK(std::abs(fold_frequency(24001.0, fs) - 23999.0) < 1e-9);
        CHECK(std::abs(fold_frequency(30000.0, fs) - 18000.0) < 1e-9);
        CHECK(std::abs(fold_frequency(47000.0, fs) - 1000.0) < 1e-9);
    }

    SECTION("above the sample rate it wraps, then mirrors — repeatedly") {
        // The case a single mirror-about-Nyquist gets wrong. Each of these
        // needs the wrap AND the mirror, and the last three are past 1.5·fs,
        // i.e. they have folded more than once.
        CHECK(std::abs(fold_frequency(48000.0, fs) - 0.0) < 1e-9);   // 1.00·fs
        CHECK(std::abs(fold_frequency(60000.0, fs) - 12000.0) < 1e-9); // 1.25·fs
        CHECK(std::abs(fold_frequency(72000.0, fs) - 24000.0) < 1e-9); // 1.50·fs
        CHECK(std::abs(fold_frequency(80000.0, fs) - 16000.0) < 1e-9); // 1.67·fs
        CHECK(std::abs(fold_frequency(82000.0, fs) - 14000.0) < 1e-9); // 1.71·fs
        CHECK(std::abs(fold_frequency(100000.0, fs) - 4000.0) < 1e-9); // 2.08·fs
        CHECK(std::abs(fold_frequency(250000.0, fs) - 10000.0) < 1e-9); // 5.21·fs
    }

    SECTION("negative frequencies fold like their magnitude") {
        // A real signal's spectrum is symmetric; sign carries no information.
        CHECK(fold_frequency(-30000.0, fs) == fold_frequency(30000.0, fs));
    }
}

TEST_CASE("Doctor: alias analyzer classifies a known injected alias",
          "[audio][doctor][alias]") {
    // ── GROUND-TRUTH TEST. The analyzer is proven against a fixture whose
    // answer is known by construction and which it cannot pass by accident. ──
    //
    // Fixture: a bandlimited series at 4100 Hz (fundamental 1.0 plus harmonics
    // 2..5, all genuinely below Nyquist), PLUS one tone injected at exactly the
    // fold-back site of harmonic 8 (8 · 4100 = 32800 Hz → 48000 − 32800 =
    // 15200 Hz) at a known level. 48 kHz, 16384-sample fit.
    //
    // What makes it un-fakeable: the analyzer must get the CLASS (alias, not
    // harmonic), the INDEX (8), the FREQUENCY (15200), and the LEVEL right —
    // and the level is swept, so an implementation that reports a constant, or
    // that reports the fundamental's leakage, fails.
    AliasOptions opts;
    opts.num_harmonics = 20;
    opts.analysis_length = kAliasFft;

    SECTION("class, index, landing frequency and level are all recovered") {
        for (const double truth_db : {-60.0, -80.0, -100.0}) {
            auto tones = bandlimited_base();
            tones.push_back({15200.0, std::pow(10.0, truth_db / 20.0)});
            const auto sig = make_series(kAliasFft, kSampleRate, tones);
            const auto report =
                measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);

            INFO("injected " << truth_db << " dBc at 15200 Hz; worst in-band alias "
                             << report.worst_alias_db << " dB at "
                             << report.worst_alias_hz << " Hz (h="
                             << report.worst_alias_index << "), detection floor "
                             << report.detection_floor_db << " dB");

            const auto& c = component(report, 8);
            CHECK(c.component_class == ComponentClass::alias);
            CHECK(c.resolved);
            CHECK(std::abs(c.source_hz - 32800.0) < 1e-9);
            CHECK(std::abs(c.hz - 15200.0) < 1e-9);
            CHECK(std::abs(c.db_below_fundamental - truth_db) < 0.5);

            // And it is the worst one, because nothing else was injected.
            CHECK(report.worst_alias_index == 8);
            CHECK(std::abs(report.worst_alias_hz - 15200.0) < 1e-9);
            CHECK(std::abs(report.worst_alias_db - truth_db) < 0.5);
        }
    }

    SECTION("the legitimate harmonics are never called aliases") {
        auto tones = bandlimited_base();
        tones.push_back({15200.0, 1e-5});
        const auto sig = make_series(kAliasFft, kSampleRate, tones);
        const auto report =
            measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);

        // h=1..5 are below Nyquist and are content, however loud.
        for (int h = 1; h <= 5; ++h) {
            const auto& c = component(report, h);
            INFO("h=" << h << " at " << c.hz << " Hz, "
                      << c.db_below_fundamental << " dBc");
            CHECK(c.component_class == ComponentClass::harmonic);
            CHECK(std::abs(c.hz - kAliasF0 * h) < 1e-9);
            CHECK(c.in_band); // a harmonic is in-band at any frequency
        }
        // h=6 upward are above Nyquist and cannot exist where they belong.
        for (int h = 6; h <= 20; ++h)
            CHECK(component(report, h).component_class == ComponentClass::alias);

        // The fitted harmonic levels are the ones built into the fixture —
        // proof the joint fit is not just finding the fundamental.
        CHECK(std::abs(component(report, 2).db_below_fundamental -
                       20.0 * std::log10(0.50)) < 0.1);
        CHECK(std::abs(component(report, 5).db_below_fundamental -
                       20.0 * std::log10(0.20)) < 0.1);
        // Note h=5 lands at 20500 Hz, ABOVE the 20 kHz alias band. It is still
        // reported in-band: the qualifier gates aliases, not content.
        CHECK(component(report, 5).hz > opts.max_alias_frequency_hz);
    }

    SECTION("an alias folding more than once is attributed to the right h") {
        // Harmonic 20 sits at 82000 Hz = 1.71 · fs, so reaching its landing
        // site needs a wrap AND a mirror. It lands at 14000 Hz — in band, and
        // 500 Hz clear of every other site. An implementation that mirrored
        // about Nyquist without wrapping first would look for it at 34000 Hz
        // (above Nyquist, i.e. nowhere) and report no alias at all.
        auto tones = bandlimited_base();
        tones.push_back({14000.0, 1e-4}); // -80 dBc
        const auto sig = make_series(kAliasFft, kSampleRate, tones);
        const auto report =
            measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);

        const auto& c = component(report, 20);
        INFO("h=20: source " << c.source_hz << " Hz (" << c.source_hz / kSampleRate
                             << " · fs) → landed " << c.hz << " Hz at "
                             << c.db_below_fundamental << " dBc");
        CHECK(c.component_class == ComponentClass::alias);
        CHECK(std::abs(c.source_hz - 82000.0) < 1e-9);
        CHECK(c.source_hz > 1.5 * kSampleRate); // folds more than once
        CHECK(std::abs(c.hz - 14000.0) < 1e-9);
        CHECK(std::abs(c.db_below_fundamental - (-80.0)) < 0.5);
        CHECK(report.worst_alias_index == 20);
    }
}

TEST_CASE("Doctor: a clean bandlimited signal reports no alias",
          "[audio][doctor][alias]") {
    // The other half of the ground truth: the analyzer must not invent aliases
    // in content that has none. Same fixture as above with nothing injected —
    // every fold site (h = 6..20) is empty, so every one must read at the noise
    // floor rather than picking up the fundamental's skirt.
    //
    // This is also the suite's blocking acceptance bar: the measurement must
    // demonstrate a detection floor at or below −110 dB on a synthetic
    // fixture before any oscillator gate built on it is trusted — a gate
    // read through a floor above its own threshold passes because it cannot
    // see the failure.
    AliasOptions opts;
    opts.num_harmonics = 20;
    opts.analysis_length = kAliasFft;

    const auto base = bandlimited_base();
    const auto sig = make_series(kAliasFft, kSampleRate, base);
    const auto report =
        measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);

    INFO("clean series: worst in-band alias " << report.worst_alias_db
                                              << " dB, full band "
                                              << report.full_band_worst_alias_db
                                              << " dB, noise " << report.noise_db
                                              << " dB, detection floor "
                                              << report.detection_floor_db << " dB");

    // No alias anywhere — in band or out. The bound is deliberately tighter
    // than the -100 dB gate this analyzer exists to serve: an earlier revision
    // of this code fitted the tones but not the constant, which left a DC
    // pedestal in the residual and put these empty sites at -128 dB. That is
    // still under -120, so a loose bound here would have shipped a floor 30 dB
    // worse than advertised. Measured: about -171 dB (21 dB of margin).
    CHECK(report.worst_alias_db < -150.0);
    CHECK(report.full_band_worst_alias_db < -150.0);
    CHECK_FALSE(report.has_unresolved_in_band_alias);

    // Nothing unexplained either: on a fixture built entirely from tones the
    // fit knows about, the residual is the float32 buffer's own quantization
    // and nothing else. Measured ≈ -151 dB. (The DC-pedestal bug put this at
    // -69 dB — this assertion is the one that catches it outright.)
    CHECK(report.noise_db < -120.0);

    // The floor must be BELOW the -100 dB gate the suite intends to run
    // through it, with margin. A gate at -100 dB read through a -85 dB floor is
    // not a gate. Measured here ≈ -187 dB, limited only by that quantization.
    // (Asserting the derived floor is legitimate HERE and only here: this
    // fixture's residual is float quantization — genuinely white — and the
    // injected-alias test corroborates the floor by recovering known levels.
    // A gate on real oscillator output must never assert it; see the header.)
    CHECK(report.detection_floor_db <= -110.0);

    // The stated floor must not be a fiction: no site may read below it.
    CHECK(report.worst_alias_db >= report.detection_floor_db);

    // The reference every dB hangs off is the fixture's actual fundamental.
    CHECK(std::abs(report.fundamental_amplitude - 1.0) < 1e-6);
    CHECK(report.analysis_length == kAliasFft);
    CHECK(std::abs(report.nyquist_hz - kSampleRate / 2.0) < 1e-9);
}

TEST_CASE("Doctor: the band qualifier is what makes an alias gate passable",
          "[audio][doctor][alias]") {
    // ── The single most important behavior in this analyzer. ────────────────
    //
    // Fixture: TWO injected aliases at once —
    //   * h=6 → 24600 Hz folds to 23400 Hz, ABOVE the 20 kHz band, at -40 dBc.
    //     This is the physically unavoidable one: it is a component just above
    //     Nyquist folding to just below it, where no realizable kernel has
    //     attenuation. Inaudible, and no correct implementation can remove it.
    //   * h=7 → 28700 Hz folds to 19300 Hz, INSIDE the band, at -90 dBc.
    //     This is the one that matters: audible, and the thing an anti-aliased
    //     oscillator is supposed to suppress.
    //
    // A full-band gate reports -40 dB and fails everything forever. The
    // band-qualified gate reports -90 dB and measures the oscillator.
    auto tones = bandlimited_base();
    tones.push_back({23400.0, std::pow(10.0, -40.0 / 20.0)}); // out of band
    tones.push_back({19300.0, std::pow(10.0, -90.0 / 20.0)}); // in band
    const auto sig = make_series(kAliasFft, kSampleRate, tones);

    AliasOptions opts;
    opts.num_harmonics = 20;
    opts.analysis_length = kAliasFft;

    SECTION("the default 20 kHz band excludes the unavoidable alias") {
        const auto report =
            measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);
        INFO("worst in-band " << report.worst_alias_db << " dB at "
                              << report.worst_alias_hz << " Hz; full band "
                              << report.full_band_worst_alias_db << " dB at "
                              << report.full_band_worst_alias_hz << " Hz");

        CHECK(opts.max_alias_frequency_hz == 20000.0); // the documented default

        // The gate number: the in-band alias, not the loud out-of-band one.
        CHECK(report.worst_alias_index == 7);
        CHECK(std::abs(report.worst_alias_hz - 19300.0) < 1e-9);
        CHECK(std::abs(report.worst_alias_db - (-90.0)) < 0.5);

        // Both are measured; only the classification differs.
        CHECK(component(report, 6).in_band == false);
        CHECK(std::abs(component(report, 6).hz - 23400.0) < 1e-9);
        CHECK(std::abs(component(report, 6).db_below_fundamental - (-40.0)) < 0.5);
        CHECK(component(report, 7).in_band == true);

        // And the honest unqualified number is still reported — it is just not
        // the thing to gate on.
        CHECK(std::abs(report.full_band_worst_alias_db - (-40.0)) < 0.5);
        CHECK(std::abs(report.full_band_worst_alias_hz - 23400.0) < 1e-9);

        // The whole point, stated as an assertion: a -80 dB gate PASSES
        // band-qualified and FAILS full-band, on identical audio.
        CHECK(report.worst_alias_db < -80.0);
        CHECK(report.full_band_worst_alias_db > -80.0);
    }

    SECTION("widening the band pulls the out-of-band alias back in") {
        // Demonstrates the qualifier is doing the work, not a coincidence of
        // this fixture: move the boundary past 23400 Hz and the -40 dB alias
        // becomes the worst in-band one.
        opts.max_alias_frequency_hz = 23900.0;
        const auto report =
            measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);
        INFO("band 23900 Hz: worst in-band " << report.worst_alias_db << " dB at "
                                             << report.worst_alias_hz << " Hz");
        CHECK(report.worst_alias_index == 6);
        CHECK(std::abs(report.worst_alias_hz - 23400.0) < 1e-9);
        CHECK(std::abs(report.worst_alias_db - (-40.0)) < 0.5);
    }

    SECTION("narrowing the band excludes an alias that was in it") {
        // 19300 Hz drops out below an 18 kHz boundary, leaving nothing.
        opts.max_alias_frequency_hz = 18000.0;
        const auto report =
            measure_aliasing(std::as_const(sig).view(), kAliasF0, kSampleRate, opts);
        INFO("band 18000 Hz: worst in-band " << report.worst_alias_db << " dB at "
                                             << report.worst_alias_hz << " Hz");
        CHECK(component(report, 7).in_band == false);
        // Note `worst_alias_index` stays NON-zero: the remaining in-band fold
        // sites still exist and are still measured, they just have nothing at
        // them. "No alias" is a LEVEL, not an absence of candidates.
        CHECK(report.worst_alias_hz != 19300.0);
        CHECK(report.worst_alias_db < -140.0);
    }

    SECTION("a qualifier at or above Nyquist is rejected as the misuse it is") {
        // The header explains at length why a full-band gate cannot be passed.
        // Refusing to compute one is what keeps a caller from writing it by
        // accident.
        opts.max_alias_frequency_hz = kSampleRate / 2.0 + 1.0;
        REQUIRE_THROWS_AS(measure_aliasing(std::as_const(sig).view(), kAliasF0,
                                           kSampleRate, opts),
                          std::invalid_argument);
        opts.max_alias_frequency_hz = 0.0;
        REQUIRE_THROWS_AS(measure_aliasing(std::as_const(sig).view(), kAliasF0,
                                           kSampleRate, opts),
                          std::invalid_argument);
    }
}

TEST_CASE("Doctor: an unmeasurable alias is reported, not silently passed",
          "[audio][doctor][alias]") {
    // A gate that passes because the measurement cannot see the failure is the
    // worst outcome available, so the analyzer has to be honest about the
    // cases it cannot resolve.
    //
    // f0 = fs/4 = 12 kHz is the clean degenerate case:
    //   h=2 → 24000 Hz = EXACTLY Nyquist — no sin quadrature there, so
    //         amplitude and phase are inseparable.
    //   h=3 → 36000 Hz folds to 12000 Hz — exactly ON TOP of the fundamental.
    //         No method can split a component from a tone at the same
    //         frequency; the segment carries no information that separates them.
    //   h=4 → 48000 Hz folds to 0 Hz — indistinguishable from a DC offset.
    //
    // The right answer is "I cannot measure this", and h=3 is in band, so a
    // gate must treat the whole report as inconclusive rather than as a pass.
    const double f0 = kSampleRate / 4.0;
    const std::vector<ToneSpec> tones = {{f0, 1.0}};
    const auto sig = make_series(kAliasFft, kSampleRate, tones);

    AliasOptions opts;
    opts.num_harmonics = 4;
    opts.analysis_length = kAliasFft;
    const auto report =
        measure_aliasing(std::as_const(sig).view(), f0, kSampleRate, opts);

    INFO("f0 = fs/4: h2 resolved=" << component(report, 2).resolved
                                   << " h3 resolved=" << component(report, 3).resolved
                                   << " h4 resolved=" << component(report, 4).resolved
                                   << " unresolved-in-band="
                                   << report.has_unresolved_in_band_alias);

    // The fundamental itself is fine — it is the reference.
    CHECK(component(report, 1).resolved);
    CHECK(component(report, 1).component_class == ComponentClass::harmonic);

    // Nyquist, self-collision and DC: all three refused.
    CHECK_FALSE(component(report, 2).resolved); // exactly Nyquist
    CHECK_FALSE(component(report, 3).resolved); // folds onto the fundamental
    CHECK_FALSE(component(report, 4).resolved); // folds onto DC
    CHECK(std::abs(component(report, 3).hz - f0) < 1e-9);

    // h=3 lands at 12 kHz, inside the band → the report says so out loud.
    CHECK(component(report, 3).in_band);
    CHECK(report.has_unresolved_in_band_alias);

    // And an unresolved component contributes no dB — worst_alias_db must not
    // be read as "clean" while this flag is set.
    CHECK(report.worst_alias_index == 0);
}

TEST_CASE("Doctor: alias analyzer refuses a series with no alias site",
          "[audio][doctor][alias]") {
    // The fail-open this guard closes: when every modeled harmonic sits below
    // Nyquist, the series contains NO fold site, so all of the signal's actual
    // aliasing lands in `noise_db` and `worst_alias_db` stays at
    // kSilenceFloorDb — a "< -100 dB" gate then PASSES on a maximally aliased
    // naive saw. Measured before the guard: a naive 300 Hz saw under DEFAULT
    // options (num_harmonics = 64; 64 · 300 = 19200 < 24000) reported
    // worst_alias_db = -200 with has_unresolved_in_band_alias = false. The
    // analyzer must refuse to produce a report that reads as clean when it
    // modeled nowhere to look.
    const auto saw300 = make_saw(1, kAliasFft, 300.0, kSampleRate);
    REQUIRE_THROWS_AS(
        measure_aliasing(std::as_const(saw300).view(), 300.0, kSampleRate),
        std::invalid_argument);

    // The same trap at a mid-band f0 with num_harmonics set too low.
    const auto saw1103 = make_saw(1, kAliasFft, 1103.0, kSampleRate);
    AliasOptions low;
    low.num_harmonics = 16; // 16 · 1103 = 17648 < Nyquist: nowhere to look.
    low.analysis_length = kAliasFft;
    REQUIRE_THROWS_AS(measure_aliasing(std::as_const(saw1103).view(), 1103.0,
                                       kSampleRate, low),
                      std::invalid_argument);

    // The remedy the guard's message directs to: enough harmonics that fold
    // sites exist. The identical saw then reads LOUD aliasing — the positive
    // control proving the guard rejects blind reports, not aliased signals.
    AliasOptions enough;
    enough.num_harmonics = 64; // 64 · 1103 ≈ 1.47 · fs.
    enough.analysis_length = kAliasFft;
    const auto report = measure_aliasing(std::as_const(saw1103).view(), 1103.0,
                                         kSampleRate, enough);
    INFO("naive 1103 Hz saw: worst in-band alias " << report.worst_alias_db
         << " dBc at " << report.worst_alias_hz << " Hz (h="
         << report.worst_alias_index << ")");
    CHECK_FALSE(report.has_unresolved_in_band_alias);
    CHECK(report.worst_alias_index > 0);
    // A naive saw's worst audible alias sits tens of dB above any real gate
    // (measured: -28.3 dBc at 19322 Hz, from h = 26).
    CHECK(report.worst_alias_db > -40.0);
    CHECK(report.worst_alias_db < -15.0);
}

TEST_CASE("Doctor: alias analyzer rejects arguments it cannot honor",
          "[audio][doctor][alias]") {
    const auto sig = make_series(1024, kSampleRate, bandlimited_base());
    const auto view = std::as_const(sig).view();

    CHECK_THROWS_AS(measure_aliasing(view, 0.0, kSampleRate), std::invalid_argument);
    CHECK_THROWS_AS(measure_aliasing(view, kAliasF0, 0.0), std::invalid_argument);
    // A "fundamental" at or above Nyquist is not a fundamental this signal has.
    CHECK_THROWS_AS(measure_aliasing(view, kSampleRate / 2.0, kSampleRate),
                    std::invalid_argument);

    AliasOptions opts;
    opts.num_harmonics = 0;
    CHECK_THROWS_AS(measure_aliasing(view, kAliasF0, kSampleRate, opts),
                    std::invalid_argument);

    opts = {};
    opts.analysis_offset = 4096; // past the end of a 1024-sample buffer
    CHECK_THROWS_AS(measure_aliasing(view, kAliasF0, kSampleRate, opts),
                    std::invalid_argument);
}

TEST_CASE("Doctor: THD refuses inputs it would silently misread",
          "[audio][doctor]") {
    // THD's fail-open modes, each measured before its guard existed. All
    // three returned a NUMBER rather than an error, and two of the numbers
    // would pass a gate.
    constexpr int kFft = 16384;
    const double tone = coherent_tone(341, kFft, kSampleRate);
    ThdOptions topts;
    topts.fft_length = kFft;

    SECTION("silence throws instead of reading THD = 0") {
        // An all-zero buffer measured thd = 0 (-200 dB): a DEAD processor
        // passed any "thd below X" gate. measure_aliasing already throws on
        // the same input; this is its sibling guard.
        auto silence = make_silence(1, kFft);
        REQUIRE_THROWS_AS(measure_thd(std::as_const(silence).view(), tone,
                                      kSampleRate, topts),
                          std::invalid_argument);
    }

    SECTION("a buffer shorter than the analysis window throws") {
        // Zero-padding a short buffer windows a TRUNCATED tone, and the
        // truncation edge leaks like any other discontinuity. Measured: a
        // clean coherent sine over 4096 samples analyzed at fft_length 16384
        // read THD -48.7 dB (truth -176.7) and THD+N +4.8 dB — while
        // recording coherent = true in the artifact.
        auto shortbuf = make_sine(1, kFft / 4, static_cast<float>(tone),
                                  kSampleRate, 0.5f);
        REQUIRE_THROWS_AS(measure_thd(std::as_const(shortbuf).view(), tone,
                                      kSampleRate, topts),
                          std::invalid_argument);

        // An offset that pushes a full-length window past the end is the
        // same trap from the other side.
        auto full = make_sine(1, kFft, static_cast<float>(tone), kSampleRate,
                              0.5f);
        ThdOptions off = topts;
        off.analysis_offset = 1;
        REQUIRE_THROWS_AS(measure_thd(std::as_const(full).view(), tone,
                                      kSampleRate, off),
                          std::invalid_argument);
    }

    SECTION("a fundamental at or above Nyquist throws") {
        // Accepted before the guard: 30 kHz at 48 kHz clamped to the Nyquist
        // bin and returned thd = 0 (gate passes) with thd_plus_n ~ 4e15.
        auto sig = make_sine(1, kFft, static_cast<float>(tone), kSampleRate,
                             0.5f);
        REQUIRE_THROWS_AS(measure_thd(std::as_const(sig).view(),
                                      kSampleRate / 2.0, kSampleRate, topts),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(measure_thd(std::as_const(sig).view(), 30000.0,
                                      kSampleRate, topts),
                          std::invalid_argument);
    }

    SECTION("the spectrum analyzer refuses the same zero-pad trap") {
        // magnitude_spectrum_curve shares the segment extraction. Measured: a
        // 4096-sample tone at fft_length 16384 through kaiser read a -14 dB
        // floor 16 bins from the tone (full-length control: -135 dB) — the
        // truncation edge reported as if it were signal, through the one
        // window whose whole purpose is a floor below -110 dB.
        auto shortbuf = make_sine(1, kFft / 4, static_cast<float>(tone),
                                  kSampleRate, 0.5f);
        ResponseOptions opts;
        opts.fft_length = kFft;
        opts.window = Window::kaiser;
        REQUIRE_THROWS_AS(
            magnitude_spectrum_curve(std::as_const(shortbuf).view(),
                                     kSampleRate, {}, opts),
            std::invalid_argument);
    }
}

TEST_CASE("Doctor: THD rejects a DC-bin fundamental", "[audio][doctor]") {
    // A fundamental that resolves to bin 0 has no energy after DC removal and
    // would divide thd/thd+n by a near-zero floor. The analyzer must reject it.
    constexpr int kFft = 1024;
    const double tone = coherent_tone(64, kFft, kSampleRate);
    auto signal = make_sine(/*channels=*/1, kFft, static_cast<float>(tone),
                            kSampleRate, 0.5f);

    ThdOptions topts;
    topts.fft_length = kFft;

    // A normal coherent tone resolves to a real bin and measures fine.
    REQUIRE_NOTHROW(measure_thd(std::as_const(signal).view(), tone, kSampleRate,
                                topts));

    // A near-DC "fundamental" rounds to bin 0 and must be rejected.
    const double dc_like = kSampleRate / kFft / 4.0; // < half a bin → bin 0.
    REQUIRE_THROWS_AS(measure_thd(std::as_const(signal).view(), dc_like,
                                  kSampleRate, topts),
                      std::invalid_argument);
}
