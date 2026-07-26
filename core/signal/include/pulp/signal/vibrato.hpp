#pragma once

/// @file vibrato.hpp
/// Three vibrato lineages that are not variations on one idea.
///
/// "Vibrato" names two different signal-theoretic objects, and seventy years of
/// guitar amplifiers calling both of them by the same word is the reason this
/// header ships three classes instead of one with a mode switch.
///
///   - **True pitch vibrato** (`DelayVibratoT`) modulates the read position of
///     a delay line. The rate of change of that position IS a frequency-scaling
///     factor, so it applies to every partial passing through equally: a 200 Hz
///     tone and a 4 kHz tone move by the same number of cents at the same
///     instant. This is real frequency modulation, and it is the only one of
///     the three that shifts pitch at all. (Dattorro, "Effect Design, Part 2:
///     Delay-Line Modulation and Chorus", JAES 45(10):764-788, 1997.)
///   - **Phase vibrato** (`PhaseVibratoT`, `UniVibeT`) modulates the corner
///     frequency of a cascade of allpass stages. An allpass's phase depends on
///     frequency *relative to its corner*, so sweeping the corner produces an
///     apparent pitch wobble that is strong near the swept corner and almost
///     absent far from it — frequency-DEPENDENT, and accompanied by moving
///     notches once the shifted path is summed with the direct one. That
///     frequency dependence is not an approximation error; it is the sound.
///
/// The test suite measures the distinction rather than asserting it: the same
/// instantaneous-frequency instrument reads the same cents at 200 Hz and 4 kHz
/// through `DelayVibratoT`, and readings roughly fifty times apart through
/// `PhaseVibratoT`.
///
/// The two phase engines then differ from each other in exactly one place, and
/// it is the place the circuits differed:
///
///   - Magnatone's varistor is a voltage-controlled resistor, so it tracks its
///     control voltage immediately. `PhaseVibratoT` drives its corner straight
///     from the LFO.
///   - The Univibe's modulator is an incandescent lamp facing four photocells.
///     A photoresistor's conductance rises fast and decays slowly, so the sweep
///     arrives late and leaves later. `UniVibeT` routes the LFO through
///     `VactrolConditionerT` (vactrol.hpp) — the same asymmetric one-pole the
///     Buchla lowpass gate uses, with its own constants — and that lag, plus
///     four deliberately unequal corner frequencies, is the whole character.
///
/// Lineage sources are cited for topology and documented behaviour only: stage
/// counts, the fact that the Univibe's phase capacitors are staggered rather
/// than matched, the fact that the modulator is a lamp and photocells, the fact
/// that a Chorus/Vibrato switch exists. No component values are reproduced; the
/// corner frequencies here are design parameters chosen as a geometric spread
/// and are labelled as such. (R.G. Keen, "The Technology of the Uni-Vibe",
/// geofex.com, for the Univibe topology; Parker & D'Angelo, "A Digital Model of
/// the Buchla Lowpass Gate", DAFx-13, by way of `VactrolConditionerT`, for the
/// optical lag.)
///
/// **No feedback anywhere.** All three engines are feedforward. `DelayVibratoT`
/// reads one tap with no regeneration; the two phase engines are allpass
/// cascades with no resonance loop. Each allpass stage is unity-gain for
/// steady-state sinusoids by the textbook property |H(e^jw)| = 1, so the
/// sinusoidal bound on a direct/shifted crossfade is exactly 1. The *sample*
/// gain is a different quantity and is larger — an allpass's impulse response
/// changes sign, so a worst-case bounded input is amplified by the cascade's
/// L1 norm. The suite measures both; see `kSinusoidalGainBound` and the
/// worst-case-gain test.
///
/// Anti-aliasing policy: **not applicable, by construction**. No engine here
/// contains a nonlinearity in the signal path. The allpass cascades are linear
/// time-varying, and the only nonlinearity in the module — `UniVibeT`'s
/// `c^kControlExponent` — shapes a control signal, never a sample. The delay
/// line's Lagrange read is an interpolation, not a harmonic generator. Nothing
/// oversamples and nothing needs to.
///
/// RT contract: `prepare()` sizes `DelayVibratoT`'s delay line for the WORST
/// CASE across the declared parameter ranges and may allocate; it is the only
/// member that does. `set_*`, `process()`, `latency_samples()`, and `reset()`
/// allocate nothing, take no locks, and perform no I/O, and are safe per sample
/// on the audio thread. Per-sample cost is dominated by one `tan` per active
/// allpass stage (the corner moves every sample) and, for `UniVibeT`, one
/// `pow`. All state is default-initialised to a valid fresh state.

#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vactrol.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// True pitch vibrato: a fractional delay whose tap position is modulated by a
/// sine, which scales the frequency of everything passing through it equally.
///
/// 100 % wet by design. A dry blend of a pitch-modulated tap against its own
/// dry signal is a chorus — a different effect with a different doc — and
/// mixing one in here would quietly turn the only true-pitch engine in the
/// module into the thing it is meant to be distinguishable from.
///
/// The delay/fade-in lifecycle ("vibrato arrives once the note has settled") is
/// a `DahdsrT` scaling the modulation depth, not a hand-rolled ramp. `reset()`
/// re-arms it, which is what makes `reset()` mean "new note" here.
template <typename SampleType = float>
class DelayVibratoT {
public:
    /// Vibrato rate. [design parameter] default 5.5 Hz, range 0.1 .. 20 Hz.
    static constexpr double kDefaultRateHz = 5.5;
    static constexpr double kMinRateHz = 0.1;
    static constexpr double kMaxRateHz = 20.0;

    /// Peak pitch excursion. Musical vibrato lives far above the few-cent pitch
    /// JND; the default sits between the narrow bowed-string end and the wide
    /// vocal end of the documented range, and effect vibrato has no citable
    /// figure of its own. [design parameter] default 20 cents, range 0 .. 100.
    static constexpr double kDefaultDepthCents = 20.0;
    static constexpr double kMaxDepthCents = 100.0;

    /// The base delay the modulation rides on, as a multiple of the modulation
    /// amplitude, so the read pointer never reaches the write pointer.
    /// [design parameter] default 2.0, range 1.5 .. 4.0.
    static constexpr double kBaseDelayHeadroom = 2.0;

    /// Floor on the base delay in samples. The Lagrange read touches index
    /// `i - 1`, so a delay below one sample would read past the write pointer
    /// no matter how small the modulation is.
    /// [design parameter] default 2 samples, range 1 .. 8.
    static constexpr double kMinBaseDelaySamples = 2.0;

    /// Guard samples past the longest read, covering the Lagrange window's
    /// `i + 2` reach. [design parameter] default 4 samples, range 2 .. 8.
    static constexpr int kInterpolatorMarginSamples = 4;

    /// Ceiling on both lifecycle times.
    /// [design parameter] default 5000 ms, range 1000 .. 20000 ms.
    static constexpr double kMaxLifecycleMs = 5000.0;

    /// Peak sample gain of the 4-point Lagrange kernel: the L1 norm of its
    /// weights, maximised at a half-sample offset where they are
    /// (-1, 9, 9, -1)/16. Provable, not fitted, and the reason this engine's
    /// worst-case gain is not exactly 0 dB despite being a single unit-gain
    /// tap. Asserted by the suite against a scan of the shipped interpolator.
    static constexpr double kInterpolatorPeakGain = 1.25;

    DelayVibratoT() {
        lfo_.set_wave(LfoWave::sine);
        lifecycle_.set_curve(0.0);
        lifecycle_.set_hold_ms(0.0);
        lifecycle_.set_decay_ms(0.0);
        update();
    }

    /// Sizes the delay line for the longest modulated read any legal parameter
    /// combination can ask for, so no `set_*` ever has to reallocate. That
    /// worst case is the slowest rate at the deepest setting — the amplitude in
    /// seconds is inversely proportional to rate — which is a few hundred
    /// milliseconds of storage, not a few.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        lfo_.prepare(sample_rate_);
        lifecycle_.prepare(sample_rate_);
        line_.prepare(worst_case_read_index(sample_rate_));
        update();
        arm();
    }

    void set_rate_hz(double hz) {
        if (!std::isfinite(hz)) return;
        rate_hz_ = std::clamp(hz, kMinRateHz, kMaxRateHz);
        lfo_.set_rate_hz(rate_hz_);
        update();
    }

    double rate_hz() const { return rate_hz_; }

    /// Peak pitch excursion in cents. The modulation amplitude in seconds is
    /// derived here, once, from `A = (2^(cents/1200) - 1) / (2*pi*rate)` — the
    /// inversion of `cents = 1200*log2(1 + 2*pi*rate*A)` — and never
    /// recomputed per sample.
    void set_depth_cents(double cents) {
        if (!std::isfinite(cents)) return;
        depth_cents_ = std::clamp(cents, 0.0, kMaxDepthCents);
        update();
    }

    double depth_cents() const { return depth_cents_; }

    /// Silence before the vibrato starts, in ms. Re-arms the lifecycle: the
    /// times describe the start of a note, so changing them mid-note has no
    /// meaningful continuation.
    void set_delay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        delay_ms_ = std::clamp(ms, 0.0, kMaxLifecycleMs);
        lifecycle_.set_delay_ms(delay_ms_);
        arm();
    }

    /// Time for the depth to ramp 0 -> 1 after the delay, in ms. Re-arms.
    void set_fade_in_ms(double ms) {
        if (!std::isfinite(ms)) return;
        fade_in_ms_ = std::clamp(ms, 0.0, kMaxLifecycleMs);
        lifecycle_.set_attack_ms(fade_in_ms_);
        arm();
    }

    /// Base delay in samples — the centre the modulation swings around.
    double base_delay_samples() const { return base_delay_samples_; }

    /// Peak modulation excursion in samples, before the lifecycle scales it.
    double modulation_amplitude_samples() const { return amplitude_samples_; }

    /// Current lifecycle depth scale in [0, 1]. 1 whenever no lifecycle times
    /// are set, so the common case costs nothing and starts at full depth.
    double depth_envelope() const { return lifecycle_active_ ? depth_env_ : 1.0; }

    /// Exactly `ceil((D0 + A) * fs)`: the longest delay the tap can read, which
    /// is the conservative reading of a time-varying delay's latency. Never 0 —
    /// a modulated tap cannot be free.
    std::size_t latency_samples() const {
        return static_cast<std::size_t>(
            std::ceil(base_delay_samples_ + amplitude_samples_));
    }

    void reset() {
        line_.reset();
        lfo_.reset();
        depth_env_ = 0.0;
        arm();
    }

    void discard_history() noexcept {
        line_.discard_history();
        lfo_.reset();
        depth_env_ = 0.0;
        arm();
    }

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            discard_history();
            return SampleType{0};
        }
        const double env = lifecycle_active_ ? advance_lifecycle() : 1.0;
        const double modulation = static_cast<double>(lfo_.next());
        const double delay = base_delay_samples_ + amplitude_samples_ * env * modulation;

        line_.push(input);

        const int index = static_cast<int>(std::floor(delay));
        const double frac = delay - static_cast<double>(index);
        // Weights in double even when the samples are float: the fractional
        // position is a double, and rounding it to float would put a
        // quantisation staircase directly on the modulation the effect exists
        // to produce.
        const double interpolated = Interpolator::lagrange(
            frac,
            static_cast<double>(line_.read(index - 1)),
            static_cast<double>(line_.read(index)),
            static_cast<double>(line_.read(index + 1)),
            static_cast<double>(line_.read(index + 2)));
        return static_cast<SampleType>(interpolated);
    }

private:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    /// The modulation amplitude in seconds that hits `cents` at `rate`.
    static double amplitude_seconds(double cents, double rate_hz) {
        return (units::cents_to_ratio(cents) - 1.0) / (kTwoPi * rate_hz);
    }

    /// The longest read index any legal parameter set can produce. Deepest
    /// depth at the slowest rate, since amplitude scales as 1/rate.
    static int worst_case_read_index(double sample_rate) {
        const double amplitude = amplitude_seconds(kMaxDepthCents, kMinRateHz) * sample_rate;
        const double base = std::max(kBaseDelayHeadroom * amplitude, kMinBaseDelaySamples);
        return static_cast<int>(std::ceil(base + amplitude)) + kInterpolatorMarginSamples;
    }

    void update() {
        amplitude_samples_ = amplitude_seconds(depth_cents_, rate_hz_) * sample_rate_;
        base_delay_samples_ =
            std::max(kBaseDelayHeadroom * amplitude_samples_, kMinBaseDelaySamples);
    }

    void arm() {
        lifecycle_active_ = delay_ms_ > 0.0 || fade_in_ms_ > 0.0;
        depth_env_ = 0.0;
        if (lifecycle_active_) lifecycle_.gate_on();
    }

    double advance_lifecycle() {
        depth_env_ = static_cast<double>(lifecycle_.next());
        return depth_env_;
    }

    double sample_rate_ = 44100.0;
    double rate_hz_ = kDefaultRateHz;
    double depth_cents_ = kDefaultDepthCents;
    double delay_ms_ = 0.0;
    double fade_in_ms_ = 0.0;

    double amplitude_samples_ = 0.0;
    double base_delay_samples_ = kMinBaseDelaySamples;
    double depth_env_ = 0.0;
    bool lifecycle_active_ = false;

    DelayLineT<SampleType> line_{};
    EffectLfoT<double> lfo_{};
    DahdsrT<double> lifecycle_{};
};

using DelayVibrato = DelayVibratoT<float>;
using DelayVibrato64 = DelayVibratoT<double>;

/// Magnatone-style phase vibrato: N cascaded first-order allpass stages whose
/// corner is driven straight from the LFO, summed back against the direct path.
///
/// The direct drive is the lineage's distinguishing feature, not an omission. A
/// varistor is a voltage-controlled resistor: its resistance follows its
/// control voltage with no storage element in the way. Adding an optical lag
/// here would turn this into the Univibe, which is the next class down and is
/// meant to sound different.
///
/// All active stages track the same corner. Staggered per-stage corners are the
/// Univibe's documented topology, not this circuit's, and giving both engines
/// the same spread would erase the difference the module exists to show.
template <typename SampleType = float>
class PhaseVibratoT {
public:
    /// [design parameter] default 0.8 Hz, range 0.05 .. 10 Hz. Amp vibrato runs
    /// slower than pedal vibrato; roughly 1-7 Hz is the characteristic band,
    /// stated as approximate context rather than a cited figure.
    static constexpr double kDefaultRateHz = 0.8;
    static constexpr double kMinRateHz = 0.05;
    static constexpr double kMaxRateHz = 10.0;

    /// Sweep width as a fraction of `kSweepOctaves`.
    /// [design parameter] default 0.6, range 0 .. 1.
    static constexpr double kDefaultDepth = 0.6;

    /// Total one-sided sweep span at full depth.
    /// [design parameter] default 1.5 octaves, range 0.5 .. 3.0.
    static constexpr double kSweepOctaves = 1.5;

    /// Centre corner of the cascade.
    /// [design parameter] default 500 Hz, range 200 .. 2000 Hz.
    static constexpr double kDefaultCenterHz = 500.0;
    static constexpr double kMinCenterHz = 200.0;
    static constexpr double kMaxCenterHz = 2000.0;

    /// [design parameter] default 2 stages, range 1 .. 4. Two matches the
    /// documented two-sequential-stage configuration.
    static constexpr int kDefaultStageCount = 2;
    static constexpr int kMaxStages = 4;

    /// Direct/shifted blend. [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kDefaultMix = 0.5;

    /// Steady-state sinusoidal gain bound for a direct/shifted CROSSFADE of
    /// unity-magnitude paths: `(1-mix)*1 + mix*1 = 1`. The +6 dB figure that
    /// comes from `|1| + |1|` applies to an unnormalised SUM, which is not the
    /// blend this class implements. Sample gain is a separate, larger quantity;
    /// see the header note.
    static constexpr double kSinusoidalGainBound = 1.0;

    PhaseVibratoT() {
        lfo_.set_wave(LfoWave::sine);
        lfo_.set_rate_hz(rate_hz_);
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        lfo_.prepare(sample_rate_);
        for (auto& stage : stages_) stage.prepare(static_cast<SampleType>(sample_rate_));
    }

    void set_rate_hz(double hz) {
        if (!std::isfinite(hz)) return;
        rate_hz_ = std::clamp(hz, kMinRateHz, kMaxRateHz);
        lfo_.set_rate_hz(rate_hz_);
    }

    double rate_hz() const { return rate_hz_; }

    void set_depth(double depth) {
        if (!std::isfinite(depth)) return;
        depth_ = std::clamp(depth, 0.0, 1.0);
    }
    double depth() const { return depth_; }

    void set_center_hz(double hz) {
        if (!std::isfinite(hz)) return;
        center_hz_ = std::clamp(hz, kMinCenterHz, kMaxCenterHz);
    }
    double center_hz() const { return center_hz_; }

    /// Stages that become active are cleared first, so switching count in does
    /// not fold a stale state from an earlier setting into the audio.
    void set_stage_count(int count) {
        const int clamped = std::clamp(count, 1, kMaxStages);
        for (int i = stage_count_; i < clamped; ++i) stages_[static_cast<std::size_t>(i)].reset();
        stage_count_ = clamped;
    }

    int stage_count() const { return stage_count_; }

    void set_mix(double mix) {
        if (!std::isfinite(mix)) return;
        mix_ = std::clamp(mix, 0.0, 1.0);
    }
    double mix() const { return mix_; }

    /// Corner the cascade is currently sitting at, in Hz. Reads the sweep
    /// without advancing it.
    double corner_hz() const { return corner_hz_; }

    /// Pure IIR: no bulk delay anywhere in the path.
    std::size_t latency_samples() const { return 0; }

    void reset() {
        lfo_.reset();
        for (auto& stage : stages_) stage.reset();
        corner_hz_ = center_hz_;
    }

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const double modulation = static_cast<double>(lfo_.next());
        corner_hz_ = center_hz_ * std::exp2(depth_ * kSweepOctaves * modulation);

        double shifted = static_cast<double>(input);
        for (int i = 0; i < stage_count_; ++i) {
            auto& stage = stages_[static_cast<std::size_t>(i)];
            // The corner moves every sample, so the coefficient is recomputed
            // every sample. Sharing one `tan` across stages would need a
            // coefficient setter the shared TPT filter does not expose; paying
            // it per stage keeps this composed rather than forked.
            stage.set_cutoff(static_cast<SampleType>(corner_hz_));
            shifted = static_cast<double>(
                stage.process_allpass(static_cast<SampleType>(shifted)));
        }

        const double direct = static_cast<double>(input);
        return static_cast<SampleType>(direct + mix_ * (shifted - direct));
    }

private:
    double sample_rate_ = 44100.0;
    double rate_hz_ = kDefaultRateHz;
    double depth_ = kDefaultDepth;
    double center_hz_ = kDefaultCenterHz;
    double mix_ = kDefaultMix;
    int stage_count_ = kDefaultStageCount;
    double corner_hz_ = kDefaultCenterHz;

    EffectLfoT<double> lfo_{};
    std::array<TptFilterT<SampleType>, kMaxStages> stages_{};
};

using PhaseVibrato = PhaseVibratoT<float>;
using PhaseVibrato64 = PhaseVibratoT<double>;

/// Univibe-style phase vibrato: four allpass stages at deliberately unequal
/// corners, all swept together by one lamp-and-photocell control chain.
///
/// Two things make this not a phaser and not `PhaseVibratoT`:
///
///   - The four corners are staggered, so some part of the band is always
///     sitting in a region of strong phase shift no matter where the sweep
///     currently is. The stagger is documented topology; the actual four
///     frequencies below are design parameters, because no standardised
///     capacitor set exists across published circuit descriptions, so the model
///     uses its own explicit calibration rather than any one clone's values.
///   - The control passes through a vactrol, not straight from the LFO. The
///     photoresistor's conductance rises in under a millisecond and decays over
///     tens, so the sweep is late arriving and later leaving. That asymmetry is
///     the "loping, behind-the-beat" quality; symmetric would be a phaser.
///
/// All four stages move by the SAME multiplicative scale, so the staggered
/// ratios hold at every instant — one dimensionless shape over four base
/// frequencies, not four independently fitted curves.
template <typename SampleType = float>
class UniVibeT {
public:
    /// The documented physical switch.
    enum class Mode : std::uint8_t { vibrato, chorus };

    static constexpr int kStageCount = 4;

    /// Staggered base corners, roughly a geometric 2.1x spread across the
    /// guitar midrange. Unequal is the documented fact; these values are
    /// [design parameter], defaults with ranges 100-500, 250-900, 500-1800, and
    /// 1000-3800 Hz respectively.
    static constexpr std::array<double, kStageCount> kStageBaseHz{200.0, 430.0, 900.0, 1900.0};

    /// [design parameter] default 3.0 Hz, range 0.3 .. 8 Hz.
    static constexpr double kDefaultRateHz = 3.0;
    static constexpr double kMinRateHz = 0.3;
    static constexpr double kMaxRateHz = 8.0;

    /// [design parameter] default 0.7, range 0 .. 1.
    static constexpr double kDefaultDepth = 0.7;

    /// [design parameter] default 1.0 octave, range 0.5 .. 2.0.
    static constexpr double kSweepOctaves = 1.0;

    /// Vactrol timings for a lamp-driven photocell. Fast on, slow off.
    /// [design parameter] rise default 0.8 ms, range 0.3 .. 3 ms;
    /// fall default 35 ms, range 15 .. 80 ms.
    static constexpr double kVactrolRiseMs = 0.8;
    static constexpr double kVactrolFallMs = 35.0;

    /// Control-to-corner-scale shaping exponent, the same `c^k` convention the
    /// lowpass gate uses. [design parameter] default 1.2, range 0.8 .. 2.0.
    static constexpr double kControlExponent = 1.2;

    /// Wet proportion in vibrato mode — near-total, mono.
    /// [design parameter] default 0.9, range 0.7 .. 1.0.
    static constexpr double kVibratoMix = 0.9;

    /// Wet proportion on the right output in chorus mode; the left stays dry.
    /// [design parameter] default 0.5, range 0.3 .. 0.7.
    static constexpr double kChorusBlend = 0.5;

    /// As `PhaseVibratoT::kSinusoidalGainBound`, and for the same reason.
    static constexpr double kSinusoidalGainBound = 1.0;

    UniVibeT() {
        lfo_.set_wave(LfoWave::sine);
        lfo_.set_rate_hz(rate_hz_);
        vactrol_.set_rise_ms(kVactrolRiseMs);
        vactrol_.set_fall_ms(kVactrolFallMs);
        corners_ = kStageBaseHz;
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        lfo_.prepare(sample_rate_);
        vactrol_.prepare(sample_rate_);
        for (auto& stage : stages_) stage.prepare(static_cast<SampleType>(sample_rate_));
    }

    void set_rate_hz(double hz) {
        if (!std::isfinite(hz)) return;
        rate_hz_ = std::clamp(hz, kMinRateHz, kMaxRateHz);
        lfo_.set_rate_hz(rate_hz_);
    }

    double rate_hz() const { return rate_hz_; }

    void set_depth(double depth) {
        if (!std::isfinite(depth)) return;
        depth_ = std::clamp(depth, 0.0, 1.0);
    }
    double depth() const { return depth_; }

    void set_mode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }

    /// Conditioned control in [0, 1] — the photocell's state, after the lag.
    double control() const { return vactrol_.control(); }

    /// Live corner of stage `index` in Hz.
    double stage_corner_hz(int index) const {
        return corners_[static_cast<std::size_t>(std::clamp(index, 0, kStageCount - 1))];
    }

    /// The corner scale a given conditioned control produces. Exposed because
    /// it is the one formula a test has to be able to predict from the shipped
    /// constants rather than restate.
    static double corner_scale(double control, double depth) {
        return std::exp2(depth * kSweepOctaves * (2.0 * std::pow(control, kControlExponent) - 1.0));
    }

    std::size_t latency_samples() const { return 0; }

    void reset() {
        lfo_.reset();
        vactrol_.reset();
        for (auto& stage : stages_) stage.reset();
        corners_ = kStageBaseHz;
    }

    void process(SampleType input, SampleType& out_left, SampleType& out_right) {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            out_left = out_right = SampleType{0};
            return;
        }
        const double lamp = static_cast<double>(lfo_.next_unipolar());
        const double control = vactrol_.process(lamp);
        const double scale = corner_scale(control, depth_);

        double shifted = static_cast<double>(input);
        for (int i = 0; i < kStageCount; ++i) {
            const auto slot = static_cast<std::size_t>(i);
            corners_[slot] = kStageBaseHz[slot] * scale;
            stages_[slot].set_cutoff(static_cast<SampleType>(corners_[slot]));
            shifted = static_cast<double>(
                stages_[slot].process_allpass(static_cast<SampleType>(shifted)));
        }

        const double direct = static_cast<double>(input);
        if (mode_ == Mode::vibrato) {
            const auto wet =
                static_cast<SampleType>(direct + kVibratoMix * (shifted - direct));
            out_left = wet;
            out_right = wet;
            return;
        }
        // Chorus splits the paths: one output stays untouched, which is why it
        // is asserted bit-exact rather than to a tolerance.
        out_left = input;
        out_right = static_cast<SampleType>(direct + kChorusBlend * (shifted - direct));
    }

private:
    double sample_rate_ = 44100.0;
    double rate_hz_ = kDefaultRateHz;
    double depth_ = kDefaultDepth;
    Mode mode_ = Mode::vibrato;

    std::array<double, kStageCount> corners_{};

    EffectLfoT<double> lfo_{};
    VactrolConditionerT<SampleType> vactrol_{};
    std::array<TptFilterT<SampleType>, kStageCount> stages_{};
};

using UniVibe = UniVibeT<float>;
using UniVibe64 = UniVibeT<double>;

}  // namespace pulp::signal
