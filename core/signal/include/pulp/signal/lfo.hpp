#pragma once

/// @file lfo.hpp
/// The house low-frequency oscillator: seven waveforms plus a continuous shape
/// morph, a delay/fade/repeat lifecycle, stereo and quadrature output, and a
/// deterministic random layer — all on one phase accumulator.
///
/// RT contract: `prepare()` and the `set_*()` setters recompute coefficients
/// and are control-side calls. `next()`, `next_stereo()`, `next_unipolar()`,
/// `next_quadrature()`, `retrigger()`, `reset()`, and the accessors allocate
/// nothing and are audio-thread safe. The type owns no memory.
///
/// USE — what each waveform is actually for, because picking the wrong one is
/// the difference between "modulated" and "musical":
///
/// - **sine** — vibrato and tremolo. The only wave with no corners, so it is
///   the only one that needs nothing downstream to smooth it.
/// - **triangle** — filter sweeps. Linear travel and no dwell at the extremes,
///   so the sweep reads as constant motion rather than as two held positions.
/// - **saw_down** — rhythmic plucks and pumping. The instant reset followed by
///   a decay is read by the ear as a beat, which is why it is the shape a
///   sidechain imitation converges on.
/// - **saw_up** — risers and reverse-feel swells; the same event heard
///   backwards.
/// - **square + pulse width** — choppers and trance gates. The pulse width *is*
///   the gate length. Feed it a `SlewLimiterT` unless clicks are the goal.
/// - **sh_random** — the classic random-step patch: sample-and-hold bass
///   filters, burbling leads.
/// - **smooth_random** (and its N-segment mode) — analog drift and wander, the
///   "nothing repeats but nothing jumps" layer. Adjacent to tape wow.
/// - **shape morph automated** — evolving motion from a single source, when a
///   second LFO would be one too many moving parts.
/// - **audio rate** — the tremolo-to-AM continuum. Ride `set_rate_hz()` from
///   5 Hz into the hundreds; the transition itself is the effect.
/// - **delay + fade in** — delayed vibrato, the performance behavior where the
///   vibrato arrives after the note has settled.
/// - **repeat 1 + fade out** — a shaped one-shot swell: an envelope wearing an
///   LFO's clothes.
///
/// Depth windows and cross-modulation of rate or fade are deliberately *not*
/// here. Routing concerns belong to `ModMatrixT` and `AttenuverterT`; "an
/// envelope rides the LFO's depth" is the matrix's `via` slot. The LFO stays a
/// pure source.

#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Round-2 effect-wave vocabulary. It is intentionally distinct from the
/// modulation toolkit's nested `LfoT::Wave`: selecting this overload also
/// selects the effect convention whose triangle is sine-aligned (zero at phase
/// 0, positive peak at 0.25) instead of the toolkit convention (-1 at phase 0).
enum class LfoWave : std::uint8_t {
    sine,
    triangle,
    saw_up,
    saw_down,
    square,
    sample_hold,
    smooth_random,
};

namespace detail {

/// Selects the waveform/rate convention at the type boundary. The default is
/// the shipped modulation-toolkit contract; Round-2 effects opt into their
/// distinct phase and waveform laws through `EffectLfoT` below.
enum class LfoConvention : std::uint8_t { toolkit, effect };

/// Shared implementation selected by the two explicit public LFO types below.
/// Keeping the convention in this internal type preserves the one-parameter
/// public `LfoT` template identity and makes effect behavior impossible to
/// select through precision or setter history.
template <typename SampleType, LfoConvention Convention>
class BasicLfoT {
public:
    static constexpr bool kEffectConvention = Convention == LfoConvention::effect;

    enum class Wave : std::uint8_t {
        sine,
        triangle,
        saw_up,
        saw_down,
        square,
        sh_random,
        smooth_random,
    };

    /// `free` runs from `reset()` and ignores `retrigger()` — that is what
    /// "free running" means, and a free LFO that silently restarts on note-on
    /// is the single most common LFO bug. `retrig` restarts phase *and* the
    /// lifecycle on every trigger. `one_shot` is `retrig` with an implied
    /// repeat count of 1 when none is set.
    enum class Mode : std::uint8_t { free, retrig, one_shot };

    enum class FadeCurve : std::uint8_t { linear, quadratic };

    enum class Stage : std::uint8_t { delay, fade_in, sustain, fade_out, done };

    /// Rate floor. One cycle every ~17 minutes; below this the LFO is a
    /// constant with extra steps.
    static constexpr double kMinRateHz = 0.001;

    /// Rate ceiling as a fraction of the sample rate. Above Nyquist the phase
    /// accumulator aliases into nonsense; 0.45 keeps the "extended" audio-rate
    /// mode usable right up to the edge without crossing it.
    static constexpr double kMaxRateFraction = 0.45;

    /// Ceiling of the bounded effect-LFO lane. The modulation-toolkit `Wave`
    /// overload keeps its audio-rate ceiling of `kMaxRateFraction * fs`.
    static constexpr double kMaxRateHz = 200.0;

    /// Suggested segment count for the N-segment smooth-random mode. The
    /// default is 1 (one target per cycle, the plain glide); 4 is the setting
    /// that produces the wandering character the mode exists for.
    static constexpr int kDefaultRandomSegments = 4;

    static constexpr int kMaxRandomSegments = 16;

    /// Upper bound on `set_repeat_count()`.
    static constexpr int kMaxRepeatCount = 128;

    // ── lifecycle ────────────────────────────────────────────────────────────

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        update_increment_();
        update_stage_lengths_();
        reset();
    }

    /// Rewind to a fresh start: phase 0, lifecycle restarted, random streams
    /// re-seeded. Deterministic — two `reset()` LFOs with the same seed produce
    /// bit-identical output forever.
    void reset() {
        phase_ = 0.0;
        cycles_ = 0;
        seed_chains_();
        restart_lifecycle_();
    }

    /// Restart phase and the delay/fade lifecycle. Ignored in `free` mode.
    ///
    /// The random streams deliberately do *not* re-seed here: a re-seeding
    /// retrigger would make every note's "random" modulation identical, which
    /// is the opposite of what a random LFO is for. The whole render stays
    /// reproducible because `reset()` is the only seeding point.
    void retrigger() {
        if (mode_ == Mode::free) return;
        phase_ = 0.0;
        cycles_ = 0;
        restart_lifecycle_();
    }

    // ── rate ─────────────────────────────────────────────────────────────────

    /// Free-running rate. The full range is legal, including audio rate; a
    /// caller mapping this to a knob should use `units::taper_log()`. The
    /// requested value is kept separately from the effective one, so a rate
    /// clamped by a low sample rate is restored by a later `prepare()` at a
    /// higher one instead of being silently lost.
    void set_rate_hz(double hz) {
        requested_rate_hz_ = hz;
        update_increment_();
    }

    /// Synced use. Tempo-to-period conversion is the *caller's* job so one
    /// transport read feeds every synced object in the plugin;
    /// `units::division_to_samples()` does the arithmetic.
    void set_period_samples(double samples) {
        const double min_period = 1.0 / kMaxRateFraction;
        increment_ = 1.0 / std::max(samples, min_period);
        rate_hz_ = increment_ * sample_rate_;
        requested_rate_hz_ = rate_hz_;
    }

    double rate_hz() const { return rate_hz_; }
    double period_samples() const { return increment_ > 0.0 ? 1.0 / increment_ : 0.0; }

    // ── shape ────────────────────────────────────────────────────────────────

    void set_wave(Wave w) requires (!kEffectConvention) {
        wave_ = w;
        update_increment_();
    }
    void set_wave(LfoWave w) requires kEffectConvention {
        morph_enabled_ = false;
        switch (w) {
            case LfoWave::sine: wave_ = Wave::sine; break;
            case LfoWave::triangle: wave_ = Wave::triangle; break;
            case LfoWave::saw_up: wave_ = Wave::saw_up; break;
            case LfoWave::saw_down: wave_ = Wave::saw_down; break;
            case LfoWave::square: wave_ = Wave::square; break;
            case LfoWave::sample_hold: wave_ = Wave::sh_random; break;
            case LfoWave::smooth_random: wave_ = Wave::smooth_random; break;
        }
        update_increment_();
    }
    Wave wave() const requires (!kEffectConvention) { return wave_; }
    LfoWave wave() const requires kEffectConvention {
        switch (wave_) {
            case Wave::sine: return LfoWave::sine;
            case Wave::triangle: return LfoWave::triangle;
            case Wave::saw_up: return LfoWave::saw_up;
            case Wave::saw_down: return LfoWave::saw_down;
            case Wave::square: return LfoWave::square;
            case Wave::sh_random: return LfoWave::sample_hold;
            case Wave::smooth_random: return LfoWave::smooth_random;
        }
        return LfoWave::sine;
    }

    /// Continuous blend across sine -> triangle -> saw_up -> square as `m`
    /// runs 0 -> 3, by crossfading the adjacent pair. Enables morph mode, which
    /// overrides `set_wave()`. At integer positions the output is bit-identical
    /// to the corresponding pure wave.
    void set_shape_morph(float m) {
        shape_morph_ = std::clamp(m, 0.0f, 3.0f);
        morph_enabled_ = true;
    }

    void set_shape_morph_enabled(bool on) { morph_enabled_ = on; }
    bool shape_morph_enabled() const { return morph_enabled_; }

    /// Duty cycle of the square wave.
    void set_pulse_width(float pw) { pulse_width_ = std::clamp(pw, 0.05f, 0.95f); }

    /// Rise/fall skew of the triangle. 0 is symmetric; +1 pushes the peak to
    /// the end of the cycle (a ramp up with a cliff), -1 to the start.
    void set_tri_bias(float b) { tri_bias_ = std::clamp(b, -1.0f, 1.0f); }

    /// Crossfade the chosen waveform against a per-cycle sample-and-hold random
    /// value — "how random is this LFO" as one control. Works with every wave,
    /// including the random ones.
    void set_random_blend(float b) { random_blend_ = std::clamp(b, 0.0f, 1.0f); }

    /// Targets latched per cycle in `smooth_random`. 1 is the plain
    /// one-target-per-cycle glide; higher values latch N targets per cycle and
    /// cosine-glide between them at N times the rate, which wanders instead of
    /// swinging. Audibly a different animal, not just a faster one.
    void set_random_segments(int n) { segments_ = std::clamp(n, 1, kMaxRandomSegments); }

    /// Seed for the random waveforms and the random blend. Applied at the next
    /// `reset()`.
    void set_seed(std::uint32_t s) {
        seed_ = s;
        if constexpr (kEffectConvention) reset();
    }

    // ── phase & stereo ───────────────────────────────────────────────────────

    /// Output phase offset in cycles (0..1); present a 0-360 degree control.
    void set_phase_offset(double cycles01) { phase_offset_ = wrap01(cycles01); }

    /// Offset of the right output in `next_stereo()`, in cycles. Present a
    /// +/-180 degree control; 0.25 is quadrature.
    void set_stereo_offset(double cycles01) {
        if constexpr (kEffectConvention)
            phase_offset_ = wrap01(cycles01);
        else
            stereo_offset_ = wrap01(cycles01);
    }

    // ── lifecycle shaping ────────────────────────────────────────────────────

    /// Silence (a neutral output) before the LFO starts. Phase does not advance
    /// during the delay, so the wave always begins at its own start.
    void set_delay_ms(double ms) {
        delay_ms_ = std::clamp(ms, 0.0, 120000.0);
        update_stage_lengths_();
        restart_if_idle_();
    }

    /// Depth ramps 0 -> 1 over this time once the delay expires.
    void set_fade_in_ms(double ms) {
        fade_in_ms_ = std::clamp(ms, 0.0, 120000.0);
        update_stage_lengths_();
        restart_if_idle_();
    }

    /// Depth ramps back to 0 after the repeat count is exhausted.
    void set_fade_out_ms(double ms, bool enabled) {
        fade_out_ms_ = std::clamp(ms, 0.0, 120000.0);
        fade_out_enabled_ = enabled;
        update_stage_lengths_();
    }

    void set_fade_curve(FadeCurve c) { fade_curve_ = c; }

    /// 0 = run forever. Otherwise the LFO completes N cycles and then fades out
    /// or stops. Repeat 1 plus a fade out is how you get a shaped one-shot
    /// swell out of an oscillator.
    void set_repeat_count(int n) { repeat_count_ = std::clamp(n, 0, kMaxRepeatCount); }

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    Stage stage() const { return stage_; }
    bool finished() const { return stage_ == Stage::done; }
    long long cycles_completed() const { return cycles_; }
    double phase() const {
        if constexpr (kEffectConvention)
            return wrap01(phase_ + static_cast<double>(phase_offset_));
        return phase_;
    }

    // ── output ───────────────────────────────────────────────────────────────

    /// Bipolar output in [-1, 1], scaled by the lifecycle depth envelope.
    SampleType next() {
        if constexpr (kEffectConvention) advance_();
        const float env = envelope_();
        const double p = wrap01(phase_ + static_cast<double>(phase_offset_));
        update_chain_(chains_[0], p);
        if constexpr (kEffectConvention)
            return static_cast<SampleType>(static_cast<double>(env)
                                           * legacy_effect_shape_(p, chains_[0]));
        const float value = shape_(p, chains_[0]);
        advance_();
        return static_cast<SampleType>(env * value);
    }

    /// Unipolar output in [0, 1]. During the delay and after the fade out this
    /// sits at 0.5 — the neutral value for a unipolar destination, which is
    /// what "silence" means for one.
    SampleType next_unipolar() { return static_cast<SampleType>(bi_to_uni(static_cast<float>(next()))); }

    /// The stereo pair: the same LFO evaluated at `phase` and
    /// `phase + stereo_offset`. A zero offset produces bit-identical channels
    /// for every waveform, including the random ones — the second latch stream
    /// is only engaged when there is actually an offset to decorrelate.
    ///
    /// With a nonzero offset the random waveforms give the right channel its
    /// own decorrelated latch stream rather than a time-shifted copy of the
    /// left, because a time-shifted random stream would require values the
    /// generator has not drawn yet. Two decorrelated random walks locked to one
    /// rate is also the more useful stereo behavior.
    void next_stereo(SampleType& left, SampleType& right) {
        const float env = envelope_();
        const double pl = wrap01(phase_ + static_cast<double>(phase_offset_));
        update_chain_(chains_[0], pl);
        const float l = shape_(pl, chains_[0]);
        float r = l;
        if (stereo_offset_ != 0.0f) {
            const double pr = wrap01(pl + static_cast<double>(stereo_offset_));
            update_chain_(chains_[1], pr);
            r = shape_(pr, chains_[1]);
        }
        left = static_cast<SampleType>(env * l);
        right = static_cast<SampleType>(env * r);
        advance_();
    }

    /// Exact sine/cosine pair at the current phase, regardless of the selected
    /// waveform. Orthogonal by construction, which is what single-sideband and
    /// barberpole effects need — a "quadrature" pair built by offsetting a
    /// triangle or a square is not orthogonal and the sidebands do not cancel.
    void next_quadrature(SampleType& sine_out, SampleType& cosine_out) {
        if constexpr (kEffectConvention) advance_();
        const float env = envelope_();
        const double p = wrap01(phase_ + static_cast<double>(phase_offset_));
        update_chain_(chains_[0], p);
        const double angle = 6.283185307179586477 * p;
        if constexpr (kEffectConvention) {
            sine_out = static_cast<SampleType>(static_cast<double>(env) * std::sin(angle));
            cosine_out = static_cast<SampleType>(static_cast<double>(env) * std::cos(angle));
            return;
        }
        sine_out = static_cast<SampleType>(env * static_cast<float>(std::sin(angle)));
        cosine_out = static_cast<SampleType>(env * static_cast<float>(std::cos(angle)));
        advance_();
    }

private:
    struct RandomChain {
        Xorshift32 rng{};
        float sh = 0.0f;
        float glide_prev = 0.0f;
        float glide_next = 0.0f;
        float seg_frac = 0.0f;
        int seg_index = -1;
        float prev_phase = 2.0f;
        bool primed = false;

        float smooth() const {
            const float w = 0.5f - 0.5f * std::cos(3.14159265358979324f * seg_frac);
            return glide_prev + (glide_next - glide_prev) * w;
        }
    };

    static double wrap01(double x) { return x - std::floor(x); }
    static float wrap01(float x) { return x - std::floor(x); }

    void update_increment_() {
        const double max_rate = kEffectConvention
                                    ? kMaxRateHz
                                    : kMaxRateFraction * sample_rate_;
        const double min_rate = kEffectConvention ? 0.0 : kMinRateHz;
        rate_hz_ = std::clamp(requested_rate_hz_, min_rate,
                              std::max(min_rate, max_rate));
        increment_ = rate_hz_ / sample_rate_;
    }

    void update_stage_lengths_() {
        delay_samples_ = static_cast<long long>(std::lround(delay_ms_ * 0.001 * sample_rate_));
        fade_in_samples_ = static_cast<long long>(std::lround(fade_in_ms_ * 0.001 * sample_rate_));
        fade_out_samples_ = static_cast<long long>(std::lround(fade_out_ms_ * 0.001 * sample_rate_));
    }

    void seed_chains_() {
        chains_[0] = RandomChain{};
        chains_[1] = RandomChain{};
        chains_[0].rng.seed(static_cast<std::uint32_t>(mix64(seed_) & 0xFFFFFFFFu));
        chains_[1].rng.seed(static_cast<std::uint32_t>(mix64(seed_ ^ 0x5DEECE66Du) >> 32));
    }

    void restart_lifecycle_() {
        stage_pos_ = 0;
        fade_out_start_ = 1.0f;
        if (delay_samples_ > 0) stage_ = Stage::delay;
        else if (fade_in_samples_ > 0) stage_ = Stage::fade_in;
        else stage_ = Stage::sustain;
    }

    /// Re-derive the opening stage when nothing has been produced yet, so the
    /// natural `prepare()` -> configure -> run order works without a second
    /// `reset()`.
    void restart_if_idle_() {
        if (stage_pos_ == 0 && cycles_ == 0 && phase_ == 0.0) restart_lifecycle_();
    }

    int effective_repeat_() const {
        if (mode_ == Mode::one_shot && repeat_count_ == 0) return 1;
        return repeat_count_;
    }

    float fade_shape_(float t) const {
        const float c = std::clamp(t, 0.0f, 1.0f);
        return fade_curve_ == FadeCurve::quadratic ? c * c : c;
    }

    float envelope_() const {
        switch (stage_) {
            case Stage::delay:
                return 0.0f;
            case Stage::fade_in:
                if (fade_in_samples_ <= 0) return 1.0f;
                return fade_shape_(static_cast<float>(stage_pos_)
                                   / static_cast<float>(fade_in_samples_));
            case Stage::sustain:
                return 1.0f;
            case Stage::fade_out:
                if (fade_out_samples_ <= 0) return 0.0f;
                return fade_out_start_
                       * (1.0f - fade_shape_(static_cast<float>(stage_pos_)
                                             / static_cast<float>(fade_out_samples_)));
            case Stage::done:
            default:
                return 0.0f;
        }
    }

    void advance_() {
        const Stage before = stage_;
        advance_lifecycle_();
        if (before == Stage::delay || before == Stage::done) return;
        advance_phase_();
    }

    void advance_lifecycle_() {
        switch (stage_) {
            case Stage::delay:
                if (++stage_pos_ >= delay_samples_) {
                    stage_ = fade_in_samples_ > 0 ? Stage::fade_in : Stage::sustain;
                    stage_pos_ = 0;
                }
                return;
            case Stage::fade_in:
                if (++stage_pos_ >= fade_in_samples_) {
                    stage_ = Stage::sustain;
                    stage_pos_ = 0;
                }
                return;
            case Stage::fade_out:
                if (++stage_pos_ >= fade_out_samples_) {
                    stage_ = Stage::done;
                    stage_pos_ = 0;
                }
                return;
            case Stage::sustain:
            case Stage::done:
            default:
                return;
        }
    }

    void advance_phase_() {
        phase_ += increment_;
        if (phase_ >= 1.0) {
            const double whole = std::floor(phase_);
            phase_ -= whole;
            cycles_ += static_cast<long long>(whole);
            on_cycles_completed_();
        }
    }

    void on_cycles_completed_() {
        const int limit = effective_repeat_();
        if (limit <= 0 || cycles_ < limit) return;
        if (stage_ == Stage::fade_out || stage_ == Stage::done) return;
        if (fade_out_enabled_ && fade_out_samples_ > 0) {
            fade_out_start_ = envelope_();
            stage_ = Stage::fade_out;
            stage_pos_ = 0;
        } else {
            stage_ = Stage::done;
            stage_pos_ = 0;
        }
    }

    void update_chain_(RandomChain& c, double phase) {
        const auto p = static_cast<float>(phase);
        if (!c.primed || p < c.prev_phase) {
            c.sh = c.rng.next_bipolar();
            c.primed = true;
        }
        c.prev_phase = p;
        const float scaled = p * static_cast<float>(segments_);
        int seg = static_cast<int>(scaled);
        seg = std::clamp(seg, 0, segments_ - 1);
        if (seg != c.seg_index) {
            c.glide_prev = c.seg_index < 0 ? c.rng.next_bipolar() : c.glide_next;
            c.glide_next = c.rng.next_bipolar();
            c.seg_index = seg;
        }
        c.seg_frac = std::clamp(scaled - static_cast<float>(seg), 0.0f, 1.0f);
    }

    float pure_wave_(Wave w, double phase, const RandomChain& c) const {
        const auto p = static_cast<float>(phase);
        switch (w) {
            case Wave::sine:
                return static_cast<float>(std::sin(6.283185307179586477 * phase));
            case Wave::triangle: {
                if constexpr (kEffectConvention) {
                    if (p < 0.25f) return 4.0f * p;
                    if (p < 0.75f) return 2.0f - 4.0f * p;
                    return 4.0f * p - 4.0f;
                }
                const float peak = std::clamp(0.5f + 0.5f * tri_bias_, 1.0e-4f, 1.0f - 1.0e-4f);
                if (p < peak) return -1.0f + 2.0f * (p / peak);
                return 1.0f - 2.0f * ((p - peak) / (1.0f - peak));
            }
            case Wave::saw_up:
                return 2.0f * p - 1.0f;
            case Wave::saw_down:
                return 1.0f - 2.0f * p;
            case Wave::square:
                return p < pulse_width_ ? 1.0f : -1.0f;
            case Wave::sh_random:
                return c.sh;
            case Wave::smooth_random:
            default:
                return c.smooth();
        }
    }

    /// The Round-2 effect lane was specified and tested at double precision;
    /// retaining the toolkit's float waveform core here would put quantization
    /// steps into otherwise-linear sweeps. Random modes intentionally reuse
    /// the shared seeded implementation because their exact values, rather
    /// than their analytic slope, are the contract.
    double legacy_effect_shape_(double p, const RandomChain& c) const {
        switch (wave_) {
            case Wave::sine: return std::sin(6.283185307179586477 * p);
            case Wave::triangle:
                if (p < 0.25) return 4.0 * p;
                if (p < 0.75) return 2.0 - 4.0 * p;
                return 4.0 * p - 4.0;
            case Wave::saw_up: return 2.0 * p - 1.0;
            case Wave::saw_down: return 1.0 - 2.0 * p;
            case Wave::square: return p < static_cast<double>(pulse_width_) ? 1.0 : -1.0;
            case Wave::sh_random:
            case Wave::smooth_random:
            default: return static_cast<double>(shape_(p, c));
        }
    }

    float morph_shape_(double phase, const RandomChain& c) const {
        static constexpr Wave kMorphOrder[4] = {
            Wave::sine, Wave::triangle, Wave::saw_up, Wave::square};
        int index = static_cast<int>(shape_morph_);
        index = std::clamp(index, 0, 2);
        const float w = shape_morph_ - static_cast<float>(index);
        // Both endpoints of the pair short-circuit, so an integer morph
        // position is the pure wave bit for bit rather than a crossfade that
        // happens to land near it.
        if (w == 0.0f) return pure_wave_(kMorphOrder[index], phase, c);
        if (w == 1.0f) return pure_wave_(kMorphOrder[index + 1], phase, c);
        const float a = pure_wave_(kMorphOrder[index], phase, c);
        const float b = pure_wave_(kMorphOrder[index + 1], phase, c);
        return a + w * (b - a);
    }

    float shape_(double phase, const RandomChain& c) const {
        const float base = morph_enabled_ ? morph_shape_(phase, c)
                                          : pure_wave_(wave_, phase, c);
        if (random_blend_ <= 0.0f) return base;
        return base + random_blend_ * (c.sh - base);
    }

    RandomChain chains_[2]{};

    double sample_rate_ = 48000.0;
    double rate_hz_ = 1.0;
    double requested_rate_hz_ = 1.0;
    double increment_ = 1.0 / 48000.0;
    double phase_ = 0.0;

    double delay_ms_ = 0.0;
    double fade_in_ms_ = 0.0;
    double fade_out_ms_ = 0.0;
    long long delay_samples_ = 0;
    long long fade_in_samples_ = 0;
    long long fade_out_samples_ = 0;
    long long stage_pos_ = 0;

    double phase_offset_ = 0.0;
    float stereo_offset_ = 0.0f;
    float shape_morph_ = 0.0f;
    float pulse_width_ = 0.5f;
    float tri_bias_ = 0.0f;
    float random_blend_ = 0.0f;
    float fade_out_start_ = 1.0f;

    std::uint32_t seed_ = 0x12345678u;
    int segments_ = 1;
    int repeat_count_ = 0;
    // 64-bit: a free-running LFO at the rate ceiling wraps ~86400 cycles per
    // second at 192 kHz, which overflows a 32-bit counter within hours.
    long long cycles_ = 0;

    Wave wave_ = Wave::sine;
    Mode mode_ = Mode::free;
    Stage stage_ = Stage::sustain;
    FadeCurve fade_curve_ = FadeCurve::linear;
    bool morph_enabled_ = false;
    bool fade_out_enabled_ = false;
};

} // namespace detail

template <typename SampleType = float>
class LfoT : public detail::BasicLfoT<SampleType, detail::LfoConvention::toolkit> {};
template <typename SampleType = float>
class EffectLfoT : public detail::BasicLfoT<SampleType, detail::LfoConvention::effect> {};
using Lfo = LfoT<float>;
using Lfo64 = LfoT<double>;
using EffectLfo = EffectLfoT<float>;
using EffectLfo64 = EffectLfoT<double>;

} // namespace pulp::signal
