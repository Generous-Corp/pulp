#pragma once

// A looping shift register with a probabilistic feedback bit, and a weighted DAC
// reading a window of it.
//
// The classic circuit: a ring of bits clocks around once per step, and the bit
// that falls off the end is fed back to the front — sometimes inverted. How often
// it is inverted is the only control that matters, and it produces three regimes
// from one knob:
//
//   never invert   → the ring is a fixed loop of `length` steps. A locked pattern.
//   invert half    → maximum randomness. The loop never repeats.
//   always invert  → still fixed, but the ring must go round twice to come back:
//                    `2 × length` steps, the second half the complement of the
//                    first. A locked pattern, alternately inverted.
//
// None of that is coded. It falls out of one line — `p = (1 - randomness) / 2` —
// which is why the knob is signed: -1 and +1 are the two locked ends, 0 the free
// middle.
//
// The register drives a DAC: a weighted sum of a window of bits, normalized to
// [0, 1]. Widening the window from one bit to eight takes the output from a gate
// to a smooth-ish staircase, because each additional bit halves the step size.
//
// The whole thing has to stay a pure function of the absolute step index, for the
// same reason everything else in this suite does — bar 57 always plays step 3,
// and bouncing twice renders the same samples. So the register is *replayed* from
// the origin rather than advanced from wherever it happens to be, and the coin it
// flips at step k is a hash of k rather than draws from a generator. A cache makes
// the common case (the next step) O(1); a backwards locate rebuilds. See
// `ShiftRegister::at`.

#include <brew/random.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace pulp::examples::brew {

/// The ring's maximum size. Long enough that a pattern can outlive a section
/// without repeating; short enough to live in one `std::uint64_t`.
inline constexpr int kMaxRegisterBits = 48;

/// Bits the DAC can read. Eight gives 256 levels, which is where a staircase stops
/// looking like a staircase on a filter cutoff.
inline constexpr int kMaxDacBits = 8;

/// Separates the feedback coin from every other hash keyed on a step index.
inline constexpr std::uint32_t kRegisterSalt = 0x4F1BBCDCu;

/// How often the fed-back bit is inverted, from a signed randomness control.
///
/// `+1` never (a locked loop), `0` half the time (maximum entropy), `-1` always
/// (a locked loop of twice the length, alternately inverted). The three documented
/// endpoints are one linear map, not three special cases.
[[nodiscard]] inline constexpr double flip_probability(double randomness) noexcept {
    return (1.0 - std::clamp(randomness, -1.0, 1.0)) * 0.5;
}

/// Everything about the ring that a step's bits depend on. Changing any of it
/// changes the whole pattern, from the origin — which is the point: this is a
/// *pattern*, and a pattern that only changed from here on would be un-recallable.
struct RegisterSettings {
    /// Bits in the ring, 1..kMaxRegisterBits.
    int length = 8;
    /// Signed. See `flip_probability`.
    double randomness = 1.0;
    std::uint32_t seed = 0;
    /// While engaged, every bit shifted in is a one. A latch rather than a
    /// momentary button: automate it high for a single step and the two are the
    /// same operation, and a latch is the only form of it a preset can hold.
    bool set_next = false;

    friend constexpr bool operator==(const RegisterSettings&,
                                     const RegisterSettings&) = default;
};

/// Euclidean modulo over the ring. A negative rotate must land inside the ring,
/// not outside it: `%` would hand back a negative shift count, which is undefined.
[[nodiscard]] inline constexpr int wrap_bit(int i, int modulus) noexcept {
    if (modulus <= 0) return 0;
    const int r = i % modulus;
    return r < 0 ? r + modulus : r;
}

[[nodiscard]] inline constexpr int clamp_register_length(int n) noexcept {
    return std::clamp(n, 1, kMaxRegisterBits);
}

[[nodiscard]] inline constexpr std::uint64_t register_mask(int length) noexcept {
    const int n = clamp_register_length(length);
    return n >= 64 ? ~0ULL : ((1ULL << n) - 1ULL);
}

/// The ring at step 0: a hash of the seed, so two seeds start in different states
/// and the same seed always starts in the same one.
///
/// Never all-zero. An all-zero ring with `randomness = +1` never flips a bit and
/// so can never leave zero — a silent sequencer that looks broken. One forced bit
/// costs nothing and removes the trap.
[[nodiscard]] inline std::uint64_t initial_register(std::uint32_t seed,
                                                    int length) noexcept {
    const std::uint64_t bits = mix64(static_cast<std::uint64_t>(seed) ^ kRegisterSalt);
    return (bits | 1ULL) & register_mask(length);
}

/// Clock the ring once, for the transition *into* step `next_step`.
[[nodiscard]] inline std::uint64_t advance_register(std::uint64_t bits,
                                                    std::int64_t next_step,
                                                    const RegisterSettings& s) noexcept {
    const int n = clamp_register_length(s.length);
    const std::uint64_t mask = register_mask(n);
    const std::uint64_t out = (bits >> (n - 1)) & 1ULL;
    std::uint64_t in;
    if (s.set_next) {
        in = 1ULL;
    } else {
        const bool flip = static_cast<double>(hash_unit(next_step, s.seed ^ kRegisterSalt)) <
                          flip_probability(s.randomness);
        in = out ^ (flip ? 1ULL : 0ULL);
    }
    // A one-bit ring needs no special case: the shift moves its only bit out of
    // the mask, so what is left is the bit that just came back round.
    return ((bits << 1) | in) & mask;
}

/// The default weight of DAC bit `i`: a binary ladder, most significant first.
[[nodiscard]] inline constexpr float default_dac_weight(int i) noexcept {
    float w = 1.0f;
    for (int k = 0; k < i; ++k) w *= 0.5f;
    return w;
}

/// The DAC's window: which bits it reads, and how much each is worth.
struct DacSettings {
    /// Bits read, 1..kMaxDacBits.
    int bits = kMaxDacBits;
    /// Where in the ring the window starts, as one signed offset rather than a
    /// pair of nudge buttons — a button that can only be pressed while the editor
    /// is open cannot be automated, recalled, or tested.
    int rotate = 0;
    /// Per-bit weights, most significant first.
    std::array<float, kMaxDacBits> weights{};
    /// Divide by the sum of the weights in use (so full scale is always 1.0), or
    /// by `scale` (so a hand-set ladder keeps its own headroom).
    bool automatic_scale = true;
    float scale = 1.0f;

    static DacSettings binary() noexcept {
        DacSettings d;
        for (int i = 0; i < kMaxDacBits; ++i) d.weights[static_cast<std::size_t>(i)] = default_dac_weight(i);
        return d;
    }
};

/// Read the window as a unipolar level in [0, 1].
///
/// A negative or zero divisor would turn the DAC into a step change of sign at the
/// user's fingertip; it yields zero instead. Clamped, because a hand-set ladder can
/// sum past its own divisor and a control voltage has a ceiling.
[[nodiscard]] inline float dac_value(std::uint64_t bits, int register_length,
                                     const DacSettings& d) noexcept {
    const int n = clamp_register_length(register_length);
    const int count = std::clamp(d.bits, 1, kMaxDacBits);
    float sum = 0.0f;
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float w = d.weights[static_cast<std::size_t>(i)];
        total += w;
        const int pos = wrap_bit(d.rotate + i, n);
        if ((bits >> pos) & 1ULL) sum += w;
    }
    const float divisor = d.automatic_scale ? total : d.scale;
    if (!(divisor > 0.0f)) return 0.0f;
    return std::clamp(sum / divisor, 0.0f, 1.0f);
}

/// The ring, replayed from the origin, with a cache for the forward case.
///
/// `at(k)` is a pure function of `(k, settings)`. It is not `const`, because
/// keeping it pure at a usable cost means remembering where it was: stepping
/// forward one step is one shift, and only a settings change or a backwards
/// locate replays. A replay is `k` iterations of five integer ops — a couple of
/// hundred microseconds for an hour-long project, once, on the block after the
/// user drags the playhead backwards. It is the same trade the rest of the suite
/// makes: state exists to make a pure function cheap, never to define it.
class ShiftRegister {
public:
    void reset() noexcept {
        cached_step_ = -1;
        primed_ = false;
        has_prev_ = false;
    }

    /// The ring's bits at absolute step `step`. Steps before the origin replay
    /// from the origin too — the pattern simply has not started, and clamping is
    /// the only answer that does not run the ring backwards through a coin toss
    /// that has no inverse.
    [[nodiscard]] std::uint64_t at(std::int64_t step, const RegisterSettings& s) noexcept {
        const std::int64_t target = step < 0 ? 0 : step;
        // A glide reads step k-1 and step k on the same sample. Keeping one step of
        // history turns that alternation from a full replay per sample into two
        // pointer reads; without it the sequencer's cheapest setting is its most
        // expensive one.
        if (primed_ && settings_ == s && has_prev_ && target == cached_step_ - 1)
            return prev_bits_;
        if (!primed_ || settings_ != s || cached_step_ > target) {
            settings_ = s;
            bits_ = initial_register(s.seed, s.length);
            cached_step_ = 0;
            primed_ = true;
            has_prev_ = false;
        }
        while (cached_step_ < target) {
            prev_bits_ = bits_;
            has_prev_ = true;
            ++cached_step_;
            bits_ = advance_register(bits_, cached_step_, s);
        }
        return bits_;
    }

private:
    RegisterSettings settings_{};
    std::uint64_t bits_ = 0;
    std::uint64_t prev_bits_ = 0;
    std::int64_t cached_step_ = -1;
    bool primed_ = false;
    bool has_prev_ = false;
};

}  // namespace pulp::examples::brew
