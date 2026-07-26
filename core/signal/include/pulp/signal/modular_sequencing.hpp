#pragma once

/// @file modular_sequencing.hpp
/// Authored sequence in the signal domain: blocks that remember a pattern, walk
/// it under a clock, and hand back pitch CV and gates.
///
/// `trigger_kit.hpp` ships the *event plumbing* — what an edge is, how a gate
/// is made, how a clock is divided and multiplied. What it does not ship is a
/// pattern. This header is that layer, and it **composes** the kit rather than
/// re-deriving it: every edge in here is a `TriggerDetectT` edge, every slide is
/// a `SlewLimiterT` in constant-time mode, every ratchet is an upstream
/// `ClockMultT`, and every random draw is a seeded `Xorshift32`.
///
/// Three sequencer topologies, chosen because none of them is a linear step
/// sequencer and none of them can be spelled by a MIDI arpeggiator:
///
///   - `StageSeqT` — the **pulse-count stage** sequencer (Ryk M-185 / Intellijel
///     Metropolis lineage): a handful of stages, each holding a pitch, a number
///     of clock pulses it occupies, and a gate mode. Polymeter, held notes and
///     TB-303-lineage slide fall out of it directly.
///   - `CartesianWalkT` — the **2-D grid** sequencer (Make Noise René lineage):
///     a grid of voltages with X and Y clocked *independently*, so two cheap
///     clocks generate a sequence far longer than the grid.
///   - `RunglerT` — the **shift-register/DAC** sequencer (Rob Hordijk's Blippoo
///     Box rungler): deterministic chaos, a long quasi-periodic line out of a
///     few bits of state.
///
/// Plus the two support blocks every patch built from those three needs:
/// `QuantizeScaleT` (snap any CV to a scale on the 1 V/oct standard) and
/// `GateLogicT` / `ProbGateT` (Boolean and probabilistic gate combinators).
///
/// ── Pitch is volts, everywhere ────────────────────────────────────────────
///
/// Pitch is carried as **volts on the 1 V/octave standard** (12 semitones per
/// volt, `units::volts_to_semitones`), not as note numbers, so any block's
/// pitch output feeds any other's pitch input, the quantizer, a slew limiter,
/// or a filter cutoff with no unit negotiation. That interchangeability is the
/// whole reason these are signal blocks rather than note generators.
///
/// ── The two reset verbs (the ambiguity this header closes) ────────────────
///
/// A "reset" input on one hardware module zeroes an LFO's phase, on another
/// zeroes a counter, on another jumps a pattern to step 0, and a user can never
/// predict which. There are exactly **two** verbs here and no third:
///
///   1. **`reset()` — the C++ lifecycle verb.** Returns the object to its
///      as-constructed state: positions 0, counters 0, gates low, held outputs
///      0, **and every RNG rewound to its fixed seed** (series law 2). This is
///      what the framework calls on patch load, transport-stop-to-top and voice
///      steal. It is not clocked by audio. Configuration — stage pitches, grid
///      values, scale masks — survives it; only running state is cleared.
///   2. **`apply_reset_edge()` — the modular reset jack.** A rising edge on a
///      reset input during playback. It sets every **position** (counter, step,
///      grid coordinate) to its start-of-pattern value and forces every **gate**
///      low, but leaves every **continuous output** (pitch CV) holding its last
///      value until the next clock, and **never advances or rewinds an RNG**.
///      Reseeding on a live reset jack would make every reset sound identical,
///      which is musically wrong; reseeding is a `reset()`-only concern.
///
/// `RunglerT` is the single documented exception to the "continuous output
/// holds" half of verb 2: its reset edge restores the seed pattern *and* the
/// DAC output immediately, because re-pinning a wandering rungler line live is
/// the entire point of giving it a reset jack, and its output is a stepped hold
/// that steps by design (§ `RunglerT`) rather than a pitch CV that would click.
///
/// ── Run / stop / reset transport ──────────────────────────────────────────
///
/// The three sequencers take `process(run_high, reset_edge, clock_edge, …)` and
/// obey identical rules:
///
///   1. **Run is a level, not an edge.** While `run_high` is false the block
///      ignores clock edges, holds its position, and forces its gate low — no
///      hung gate across a stop. It does *not* reset position: stop/start is
///      pause/continue, as on hardware.
///   2. **Reset is independent of run.** A reset edge applies verb-2 semantics
///      whether running or stopped; resetting while stopped arms the pattern at
///      the top for the next run.
///   3. **Order of operations inside one `process` call**, fixed and tested:
///      (a) apply the reset edge; (b) if not running, force the gate low and
///      return the held CV; (c) if a clock edge, advance and recompute gate and
///      CV; (d) update the slide smoother; (e) return. Reset and clock in the
///      same sample is therefore deterministic — reset wins, then the clock
///      advances from the top, so the downbeat fires on that very clock.
///      Because (b) returns before (d), a slide freezes where it was when the
///      transport stopped and resumes from there, which is what "position held"
///      has to mean for a continuous output.
///   4. **No free-running.** Nothing here generates its own clock. Timing comes
///      from `ClockDividerT` / `ClockMultT` upstream so there is one timing
///      authority in a patch.
///
/// `TransportEdgeT` is the only place reset-edge detection lives, so every
/// sequencer agrees on what an edge is.
///
/// ── Oversampling policy ───────────────────────────────────────────────────
///
/// **None, deliberately.** Every output here is a stepped hold — a DAC level, a
/// quantized semitone, a gate — and the steps *are* the sound, not an artefact
/// of undersampling a continuous nonlinearity. There is no gain-carrying
/// nonlinearity in this header to alias, so band-limiting would only remove the
/// intended edge. A caller who wants the steps softened puts a `SlewLimiterT`
/// downstream, which is a musical choice and not ours to make.
///
/// ── Determinism ───────────────────────────────────────────────────────────
///
/// Series law 2: every draw is a seeded `Xorshift32` rewound by `reset()`;
/// seeds are compile-time/config values and never automatable parameters. Every
/// block is per-sample and carries no block-length state, so a render is
/// bit-identical across buffer sizes as well as across resets.
///
/// RT contract: `prepare()` recomputes sample counts and allocates nothing.
/// `set_*`, `process()`, `apply_reset_edge()` and `reset()` allocate nothing,
/// take no locks, and perform no I/O. All state is POD and fixed-capacity; a
/// default-constructed instance is already in the state `reset()` would put it
/// in. Every block reports `latency_samples() == 0`: a slide is a continuous
/// smoother, not a delay.

#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/trigger_kit.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

namespace seq_detail {

/// Clamps into `[lo, hi]` and maps a non-finite input onto `lo`.
///
/// `std::clamp` propagates NaN, and these values go on to index arrays, size
/// windows and get cast to `int` — where a NaN or an out-of-range double is
/// undefined behaviour rather than a wrong note. Every setter that accepts a
/// number from outside the library goes through here, so a broken upstream
/// block produces a bounded output instead of a crash.
inline double clamp_finite(double x, double lo, double hi) {
    if (!(x >= lo)) return lo;  // false for NaN and for anything below lo
    if (!(x <= hi)) return hi;
    return x;
}

}  // namespace seq_detail

// ── Transport front end ───────────────────────────────────────────────────

/// Converts a run **level** and a reset **level** into the `(run_high,
/// reset_edge)` pair the sequencers consume, and optionally detects the clock
/// edge alongside them.
///
/// It exists so that "what is a reset edge" has exactly one answer in the
/// library. It carries no musical state at all: hand it the same three signals
/// and it produces the same three booleans regardless of which sequencer is
/// downstream.
///
/// The reset input gets a **refractory window** on top of the kit's hysteresis.
/// Hysteresis alone already rejects a clock hovering at the threshold, but a
/// reset jack is often driven by a physical switch or a long cable, where the
/// signal genuinely crosses the window several times in a millisecond. A
/// refractory window is the debounce for that, and it lives here rather than in
/// `TriggerDetectT` because a *clock* must never be debounced — a debounced
/// clock silently drops fast subdivisions.
///
/// RT contract: as the header. No allocation, no locks.
template <typename SampleType = float>
class TransportEdgeT {
public:
    /// Debounce window after a reset edge, during which further reset edges are
    /// suppressed. Purely mechanical — long enough to swallow switch bounce,
    /// far shorter than any musical reset interval.
    /// [design parameter] default 0.5 ms, range 0.1 .. 5 ms.
    static constexpr double kRefractoryMs = 0.5;

    /// One sample of transport, decoded.
    struct Frame {
        bool run_high = false;
        bool reset_edge = false;
        bool clock_edge = false;
    };

    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0) sample_rate_ = sample_rate;
        update();
    }

    /// Upper bound on the refractory window. Two orders of magnitude past the
    /// documented range, purely so the sample-count conversion below cannot
    /// overflow on a nonsense value from a host.
    /// [design parameter] default 1000 ms ceiling, range 10 .. 60000 ms.
    static constexpr double kMaxRefractoryMs = 1000.0;

    void set_refractory_ms(double ms) {
        refractory_ms_ = seq_detail::clamp_finite(ms, 0.0, kMaxRefractoryMs);
        update();
    }

    /// Sets the hysteresis window used for all three inputs, so a patch running
    /// at ±5 V and one running at 0/1 configure in one call.
    void set_thresholds(double high, double low) {
        run_.set_thresholds(high, low);
        reset_.set_thresholds(high, low);
        clock_.set_thresholds(high, low);
    }

    void reset() {
        run_.reset();
        reset_.reset();
        clock_.reset();
        refractory_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// Decodes run, reset and clock levels for one sample.
    Frame process(SampleType run, SampleType reset_in, SampleType clock) {
        Frame f{};
        run_.process(run);
        f.run_high = run_.high();

        const bool raw_reset = reset_.process(reset_in);
        if (refractory_ > 0) --refractory_;
        if (raw_reset && refractory_ == 0) {
            f.reset_edge = true;
            refractory_ = refractory_samples_;
        }

        f.clock_edge = clock_.process(clock);
        return f;
    }

    /// Run and reset only, for a caller whose clock edge already arrives as a
    /// boolean (a `ClockDividerT` output, say).
    Frame process(SampleType run, SampleType reset_in) {
        return process(run, reset_in, SampleType{0});
    }

private:
    void update() {
        refractory_samples_ = static_cast<std::int64_t>(
            std::llround(units::ms_to_samples(refractory_ms_, sample_rate_)));
        if (refractory_samples_ < 0) refractory_samples_ = 0;
    }

    double sample_rate_ = 44100.0;
    double refractory_ms_ = kRefractoryMs;
    std::int64_t refractory_samples_ = 0;
    std::int64_t refractory_ = 0;
    TriggerDetectT<SampleType> run_{};
    TriggerDetectT<SampleType> reset_{};
    TriggerDetectT<SampleType> clock_{};
};

using TransportEdge = TransportEdgeT<float>;
using TransportEdge64 = TransportEdgeT<double>;

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

// ── Cartesian (2-D) sequencer ─────────────────────────────────────────────

/// How a `CartesianWalkT`'s two axes relate.
enum class CartesianAccess {
    /// X advances on its clock; an X wrap carries into Y, like reading text.
    /// The Y clock input is ignored — carrying *and* clocking Y would make the
    /// row advance twice per wrap.
    row_major,
    /// X and Y advance only on their own clocks and never on each other's. The
    /// documented René behaviour and the default.
    independent,
};

/// The 2-D (Cartesian) sequencer.
///
/// *(lineage — informative)* **Make Noise René** (2009) is documented as "the
/// world's first Cartesian sequencer for music synthesizers": sixteen locations
/// in a **4×4 grid** with the **X and Y axes clocked independently**, plus CV
/// offset inputs added to each axis counter — "a control signal at the X-CV
/// input… is added to the number generated by the X-Axis counter to create the
/// X coordinate" [Make Noise, *René Manual*]. Cited for concept and topology
/// only; the 8×8 cap is ours.
///
/// Two independent clocks over a grid is a genuinely different topology from a
/// linear sequencer, and the reason is arithmetic: clock only X on a 4×4 and you
/// loop four values; add a Y clock at four times the period and the sequence is
/// sixteen; make the two periods coprime and the walk visits cells in an order
/// with a super-cycle of `lcm` of the two — long sequences out of two cheap
/// dividers and no memory.
///
/// **The gate is a one-sample trigger**, not a level, and fires on any clock
/// edge that changes the output coordinate (a simultaneous X and Y edge is one
/// gate). This block has no gate modes to give a gate a length, so inventing one
/// would be inventing a tempo; a caller who wants a gate of a stated length puts
/// a `GateGenT` (kit) downstream, which is also where a gate length belongs.
///
/// **USE.** *Evolving pads* — slow Y, fast X: the melody re-colours row by row.
/// *Two-hand performance* — drive X-clock and Y-clock from two `ClockDividerT`
/// (kit) taps at different divisions for hands-free polyrhythm. *Chord tables* —
/// a chord's notes across each row: walk X for arpeggios, tap Y to change chord,
/// with a `QuantizeScaleT` downstream to keep arbitrary grid values in key.
///
/// RT contract: as the header.
template <typename SampleType = float>
class CartesianWalkT {
public:
    /// Grid cap. 4×4 is the cited hardware; 8×8 is ours.
    /// [design parameter] default 4×4, range 1×1 .. 8×8.
    static constexpr int kMaxDim = 8;
    static constexpr int kDefaultDim = 4;

    /// Bound on an axis offset. Any offset is equivalent to itself modulo the
    /// axis length, so a bound loses nothing musically; it exists so
    /// `counter + offset` cannot overflow on a runaway CV.
    /// [design parameter] default ±1,000,000, range ±(kMaxDim) .. ±2^24.
    static constexpr int kMaxOffset = 1000000;

    struct Frame {
        SampleType cv = SampleType{0};
        bool gate = false;
    };

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
    }

    void set_size(int w, int h) {
        width_ = std::clamp(w, 1, kMaxDim);
        height_ = std::clamp(h, 1, kMaxDim);
        if (x_ >= width_) x_ = 0;
        if (y_ >= height_) y_ = 0;
    }
    int width() const { return width_; }
    int height() const { return height_; }

    void set_value(int x, int y, SampleType volts) {
        if (x >= 0 && x < kMaxDim && y >= 0 && y < kMaxDim &&
            std::isfinite(static_cast<double>(volts)))
            value_[y][x] = volts;
    }
    SampleType value(int x, int y) const {
        return (x >= 0 && x < kMaxDim && y >= 0 && y < kMaxDim) ? value_[y][x] : SampleType{0};
    }

    void set_access(CartesianAccess mode) { access_ = mode; }
    CartesianAccess access() const { return access_; }

    /// Integer CV offsets added to each axis counter. Read at clock time only,
    /// never continuously — a continuously-read offset would zipper the output
    /// between cells whenever the offset crossed an integer.
    void set_offsets(int x_cv, int y_cv) {
        x_offset_ = std::clamp(x_cv, -kMaxOffset, kMaxOffset);
        y_offset_ = std::clamp(y_cv, -kMaxOffset, kMaxOffset);
    }

    /// Verb 2: both counters home, gate low, output CV **held**. The next clock
    /// lands *on* the home cell rather than one past it, so "reset then run"
    /// plays from the top on both sequencers in this header.
    void apply_reset_edge() {
        x_ = 0;
        y_ = 0;
        cell_x_ = 0;
        cell_y_ = 0;
        started_ = false;
        gate_ = false;
    }

    void reset() {
        apply_reset_edge();
        held_ = SampleType{0};
    }

    static constexpr int latency_samples() { return 0; }

    int x() const { return x_; }
    int y() const { return y_; }
    int cell_x() const { return cell_x_; }
    int cell_y() const { return cell_y_; }
    bool gate() const { return gate_; }
    SampleType cv() const { return held_; }

    Frame process(bool run_high, bool reset_edge, bool x_clock_edge, bool y_clock_edge) {
        if (reset_edge) apply_reset_edge();

        if (!run_high) {
            gate_ = false;
            return Frame{held_, false};
        }

        gate_ = false;
        if (x_clock_edge || y_clock_edge) {
            const int prev_x = cell_x_;
            const int prev_y = cell_y_;

            if (!started_) {
                // The downbeat: land on the home cell without advancing.
                started_ = true;
                gate_ = true;
            } else {
                if (access_ == CartesianAccess::row_major) {
                    if (x_clock_edge) {
                        x_ = (x_ + 1) % width_;
                        if (x_ == 0) y_ = (y_ + 1) % height_;
                    }
                } else {
                    if (x_clock_edge) x_ = (x_ + 1) % width_;
                    if (y_clock_edge) y_ = (y_ + 1) % height_;
                }
            }

            cell_x_ = wrap(x_ + x_offset_, width_);
            cell_y_ = wrap(y_ + y_offset_, height_);
            held_ = value_[cell_y_][cell_x_];
            if (cell_x_ != prev_x || cell_y_ != prev_y) gate_ = true;
        }

        return Frame{held_, gate_};
    }

private:
    static int wrap(int v, int m) {
        const int r = v % m;
        return r < 0 ? r + m : r;
    }

    double sample_rate_ = 44100.0;
    int width_ = kDefaultDim;
    int height_ = kDefaultDim;
    CartesianAccess access_ = CartesianAccess::independent;
    int x_offset_ = 0;
    int y_offset_ = 0;
    SampleType value_[kMaxDim][kMaxDim]{};

    int x_ = 0;
    int y_ = 0;
    int cell_x_ = 0;
    int cell_y_ = 0;
    bool started_ = false;
    bool gate_ = false;
    SampleType held_ = SampleType{0};
};

using CartesianWalk = CartesianWalkT<float>;
using CartesianWalk64 = CartesianWalkT<double>;

// ── Rungler ───────────────────────────────────────────────────────────────

/// The Hordijk shift-register / DAC chaos sequencer.
///
/// *(lineage — informative)* The **rungler** is Rob Hordijk's signature circuit
/// from the Blippoo Box: an N-stage shift register whose serial input is fed
/// back from the register itself, clocked by an oscillator, with a few register
/// bits driving a small **DAC** whose stepped voltage modulates the oscillators
/// — a loop that produces smooth, quasi-periodic chaos, roughly predictable yet
/// never exactly repeating [Rob Hordijk, "The Blippoo Box: A Chaotic Electronic
/// Music Instrument, Bent by Design," *Leonardo Music Journal* 19 (2009),
/// 35–43, MIT Press]. The *topology* is cited; the **specific tap positions are
/// a design parameter**, because the paper describes the concept rather than one
/// canonical wiring and no citable exact tap map exists. Those are our
/// engineering choices, not cited constants.
///
/// **Why it sounds "smoothly chaotic."** With pure self-feedback the register is
/// a finite-state machine over `2^N` states, so the output is strictly
/// *periodic* — but the period can be very long and the orbit visits its DAC
/// levels in an order with no small repeating unit, which the ear reads as
/// evolving, roughly predictable, never quite looping. XOR-ing an external data
/// bit perturbs the state each clock, lengthening and reshaping the orbit, so a
/// slowly changing input voltage *steers* the chaos without ever making it
/// random. The compositional value is deterministic long-form variation:
/// controllable, but not memorizable.
///
/// **The bound is provable, not measured** (series law 1 + law 8). The only
/// feedback is a 1-bit XOR into the register; there is no gain-carrying analog
/// nonlinearity, so no small-signal-gain compensation applies. The output is a
/// `D`-bit DAC code mapped affinely onto `[−range_v, +range_v]`, so
/// `|y| ≤ range_v` **by construction** for any clock or data sequence
/// whatsoever. That is the invariant a registry `worst_case_gain` cites, and the
/// suite asserts it over adversarial data rather than typical data.
///
/// **No oversampling, no smoothing.** The stepped hold *is* the sound. There is
/// no continuous nonlinearity here to alias, so band-limiting would only remove
/// the intended edge; a caller who wants the steps softened puts a
/// `SlewLimiterT` downstream (the "drunken glide" patch).
///
/// **The all-zero register is an absorbing state.** With XOR feedback, `0 ⊕ 0`
/// is 0 forever, so a seed pattern of 0 produces a constant `−range_v`. That is
/// left as-is rather than remapped the way `Xorshift32` remaps a zero seed: the
/// seed pattern is a musical parameter whose neighbours must stay meaningful,
/// and silently substituting a different pattern for one value would make it
/// non-monotone. Enabling `external_data` kicks the register out of the state on
/// the first `data_in` bit that is 1.
///
/// **USE.** *Generative pitch source* — rungler CV → `QuantizeScaleT` →
/// oscillator: the archetypal self-playing patch, always in key, never quite
/// repeating. *Timbral chaos* — into a filter cutoff or a wavefolder depth for
/// burbling, semi-predictable motion. *Cross-modulation* — clock the rungler
/// from an oscillator whose pitch the rungler modulates (caller-wired) for the
/// Blippoo's audio-rate loop. *Tamed* — into a `SlewLimiterT` for a wandering
/// portamento line.
///
/// RT contract: as the header. Integer bit operations only.
template <typename SampleType = float>
class RunglerT {
public:
    /// Shift-register length.
    /// [design parameter] default 8, range 4 .. 16.
    static constexpr int kMinBits = 4;
    static constexpr int kMaxBits = 16;
    static constexpr int kDefaultBits = 8;

    /// DAC width — `2^D` output levels. D = 3 gives the classic eight steps.
    /// [design parameter] default 3, range 1 .. 4.
    static constexpr int kMinDacBits = 1;
    static constexpr int kMaxDacBits = 4;
    static constexpr int kDefaultDacBits = 3;

    /// Initial register pattern, restored by `reset()` *and* by a reset edge.
    /// [design parameter] default 0b10110100 (180), range 0 .. 65535.
    static constexpr std::uint32_t kSeedPattern = 0b10110100u;

    /// Register index XOR-ed with the last stage to form the serial input.
    /// [design parameter] default 0, range 0 .. N − 2.
    static constexpr int kFeedbackTap = 0;

    /// Output span in volts: the DAC covers `[−range_v, +range_v]`.
    /// [design parameter] default 2 V, range 0.5 .. 5 V.
    static constexpr double kRangeV = 2.0;

    /// Ceiling on the output span. Two orders past the documented range — no
    /// modular standard exceeds ±15 V — so it only catches a nonsense value.
    /// The bound is what makes `|y| <= range_v` a FINITE guarantee.
    /// [design parameter] default 1000 V ceiling, range 15 .. 100000 V.
    static constexpr double kMaxRangeV = 1000.0;

    RunglerT() { load_seed(); }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
    }

    /// Changing the register length reloads the seed pattern. A shift register
    /// whose length changes mid-orbit has no meaningful "same state" to keep —
    /// truncating drops bits and extending invents them — so the deterministic
    /// answer is to start the new length from the seed rather than from a
    /// silently reinterpreted bit pattern.
    void set_reg_bits(int n) {
        bits_ = std::clamp(n, kMinBits, kMaxBits);
        clamp_config();
        load_seed();
    }
    int reg_bits() const { return bits_; }

    void set_dac_bits(int d) {
        dac_bits_ = std::clamp(d, kMinDacBits, kMaxDacBits);
        clamp_config();
        refresh_output();
    }
    int dac_bits() const { return dac_bits_; }

    void set_feedback_tap(int tap) {
        tap_ = tap;
        clamp_config();
    }
    int feedback_tap() const { return tap_; }

    void set_range_v(double volts) {
        range_v_ = seq_detail::clamp_finite(volts, 0.0, kMaxRangeV);
        refresh_output();
    }
    double range_v() const { return range_v_; }

    void set_external_data(bool on) { external_data_ = on; }
    bool external_data() const { return external_data_; }

    void set_seed_pattern(std::uint32_t pattern) {
        seed_pattern_ = pattern;
        load_seed();
    }
    std::uint32_t seed_pattern() const { return seed_pattern_; }

    /// Verb 2 — and the one documented exception to "continuous outputs hold":
    /// the register goes back to the seed pattern and the output steps to
    /// `DAC(seed)` immediately, so a wandering line can be re-pinned live.
    void apply_reset_edge() { load_seed(); }

    void reset() { load_seed(); }

    static constexpr int latency_samples() { return 0; }

    /// Current register contents, bit `i` at bit position `i`.
    std::uint32_t register_bits() const { return reg_; }
    /// Current DAC code in `0 .. 2^D − 1`.
    int dac_code() const { return static_cast<int>(reg_ & dac_mask()); }
    SampleType value() const { return out_; }

    /// Advances one sample. `data_in` is XOR-ed into the serial input when
    /// `external_data` is enabled — the paper's "bent by design" input, most
    /// naturally driven from a `ComparatorT` (kit) on any signal in the patch.
    SampleType process(bool run_high, bool reset_edge, bool clock_edge, bool data_in = false) {
        if (reset_edge) apply_reset_edge();
        if (!run_high) return out_;
        if (!clock_edge) return out_;

        const std::uint32_t last = (reg_ >> (bits_ - 1)) & 1u;
        const std::uint32_t tap = (reg_ >> tap_) & 1u;
        std::uint32_t new_bit = last ^ tap;
        if (external_data_ && data_in) new_bit ^= 1u;

        reg_ = ((reg_ << 1) | new_bit) & reg_mask();
        refresh_output();
        return out_;
    }

private:
    std::uint32_t reg_mask() const { return (1u << bits_) - 1u; }
    std::uint32_t dac_mask() const { return (1u << dac_bits_) - 1u; }

    void clamp_config() {
        if (dac_bits_ > bits_) dac_bits_ = bits_;
        tap_ = std::clamp(tap_, 0, std::max(bits_ - 2, 0));
    }

    void load_seed() {
        clamp_config();
        reg_ = seed_pattern_ & reg_mask();
        refresh_output();
    }

    void refresh_output() {
        const double levels = static_cast<double>(dac_mask());
        const double k = static_cast<double>(reg_ & dac_mask());
        const double unit = levels > 0.0 ? (2.0 * k / levels - 1.0) : 0.0;
        out_ = static_cast<SampleType>(range_v_ * unit);
    }

    double sample_rate_ = 44100.0;
    int bits_ = kDefaultBits;
    int dac_bits_ = kDefaultDacBits;
    int tap_ = kFeedbackTap;
    double range_v_ = kRangeV;
    bool external_data_ = false;
    std::uint32_t seed_pattern_ = kSeedPattern;

    std::uint32_t reg_ = 0;
    SampleType out_ = SampleType{0};
};

using Rungler = RunglerT<float>;
using Rungler64 = RunglerT<double>;

// ── Quantizer ─────────────────────────────────────────────────────────────

/// How a `QuantizeScaleT` divides the octave.
enum class QuantizeMode {
    edo,         ///< `N` equal steps per octave.
    scale_mask,  ///< 12-TET, then snapped to a 12-bit pitch-class mask.
};

/// CV-to-scale quantizer on the 1 V/octave standard.
///
/// *(informative)* In 12-tone equal temperament the octave is twelve equal
/// semitones of 100 cents, so on the 1 V/oct standard a pitch plays in tune when
/// it is a multiple of 1/12 V — "steps of exactly 1/12 V = one semitone"
/// [illustrated by public quantizer documentation, e.g. Doepfer A-156; 12-TET
/// itself is common practice, cited for illustration only]. The pitch-class
/// bitmask convention is shared verbatim with the MIDI series' scale lock, so a
/// scale authored on either side means the same thing here.
///
/// **Hysteresis is the load-bearing part.** A quantizer without it chatters
/// whenever its input hovers on a step boundary — an LFO peak, a slow envelope,
/// a noisy CV — and the chatter is audible as a trill, not as jitter. A step
/// change therefore requires the input to cross the boundary by
/// `hyst_cents`, and the latch is the block's only state.
///
/// The latch holds the **chromatic** (pre-mask) step rather than the snapped
/// output. That is what makes the hysteresis window a fixed number of cents of
/// *input* travel: if it latched the output, the window would silently widen and
/// narrow with the gaps in the scale, so a semitone-wide leading tone and a
/// minor-third gap would debounce differently.
///
/// **Glide is not baked in.** The output is a stepped voltage; a caller wanting
/// smooth transitions puts a `SlewLimiterT` (kit) downstream, which keeps this
/// block pure and lets the same quantizer feed a glide and a hard-stepped path.
///
/// **USE.** *Taming the rungler or the Cartesian walk* — any wild CV becomes an
/// in-key melody. *Sample-and-hold pitch* — a `SampleHoldT` (kit) on an LFO into
/// the quantizer into an oscillator is the classic random arpeggio.
/// *Microtonal* — EDO-N mode reaches 24-tone quarter tones and 19- or 31-tone
/// tunings without a lookup table.
///
/// RT contract: as the header. One `llround`-free floor and a bounded search.
template <typename SampleType = float>
class QuantizeScaleT {
public:
    /// Steps per octave in `edo` mode. 12 is 12-TET (cited); the cap is ours.
    /// [design parameter] default 12, range 1 .. 48.
    static constexpr int kDefaultEdo = 12;
    static constexpr int kMaxEdo = 48;

    /// Pitch classes of the major scale — bits 0, 2, 4, 5, 7, 9, 11.
    static constexpr std::uint16_t kMajorMask = 0b0000'1010'1011'0101u;

    /// Extra travel, in cents, an input must make past a step boundary before
    /// the output follows it.
    /// [design parameter] default 20 cents, range 0 .. 50 cents.
    static constexpr double kHystCents = 20.0;

    /// Ceiling on the hysteresis setting, one octave. Beyond a full octave the
    /// value is meaningless anyway, since `kMaxHystSteps` caps the window that
    /// actually applies; this bound exists so the arithmetic stays finite.
    /// [design parameter] default 1200 cents ceiling, range 100 .. 12000 cents.
    static constexpr double kMaxHystCents = 1200.0;

    /// Ceiling on the hysteresis window expressed as a fraction of one step.
    ///
    /// A window in *cents* and a step count are independent knobs, and above
    /// EDO-30 the shipped 20-cent default is wider than the half-step boundary
    /// it debounces: one EDO-31 step is 38.71 cents, the boundary sits 19.35
    /// cents away, and 19.35 + 20 = 39.35 cents of required travel exceeds the
    /// 38.71 cents an adjacent step is away. The quantizer would then be unable
    /// to reach the next step *at all* and would lag a monotone input by one
    /// step forever — not anti-chatter, a stuck output. The window is therefore
    /// capped so an adjacent step always stays reachable with margin: required
    /// travel is `0.5 + kMaxHystSteps` = 0.95 of a step, leaving 5 % of a step
    /// of headroom against float noise at the comparison.
    /// [design parameter] default 0.45 step, range 0.1 .. 0.499 step.
    static constexpr double kMaxHystSteps = 0.45;

    /// Widest step index the quantizer will produce, in either direction.
    ///
    /// This is the block that exists to tame *wild* CV — a rungler, a Cartesian
    /// walk, a runaway envelope — so it is the one place in the header that has
    /// to survive an input no musician would send. Casting an unbounded or
    /// non-finite `double` to `int` is undefined behaviour, not a wrong note.
    /// The bound sits far past any real pitch: the widest EDO at ten volts
    /// (EDO-48 at ±10 V) reaches ±480 steps.
    /// [design parameter] default ±4096 steps, range ±512 .. ±2^20.
    static constexpr double kMaxAbsSteps = 4096.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
    }

    void set_mode(QuantizeMode mode) { mode_ = mode; }
    QuantizeMode mode() const { return mode_; }

    void set_edo(int n) { edo_ = std::clamp(n, 1, kMaxEdo); }
    int edo() const { return edo_; }

    void set_scale_mask(std::uint16_t mask) { mask_ = static_cast<std::uint16_t>(mask & 0x0FFFu); }
    std::uint16_t scale_mask() const { return mask_; }

    void set_root_pc(int pc) { root_pc_ = ((pc % 12) + 12) % 12; }
    int root_pc() const { return root_pc_; }

    void set_hysteresis_cents(double cents) {
        hyst_cents_ = seq_detail::clamp_finite(cents, 0.0, kMaxHystCents);
    }
    double hysteresis_cents() const { return hyst_cents_; }

    /// Verb 2: clears the hysteresis latch, so the next input quantizes on its
    /// own merits rather than being held by whatever the pattern left behind.
    void apply_reset_edge() { have_latch_ = false; }

    void reset() {
        have_latch_ = false;
        latched_step_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// The latched pre-mask step, in units of `1/N` octave (`edo` mode) or
    /// semitones (`scale_mask` mode).
    int latched_step() const { return latched_step_; }

    /// Quantizes one sample of pitch CV, in volts.
    SampleType process(SampleType cv_volts) {
        const int divisions = (mode_ == QuantizeMode::edo) ? edo_ : 12;
        const double in_steps = static_cast<double>(cv_volts) * static_cast<double>(divisions);
        const int step = latch(in_steps, divisions);
        const int out_step = (mode_ == QuantizeMode::edo) ? step : snap_to_mask(step);
        return static_cast<SampleType>(static_cast<double>(out_step) /
                                       static_cast<double>(divisions));
    }

private:
    /// Round half up, so 3.5 goes to 4 and −3.5 goes to −3.
    static int round_half_up(double x) {
        return static_cast<int>(
            std::floor(seq_detail::clamp_finite(x, -kMaxAbsSteps, kMaxAbsSteps) + 0.5));
    }

    int latch(double in_steps, int divisions) {
        const int candidate = round_half_up(in_steps);
        if (!have_latch_) {
            have_latch_ = true;
            latched_step_ = candidate;
            return latched_step_;
        }

        // One step spans 1200/divisions cents, so the window in steps is
        // `cents · divisions / 1200`. At 12-TET and the 20-cent default this is
        // 0.2 of a semitone, matching the spec's worked example. The cap only
        // engages above EDO-30 — see `kMaxHystSteps` for why it has to.
        const double window = std::min(hyst_cents_ * static_cast<double>(divisions) / 1200.0,
                                       kMaxHystSteps);
        const double here = static_cast<double>(latched_step_);
        if (candidate > latched_step_ && in_steps < here + 0.5 + window) return latched_step_;
        if (candidate < latched_step_ && in_steps > here - 0.5 - window) return latched_step_;

        latched_step_ = candidate;
        return latched_step_;
    }

    bool allowed(int semitone) const {
        int pc = (semitone - root_pc_) % 12;
        if (pc < 0) pc += 12;
        return ((mask_ >> pc) & 1u) != 0u;
    }

    int snap_to_mask(int semitone) const {
        // An empty mask has no enabled pitch class to snap to. Passing the
        // chromatic step through is the only answer that keeps the output in
        // tune; silently substituting a scale would hide the misconfiguration.
        if (mask_ == 0u) return semitone;
        for (int d = 0; d <= 12; ++d) {
            if (allowed(semitone + d)) return semitone + d;  // ties resolve upward
            if (d > 0 && allowed(semitone - d)) return semitone - d;
        }
        return semitone;
    }

    double sample_rate_ = 44100.0;
    QuantizeMode mode_ = QuantizeMode::scale_mask;
    int edo_ = kDefaultEdo;
    std::uint16_t mask_ = kMajorMask;
    int root_pc_ = 0;
    double hyst_cents_ = kHystCents;

    bool have_latch_ = false;
    int latched_step_ = 0;
};

using QuantizeScale = QuantizeScaleT<float>;
using QuantizeScale64 = QuantizeScaleT<double>;

// ── Gate combinators ──────────────────────────────────────────────────────

/// Boolean operation performed by a `GateLogicT`. Ordered to match the catalog
/// node's `op` parameter.
enum class GateOp {
    logic_and,
    logic_or,
    logic_xor,
    logic_nand,
    logic_nor,
    logic_xnor,
};

/// Combinational logic on gate signals.
///
/// Genuinely stateless — a transport reset is a no-op, because there is nothing
/// to reset. That is why the level-domain overload compares against a plain
/// threshold rather than running each input through a `TriggerDetectT`:
/// hysteresis is memory, and memory in a combinational block would make its
/// output depend on the order inputs happened to arrive in.
///
/// The N-input form applies the base operation across all inputs and inverts for
/// the negated variants — `nand` is `not (a and b and c…)`, `xnor` is the
/// complement of parity — rather than folding the two-input operation pairwise,
/// which for the negated ops would not be associative and would make the answer
/// depend on argument order.
///
/// **USE.** `and` two clocks for an only-on-coincidence gate; `xor` two divided
/// clocks for a composite rhythm busier than either; `or` to merge two gate
/// streams. Three `ClockDividerT` (kit) taps at ÷2/÷3/÷5 through a tree of these
/// is a dense polymetric pattern from one master clock and zero randomness.
///
/// RT contract: pure arithmetic, no state, no allocation.
template <typename SampleType = float>
class GateLogicT {
public:
    /// Level above which an input counts as high in the level-domain overload.
    /// Matches the kit's rising threshold so a gate that opens a
    /// `TriggerDetectT` also reads high here.
    /// [design parameter] default `kTriggerHighThreshold` (0.5), range 0.01 .. 2.5.
    static constexpr double kLevelThreshold = kTriggerHighThreshold;

    void prepare(double) {}

    void set_op(GateOp op) { op_ = op; }
    GateOp op() const { return op_; }

    void apply_reset_edge() {}
    void reset() {}

    static constexpr int latency_samples() { return 0; }

    bool process(bool a, bool b) const {
        switch (op_) {
            case GateOp::logic_and: return a && b;
            case GateOp::logic_or: return a || b;
            case GateOp::logic_xor: return a != b;
            case GateOp::logic_nand: return !(a && b);
            case GateOp::logic_nor: return !(a || b);
            case GateOp::logic_xnor: return a == b;
        }
        return false;
    }

    /// N-input form. An empty input list returns the operation's identity, so a
    /// tree that loses a branch degrades predictably instead of returning false
    /// for everything.
    bool process(const bool* gates, int count) const {
        bool all = true;
        bool any = false;
        bool parity = false;
        for (int i = 0; i < count; ++i) {
            all = all && gates[i];
            any = any || gates[i];
            parity = parity != gates[i];
        }
        switch (op_) {
            case GateOp::logic_and: return all;
            case GateOp::logic_or: return any;
            case GateOp::logic_xor: return parity;
            case GateOp::logic_nand: return !all;
            case GateOp::logic_nor: return !any;
            case GateOp::logic_xnor: return !parity;
        }
        return false;
    }

    /// Level-domain form: thresholds both inputs and returns 0 or 1.
    SampleType process_levels(SampleType a, SampleType b) const {
        const bool ha = static_cast<double>(a) >= kLevelThreshold;
        const bool hb = static_cast<double>(b) >= kLevelThreshold;
        return process(ha, hb) ? SampleType{1} : SampleType{0};
    }

private:
    GateOp op_ = GateOp::logic_and;
};

using GateLogic = GateLogicT<float>;
using GateLogic64 = GateLogicT<double>;

/// Passes each incoming trigger with a stated probability, deterministically.
///
/// The draw is a seeded `Xorshift32` (series law 2), so a probabilistic groove
/// can be auditioned, bounced and reloaded and give *the same* performance every
/// time. Changing the seed is how you roll again; the seed is a config value and
/// never an automation lane.
///
/// **A draw is consumed on every trigger regardless of `p`.** `p` gates the
/// *result*, never *whether we draw*. If a `p` of 1 skipped the draw, automating
/// `p` during a take would shift the stream position and every later decision
/// with it — the same pattern would render differently depending on the
/// automation that preceded it, which is exactly the reproducibility law 2
/// exists to protect.
///
/// A transport reset edge clears the edge-detector latch but **does not** rewind
/// the stream: a live reset jack that rewound randomness would make every reset
/// sound identical.
///
/// **USE.** *Generative drums* — on a hi-hat clock for humanized density;
/// automate `p` for a build-up that is still bit-reproducible. *Trigger
/// thinning* — drop hits from a `BurstGenT` (kit) ratchet for un-mechanical
/// rolls. *Sometimes-accent* — `GateLogicT(and)` of a probability gate with a
/// downbeat clock.
///
/// RT contract: as the header. One generator call per trigger.
template <typename SampleType = float>
class ProbGateT {
public:
    /// Pass probability.
    /// [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kDefaultProbability = 0.5;

    /// [design parameter] default 0x1234567, range 0 .. 2^31 − 1.
    static constexpr std::uint32_t kProbSeed = 0x1234567u;

    ProbGateT() { rng_.set_seed(kProbSeed); }

    void prepare(double) {}

    void set_probability(double p) {
        if (std::isfinite(p)) probability_ = std::clamp(p, 0.0, 1.0);
    }
    double probability() const { return probability_; }

    void set_seed(std::uint32_t seed) { rng_.set_seed(seed); }

    /// Verb 2: clears the edge-detector latch only. The RNG stream is untouched.
    void apply_reset_edge() { detector_.reset(); }

    /// Verb 1: clears the latch and rewinds the stream to the seed.
    void reset() {
        detector_.reset();
        rng_.reset();
        draws_ = 0;
    }

    static constexpr int latency_samples() { return 0; }

    /// Number of draws consumed since the last `reset()` — the stream position,
    /// exposed so a test can assert it advances once per trigger whatever `p` is.
    std::uint32_t draw_count() const { return draws_; }

    /// Decides one already-detected trigger edge.
    bool process_edge(bool trigger_edge) {
        if (!trigger_edge) return false;
        ++draws_;
        return rng_.next_unit<double>() < probability_;
    }

    /// Detects the edge from a trigger signal and decides it, so this block can
    /// sit directly on a clock output without a separate detector.
    bool process(SampleType trigger) { return process_edge(detector_.process(trigger)); }

private:
    double probability_ = kDefaultProbability;
    std::uint32_t draws_ = 0;
    Xorshift32 rng_{kProbSeed};
    TriggerDetectT<SampleType> detector_{};
};

using ProbGate = ProbGateT<float>;
using ProbGate64 = ProbGateT<double>;

}  // namespace pulp::signal
