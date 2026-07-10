#pragma once

// A multi-stage envelope generator, and the four signals a note can become.
//
// Three attack stages, a sustain, and two release stages. Each stage has its own
// target level, its own duration, and its own curve — and the familiar ADSR is a
// subset of that, not a different thing:
//
//     A → Time A2      D → Time A3      S → Sustain      R → Time R1
//
// A1 is the stage before the attack. Leave `Level A1` at zero and `Time A1` is a
// delay before the envelope starts, because a rise to zero from zero is a wait.
// R2 is the tail after the release, ending at zero.
//
// Everything else in this suite is a pure function of the host's position. This is
// not, and it cannot be: a note is an event, not a coordinate. What it gets instead
// is the next best thing — `envelope_at`, a pure function of the time since the note,
// which the running generator does not merely agree with but is *written in terms
// of*. The audio thread evaluates it; the editor draws it; there is no second
// implementation to drift.
//
// Times are in seconds. `Mult` scales all of them at once.

#include <brew/cv.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::examples::brew {

/// Where the envelope is. Ordered so `>= Stage::release_1` means "letting go".
enum class Stage : int {
    idle = 0,
    attack_1 = 1,
    attack_2 = 2,
    attack_3 = 3,
    sustain = 4,
    release_1 = 5,
    release_2 = 6,
};

inline constexpr int kAttackStages = 3;
inline constexpr int kReleaseStages = 2;

/// The steepest curve either shaping law will bend to. Past this a stage is a step
/// with a delay in front of it, and the control stops being useful before it stops
/// being defined.
inline constexpr double kMaxCurveExponent = 3.0;
inline constexpr double kMaxCurveRate = 6.0;

/// Normalized progress through a stage, bent.
///
/// Returns 0 at `p = 0` and 1 at `p = 1` under every setting, so a stage always
/// arrives exactly at its target. `curve = 0` is a straight line in both laws —
/// and it is a *bit-exact* straight line, not a limit approached, because a curve
/// knob at rest must not colour the sound.
///
/// Positive bends the rise toward its target early (fast attack, slow tail);
/// negative holds it back. The two laws differ in how hard: `parabolic` is a power
/// of `p`, `exponential` is the charging curve of an RC network, which is what a
/// hardware envelope's capacitor actually does.
[[nodiscard]] inline float curve_shape(double p, float curve, bool exponential) noexcept {
    const double x = std::clamp(p, 0.0, 1.0);
    const double c = std::clamp(static_cast<double>(curve), -1.0, 1.0);
    if (c == 0.0) return static_cast<float>(x);
    if (!exponential) {
        // `c = +1` → x^(1/8): most of the travel in the first eighth of the stage.
        return static_cast<float>(std::pow(x, std::pow(2.0, -c * kMaxCurveExponent)));
    }
    const double a = c * kMaxCurveRate;
    return static_cast<float>((1.0 - std::exp(-a * x)) / (1.0 - std::exp(-a)));
}

/// One stage: where it ends up, how long it takes, and how it gets there.
///
/// `attack_3` ends at the sustain level and `release_2` ends at zero, so those two
/// carry no target of their own — the envelope supplies it. Storing a target they
/// would ignore is how a control ends up on a panel doing nothing.
struct StageSpec {
    float target = 0.0f;
    double seconds = 0.0;
    float curve = 0.0f;
};

/// The whole shape.
struct EnvelopeSpec {
    /// A1 (its own target), A2 (the peak), A3 (falls to `sustain`).
    std::array<StageSpec, kAttackStages> attack{};
    float sustain = 1.0f;
    /// R1 (its own target), R2 (falls to zero).
    std::array<StageSpec, kReleaseStages> release{};
    /// Parabolic, or the RC network's charging curve.
    bool exponential = false;
    /// Whether a retrigger snaps back to zero or continues from where it was.
    bool reset_to_zero = true;
    /// Scales every time at once.
    double time_multiplier = 1.0;

    /// A stage's duration after `Mult`, never negative.
    [[nodiscard]] double stage_seconds(Stage s) const noexcept {
        const double raw = [&] {
            switch (s) {
                case Stage::attack_1: return attack[0].seconds;
                case Stage::attack_2: return attack[1].seconds;
                case Stage::attack_3: return attack[2].seconds;
                case Stage::release_1: return release[0].seconds;
                case Stage::release_2: return release[1].seconds;
                default: return 0.0;
            }
        }();
        const double t = raw * time_multiplier;
        return t > 0.0 ? t : 0.0;
    }

    /// Where a stage ends. The two that have no target of their own read one.
    [[nodiscard]] float stage_target(Stage s) const noexcept {
        switch (s) {
            case Stage::attack_1: return attack[0].target;
            case Stage::attack_2: return attack[1].target;
            case Stage::attack_3: return sustain;
            case Stage::sustain: return sustain;
            case Stage::release_1: return release[0].target;
            case Stage::release_2: return 0.0f;
            case Stage::idle: return 0.0f;
        }
        return 0.0f;
    }

    [[nodiscard]] float stage_curve(Stage s) const noexcept {
        switch (s) {
            case Stage::attack_1: return attack[0].curve;
            case Stage::attack_2: return attack[1].curve;
            case Stage::attack_3: return attack[2].curve;
            case Stage::release_1: return release[0].curve;
            case Stage::release_2: return release[1].curve;
            default: return 0.0f;
        }
    }
};

/// Whether a stage does anything at all.
///
/// A stage with no duration reaches its target instantly — an attack time of zero
/// is a jump to the peak, which is what every synth does and what a user setting it
/// to zero is asking for. A1 is the one exception: with no duration *and* no level
/// it is not a stage, it is the absence of a delay, and letting it stamp a zero over
/// the value a retrigger started from would leave `RTZ` a control with no off
/// position.
[[nodiscard]] inline bool stage_is_absent(const EnvelopeSpec& spec, Stage s) noexcept {
    return s == Stage::attack_1 && !(spec.stage_seconds(s) > 0.0) &&
           spec.stage_target(s) == 0.0f;
}

/// The envelope's level `seconds` after a note-on that is still held, having begun
/// from `from`.
///
/// Pure, and — because the running generator below is written in terms of it — not
/// merely *tested* against what the DSP does but *identical* to it. The editor draws
/// this function; the audio thread evaluates this function; there is no second
/// implementation to drift.
[[nodiscard]] inline float envelope_at(const EnvelopeSpec& spec, double seconds,
                                       float from = 0.0f) noexcept {
    double t = seconds > 0.0 ? seconds : 0.0;
    float start = from;
    for (Stage s : {Stage::attack_1, Stage::attack_2, Stage::attack_3}) {
        if (stage_is_absent(spec, s)) continue;
        const double len = spec.stage_seconds(s);
        const float target = spec.stage_target(s);
        if (len > 0.0 && t < len) {
            const float k = curve_shape(t / len, spec.stage_curve(s), spec.exponential);
            return start + (target - start) * k;
        }
        t -= len;
        start = target;
    }
    return spec.sustain;
}

/// The envelope's level `seconds` after a note-off that let go from `from`.
[[nodiscard]] inline float release_at(const EnvelopeSpec& spec, double seconds,
                                      float from) noexcept {
    double t = seconds > 0.0 ? seconds : 0.0;
    float start = from;
    for (Stage s : {Stage::release_1, Stage::release_2}) {
        const double len = spec.stage_seconds(s);
        const float target = spec.stage_target(s);
        if (len > 0.0 && t < len) {
            const float k = curve_shape(t / len, spec.stage_curve(s), spec.exponential);
            return start + (target - start) * k;
        }
        t -= len;
        start = target;
    }
    return 0.0f;
}

/// How long a note-on takes to reach the sustain, after `Mult`.
[[nodiscard]] inline double attack_seconds(const EnvelopeSpec& spec) noexcept {
    return spec.stage_seconds(Stage::attack_1) + spec.stage_seconds(Stage::attack_2) +
           spec.stage_seconds(Stage::attack_3);
}

/// How long a note-off takes to reach zero, after `Mult`.
[[nodiscard]] inline double release_seconds(const EnvelopeSpec& spec) noexcept {
    return spec.stage_seconds(Stage::release_1) + spec.stage_seconds(Stage::release_2);
}

/// Which stage `seconds` after the note-on (or the note-off) falls in.
[[nodiscard]] inline Stage stage_at(const EnvelopeSpec& spec, double seconds,
                                    bool releasing) noexcept {
    const double t = seconds > 0.0 ? seconds : 0.0;
    if (releasing) {
        return t < spec.stage_seconds(Stage::release_1) ? Stage::release_1
                                                        : Stage::release_2;
    }
    double edge = 0.0;
    for (Stage s : {Stage::attack_1, Stage::attack_2, Stage::attack_3}) {
        if (stage_is_absent(spec, s)) continue;
        edge += spec.stage_seconds(s);
        if (t < edge) return s;
    }
    return Stage::sustain;
}

/// The running envelope. One per voice.
///
/// Its whole state is: how many samples since the last event, what value the
/// envelope began that event from, and whether the note is still down. Everything
/// else it reads out of `envelope_at` and `release_at`, which is why the curve in
/// the editor is not a picture *of* the signal but the signal itself, sampled.
///
/// Counting samples rather than accumulating a phase is what buys that. A phase
/// advanced by `1/(len * sr)` crosses its stage boundary a sample early or late
/// depending on how the division rounded, and a curve is at its steepest exactly
/// there — so a shape drawn from a formula and a shape rendered from an accumulator
/// disagree visibly at every knee.
class Envelope {
public:
    void reset() noexcept {
        samples_ = 0;
        time_ = 0.0;
        from_ = 0.0f;
        value_ = 0.0f;
        active_ = false;
        releasing_ = false;
    }

    /// Retrigger. `reset_to_zero` decides whether the rise starts from silence or
    /// from wherever the last note left it — the difference between a plucked
    /// string and a bowed one.
    void note_on(const EnvelopeSpec& spec) noexcept {
        from_ = spec.reset_to_zero ? 0.0f : value_;
        value_ = from_;
        samples_ = 0;
        time_ = 0.0;
        active_ = true;
        releasing_ = false;
    }

    /// Let go from wherever the envelope currently is, which is not always the
    /// sustain: a note shorter than its own attack releases from halfway up it.
    void note_off() noexcept {
        if (!active_) return;
        from_ = value_;
        samples_ = 0;
        time_ = 0.0;
        releasing_ = true;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool releasing() const noexcept { return releasing_; }
    [[nodiscard]] float value() const noexcept { return value_; }

    /// Where the envelope is. Needs the spec, because a stage's length is a
    /// parameter and the envelope stores none of them.
    [[nodiscard]] Stage stage(const EnvelopeSpec& spec) const noexcept {
        if (!active_) return Stage::idle;
        return stage_at(spec, time_, releasing_);
    }

    /// Advance one sample and return the new level.
    [[nodiscard]] float process(const EnvelopeSpec& spec, double sample_rate) noexcept {
        if (!active_) return value_ = 0.0f;
        if (!(sample_rate > 0.0)) return value_;
        time_ = static_cast<double>(samples_) / sample_rate;

        if (releasing_) {
            // Checked before the evaluation, so an all-zero release lands on zero on
            // the sample the note lifts rather than holding its last level for one.
            if (time_ >= release_seconds(spec)) {
                active_ = false;
                return value_ = 0.0f;
            }
            value_ = release_at(spec, time_, from_);
        } else {
            value_ = envelope_at(spec, time_, from_);
        }
        ++samples_;
        return value_;
    }

private:
    /// Samples since the last note-on or note-off. An integer, so the stage
    /// boundaries land where the arithmetic says and not one sample either side.
    std::int64_t samples_ = 0;
    double time_ = 0.0;
    float from_ = 0.0f;
    float value_ = 0.0f;
    bool active_ = false;
    bool releasing_ = false;
};

// ── Velocity, and the jack ───────────────────────────────────────────────────

/// How much a note's velocity scales the envelope.
///
/// `0` ignores velocity entirely, `+1` hands it the whole scale, and `-1` inverts
/// it — a soft note opens the filter and a hard one closes it. Continuous across
/// zero, and exactly 1.0 there, so an untouched knob is a wire.
[[nodiscard]] inline float velocity_gain(float amount, float velocity) noexcept {
    const float a = std::clamp(amount, -1.0f, 1.0f);
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    const float depth = std::abs(a);
    return (1.0f - depth) + depth * (a > 0.0f ? v : 1.0f - v);
}

/// Map a unipolar signal into the voltage window the jack is set to.
///
/// `low` above `high` is not an error and not clamped away: it is how an envelope
/// is inverted, and it is the reason this plug-in has no `Invert` toggle.
[[nodiscard]] inline float map_voltage(float unipolar, float low, float high) noexcept {
    const float u = std::clamp(unipolar, 0.0f, 1.0f);
    return std::clamp(low + (high - low) * u, -kFullScale, kFullScale);
}

// ── The four signals a note can become ───────────────────────────────────────

/// What the plug-in puts on the jack. The same four the reference's voice
/// controller defines, and the enum order is the parameter's: append only.
enum class TriggerSignal : int {
    gate = 0,
    trigger = 1,
    envelope = 2,
    velocity = 3,
};

inline constexpr int kTriggerSignalCount = 4;

/// Whether the signal needs the envelope generator to run.
[[nodiscard]] inline constexpr bool signal_uses_envelope(TriggerSignal s) noexcept {
    return s == TriggerSignal::envelope;
}

/// A very short pulse, counted in samples, restarted by every note that sounds.
class Pulse {
public:
    void reset() noexcept { remaining_ = 0; }

    /// Restart. A zero or negative length is no pulse at all rather than an
    /// infinite one: `remaining_` is only ever *spent*, and a count that starts at
    /// or below zero is already spent.
    void fire(double seconds, double sample_rate) noexcept {
        remaining_ = static_cast<std::int64_t>(seconds * sample_rate);
    }

    /// Advance one sample. True while the pulse is high.
    [[nodiscard]] bool process() noexcept {
        if (remaining_ <= 0) return false;
        --remaining_;
        return true;
    }

    [[nodiscard]] bool high() const noexcept { return remaining_ > 0; }

private:
    std::int64_t remaining_ = 0;
};

}  // namespace pulp::examples::brew
