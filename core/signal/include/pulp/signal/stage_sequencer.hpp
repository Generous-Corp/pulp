#pragma once

#include <pulp/signal/detail/modular_sequencing_common.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

// ── Stage sequencer ───────────────────────────────────────────────────────

/// How a `StageSeqT` walks from one stage to the next.
enum class SeqDirection {
    forward,   ///< `(s + 1) mod N`.
    reverse,   ///< `(s − 1 + N) mod N`.
    pingpong,  ///< Reflects at both ends without repeating them; period `2N − 2`.
    random,    ///< Seeded draw, rejection-sampled past skipped stages.
};

/// What the gate does across the clock pulses a stage occupies.
enum class StageGateMode {
    hold,    ///< High for the whole stage.
    repeat,  ///< A separate gate per pulse, at `repeat_duty`.
    single,  ///< High on the first pulse only.
    rest,    ///< Low for the whole stage — silence *with duration*.
};

/// The pulse-count stage sequencer.
///
/// *(lineage — informative)* The Roland-System-100M-format **Ryk M-185** and its
/// Eurorack descendant the **Intellijel Metropolis** are *stage* sequencers
/// rather than step sequencers: a small number of stages, each carrying a pitch,
/// a **pulse count** (how many clocks that stage lasts) and a **gate mode**
/// governing the gate across those pulses [Intellijel, *Metropolis Manual*
/// v1.30, 2020 — "eight STAGES, each with its own assignable gate mode, pulse
/// count, ratchet count, and pitch value"; "PULSE COUNT sets the number of
/// repetitions from 1 to 8"]. Cited for concepts and topology only.
///
/// The pulse count is what makes this different from a step sequencer with a
/// length knob: a stage that lasts three clocks is one musical event with a
/// duration, so `rest` at pulse count 3 is *three clocks of silence you can
/// hear the shape of*, and `hold` at pulse count 8 is a tied note. A step
/// sequencer spells those with three or eight steps you then have to keep in
/// sync when you edit the pattern.
///
/// **USE.** *Acid basslines* — eight stages, mostly `single`, one or two with
/// `slide`, pulse counts of 1–2, into a TB-303-style voice: the interplay of
/// slide and pulse count is the genre. *Generative melody* — `random` direction,
/// a few `rest` stages, a `QuantizeScaleT` downstream: never repeating but
/// always in key. *Polymeter against drums* — set `N` to 5 or 7 with `repeat`
/// gates while a 4/4 drum clock runs, and the sequence drifts in and out of
/// phase with the beat (the M-185 party trick). *Long tension builds* — one
/// stage at pulse count 8 in `hold`: a note held for eight clocks, then release.
///
/// **Ratchets are not baked in.** The hardware's per-stage ratchet is a
/// `ClockMultT` (kit) upstream of the clock input, or `repeat` mode. Duplicating
/// the kit inside a sequencer is how two definitions of a subdivision end up in
/// one library.
///
/// RT contract: as the header. Fixed-capacity arrays, no allocation anywhere.
template <typename SampleType = float>
class StageSeqT {
public:
    /// Stage capacity. Eight mirrors the cited hardware; the wider cap is ours.
    /// [design parameter] default 8 stages, range 1 .. 16.
    static constexpr int kMaxStages = 16;
    static constexpr int kDefaultStages = 8;

    /// Pulse count per stage. The 1–8 range is the cited hardware's.
    static constexpr int kMinPulseCount = 1;
    static constexpr int kMaxPulseCount = 8;

    /// High fraction of each pulse in `repeat` mode.
    /// [design parameter] default 0.5, range 0.1 .. 0.9.
    static constexpr double kRepeatDuty = 0.5;

    /// Constant-time portamento duration — "a constant time portamento very
    /// similar to the Roland TB-303" [Intellijel *Metropolis Manual* v1.30].
    /// [design parameter] default 30 ms, range 1 .. 500 ms.
    static constexpr double kSlideMs = 30.0;

    /// Seed for `SeqDirection::random`. A config value, never automatable
    /// (series law 2).
    /// [design parameter] default 0x2A3B, range 0 .. 2^31 − 1.
    static constexpr std::uint32_t kRandomSeed = 0x2A3Bu;

    /// Attempts a random draw makes before falling back to a deterministic
    /// forward scan. Rejection sampling has no worst-case bound, and an audio
    /// thread cannot have an unbounded loop; the fallback keeps the block RT-safe
    /// and still fully deterministic. Only reachable when nearly every stage is
    /// skipped (at 15 of 16 skipped the fallback fires on ~1.6 % of advances).
    /// [design parameter] default 64, range 8 .. 1024.
    static constexpr int kRandomDrawAttempts = 64;

    /// Longest clock period the `repeat` duty tracker will believe, in ms.
    /// Beyond it the clock is treated as stopped rather than as very slow, the
    /// same reasoning `ClockMultT` documents.
    /// [design parameter] default 4000 ms, range 250 .. 30000 ms.
    static constexpr double kMaxPeriodMs = 4000.0;

    /// One sample of sequencer output.
    struct Frame {
        SampleType pitch_v = SampleType{0};
        bool gate = false;
    };

    StageSeqT() {
        slew_.set_mode(SlewMode::linear);
        slew_.set_time_ms(0.0);
        for (int i = 0; i < kMaxStages; ++i) pulse_count_[i] = kMinPulseCount;
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        max_period_samples_ = units::ms_to_samples(kMaxPeriodMs, sample_rate_);
        slew_.prepare(sample_rate_);
    }

    // ── Pattern configuration (survives `reset()`) ─────────────────────────

    void set_num_stages(int n) {
        num_stages_ = std::clamp(n, 1, kMaxStages);
        if (stage_ >= num_stages_) stage_ = 0;
    }
    int num_stages() const { return num_stages_; }

    void set_direction(SeqDirection d) { direction_ = d; }
    SeqDirection direction() const { return direction_; }

    void set_stage_pitch(int s, SampleType volts) {
        if (in_range(s) && std::isfinite(static_cast<double>(volts))) pitch_v_[s] = volts;
    }
    SampleType stage_pitch(int s) const { return in_range(s) ? pitch_v_[s] : SampleType{0}; }

    void set_stage_pulse_count(int s, int count) {
        if (in_range(s)) pulse_count_[s] = std::clamp(count, kMinPulseCount, kMaxPulseCount);
    }
    int stage_pulse_count(int s) const { return in_range(s) ? pulse_count_[s] : kMinPulseCount; }

    void set_stage_gate_mode(int s, StageGateMode m) {
        if (in_range(s)) gate_mode_[s] = m;
    }
    StageGateMode stage_gate_mode(int s) const {
        return in_range(s) ? gate_mode_[s] : StageGateMode::hold;
    }

    void set_stage_slide(int s, bool on) {
        if (in_range(s)) slide_[s] = on;
    }
    bool stage_slide(int s) const { return in_range(s) && slide_[s]; }

    void set_stage_skip(int s, bool on) {
        if (in_range(s)) skip_[s] = on;
    }
    bool stage_skip(int s) const { return in_range(s) && skip_[s]; }

    /// Ceiling on the slide time. Two orders past the documented 1–500 ms range
    /// — a minute-long portamento is already musically absurd — so it only ever
    /// catches a nonsense value on its way into a sample-count conversion.
    /// [design parameter] default 60000 ms ceiling, range 1000 .. 600000 ms.
    static constexpr double kMaxSlideMs = 60000.0;

    void set_slide_ms(double ms) { slide_ms_ = seq_detail::clamp_finite(ms, 0.0, kMaxSlideMs); }
    double slide_ms() const { return slide_ms_; }

    void set_repeat_duty(double duty) { repeat_duty_ = seq_detail::clamp_finite(duty, 0.0, 1.0); }
    double repeat_duty() const { return repeat_duty_; }

    void set_seed(std::uint32_t seed) { rng_.set_seed(seed); }

    // ── Transport ──────────────────────────────────────────────────────────

    /// The modular reset jack (verb 2): position to the top of the pattern,
    /// gate low, pitch CV **held**, RNG untouched.
    ///
    /// The playhead is placed on stage 0, or on the first non-skipped stage at
    /// or after 0 if stage 0 is skipped, so "the first clock after a reset lands
    /// on the first playable stage's first pulse" holds under any skip pattern.
    /// The measured clock period survives, because it is a property of the
    /// incoming clock rather than a position in the pattern.
    void apply_reset_edge() {
        stage_ = first_playable_from(0);
        pulse_ = 0;
        ascending_ = true;
        started_ = false;
        gate_ = false;
    }

    /// The lifecycle verb (verb 1): everything `apply_reset_edge()` does, plus
    /// held outputs to zero, the clock-period measurement forgotten, and the
    /// RNG rewound to its seed.
    void reset() {
        apply_reset_edge();
        slew_.reset();
        target_pitch_ = SampleType{0};
        since_clock_ = 0;
        period_ = 0.0;
        have_clock_ = false;
        rng_.reset();
    }

    static constexpr int latency_samples() { return 0; }

    bool gate() const { return gate_; }
    SampleType pitch_v() const { return slew_.value(); }
    int stage() const { return stage_; }
    /// 0-based pulse index inside the current stage.
    int pulse() const { return pulse_; }
    /// True once the first clock after a reset has landed.
    bool started() const { return started_; }

    /// Advances one sample. See the header's order-of-operations rule.
    Frame process(bool run_high, bool reset_edge, bool clock_edge) {
        if (reset_edge) apply_reset_edge();

        if (!run_high) {
            gate_ = false;
            return Frame{slew_.value(), false};
        }

        // Counted only while running, so a stop does not inflate the measured
        // period and hand `repeat` mode a duty window longer than the clock it
        // is supposed to divide. The period this tracks is musical time, not
        // wall time.
        ++since_clock_;

        if (clock_edge) {
            if (have_clock_) {
                const double measured = static_cast<double>(since_clock_);
                period_ = (measured > 0.0 && measured <= max_period_samples_) ? measured : 0.0;
            }
            have_clock_ = true;
            since_clock_ = 0;
            advance();
        }

        gate_ = compute_gate();
        const SampleType pitch = slew_.process(target_pitch_);
        return Frame{pitch, gate_};
    }

private:
    static bool in_range(int s) { return s >= 0 && s < kMaxStages; }

    /// First non-skipped stage at or after `from`, or `from` itself when every
    /// stage is skipped (in which case nothing ever plays, by design).
    int first_playable_from(int from) const {
        const int n = num_stages_;
        for (int i = 0; i < n; ++i) {
            const int s = (from + i) % n;
            if (!skip_[s]) return s;
        }
        return from % n;
    }

    bool has_playable() const {
        for (int i = 0; i < num_stages_; ++i)
            if (!skip_[i]) return true;
        return false;
    }

    /// Enters `stage_`: arms the slide time for this stage, then points the
    /// smoother at its pitch. The order matters — `SlewLimiterT` computes its
    /// per-sample step when the target moves, so the time has to be right first.
    void enter_stage() {
        slew_.set_time_ms(slide_[stage_] ? slide_ms_ : 0.0);
        target_pitch_ = pitch_v_[stage_];
    }

    void advance() {
        // A pattern in which every stage is skipped has nowhere to go. Advancing
        // would either spin forever looking for a playable stage or land on one
        // the user asked to skip; the guard makes it a no-op with the gate low.
        if (!has_playable()) return;

        if (!started_) {
            // Re-validate the landing before entering it. `apply_reset_edge()`
            // already places the playhead on a playable stage, but the skip
            // flags can change between the reset and the clock — and a
            // freshly-constructed sequencer has never been through a reset at
            // all. Without this, a pattern whose stage 0 is skipped would play
            // stage 0 exactly once, on its downbeat.
            started_ = true;
            stage_ = first_playable_from(stage_);
            pulse_ = 0;
            enter_stage();
            return;
        }

        ++pulse_;
        if (pulse_ < pulse_count_[stage_]) return;

        pulse_ = 0;
        step_stage();
        enter_stage();
    }

    void step_stage() {
        const int n = num_stages_;
        if (n <= 1) return;

        switch (direction_) {
            case SeqDirection::forward:
                for (int i = 0; i < n; ++i) {
                    stage_ = (stage_ + 1) % n;
                    if (!skip_[stage_]) return;
                }
                return;
            case SeqDirection::reverse:
                for (int i = 0; i < n; ++i) {
                    stage_ = (stage_ - 1 + n) % n;
                    if (!skip_[stage_]) return;
                }
                return;
            case SeqDirection::pingpong:
                // At most 2n − 2 positions in the cycle, so 2n reflections
                // visit every one of them and the loop is bounded.
                for (int i = 0; i < 2 * n; ++i) {
                    pingpong_step();
                    if (!skip_[stage_]) return;
                }
                return;
            case SeqDirection::random:
                random_step();
                return;
        }
    }

    void pingpong_step() {
        const int n = num_stages_;
        if (ascending_) {
            if (stage_ >= n - 1) {
                ascending_ = false;
                stage_ = n - 2;
            } else {
                ++stage_;
            }
        } else {
            if (stage_ <= 0) {
                ascending_ = true;
                stage_ = 1;
            } else {
                --stage_;
            }
        }
    }

    void random_step() {
        const int n = num_stages_;
        // `next_uint() % n` carries the usual modulo bias for an `n` that does
        // not divide 2^32. At n ≤ 16 the bias is below one part in 2^28 — far
        // under any audible or measurable asymmetry — and the alternative
        // (rejection-sampling the draw itself as well as the skip) would make
        // the stream position depend on the pattern length, which is worse for
        // reproducibility than a bias nobody can measure.
        for (int attempt = 0; attempt < kRandomDrawAttempts; ++attempt) {
            const int candidate =
                static_cast<int>(rng_.next_uint() % static_cast<std::uint32_t>(n));
            if (!skip_[candidate]) {
                stage_ = candidate;
                return;
            }
        }
        stage_ = first_playable_from((stage_ + 1) % n);
    }

    bool compute_gate() const {
        if (!started_) return false;
        switch (gate_mode_[stage_]) {
            case StageGateMode::hold:
                return true;
            case StageGateMode::single:
                return pulse_ == 0;
            case StageGateMode::rest:
                return false;
            case StageGateMode::repeat:
                // The duty needs a period, and a multiplier cannot know the
                // length of a clock pulse that has not finished yet — the same
                // constraint `ClockMultT` documents. Until one period has been
                // measured the gate stays high for the whole pulse, which is the
                // only defensible answer: any fixed fallback length would be a
                // tempo this block has no business inventing.
                if (period_ <= 0.0) return true;
                return static_cast<double>(since_clock_) < repeat_duty_ * period_;
        }
        return false;
    }

    // Configuration.
    double sample_rate_ = 44100.0;
    double max_period_samples_ = 44100.0 * 4.0;
    int num_stages_ = kDefaultStages;
    SeqDirection direction_ = SeqDirection::forward;
    double slide_ms_ = kSlideMs;
    double repeat_duty_ = kRepeatDuty;
    SampleType pitch_v_[kMaxStages]{};
    int pulse_count_[kMaxStages]{};
    StageGateMode gate_mode_[kMaxStages]{};
    bool slide_[kMaxStages]{};
    bool skip_[kMaxStages]{};

    // Running state.
    int stage_ = 0;
    int pulse_ = 0;
    bool ascending_ = true;
    bool started_ = false;
    bool gate_ = false;
    std::int64_t since_clock_ = 0;
    double period_ = 0.0;
    bool have_clock_ = false;
    SampleType target_pitch_ = SampleType{0};
    ConstantTimeSlewLimiterT<SampleType> slew_{};
    Xorshift32 rng_{kRandomSeed};
};

using StageSeq = StageSeqT<float>;
using StageSeq64 = StageSeqT<double>;

}  // namespace pulp::signal
