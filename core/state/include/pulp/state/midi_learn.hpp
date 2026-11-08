#pragma once

/// @file midi_learn.hpp
/// Pure functions behind MIDI Learn: working out WHAT a user moved from a short
/// burst of messages, and how a controller that does not match the parameter's
/// current value should take over ("pickup").
///
/// Both are stateless or hold a tiny POD, allocate nothing, and never touch a
/// `StateStore`, so a learn UI, a test, or the audio thread can call them.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <pulp/state/midi_decode.hpp>
#include <pulp/state/midi_source.hpp>
#include <span>

namespace pulp::state {

// ── Learn classification ──

/// What a learn burst resolved to.
struct LearnResult {
    bool valid = false;  ///< False when the burst held nothing learnable.
    MidiSource source{}; ///< The controller to bind (channel is the one observed).
    RelativeMode mode = RelativeMode::absolute;
    float confidence = 0.0f; ///< 0..1; below ~0.6 a UI should offer a mode override.
};

namespace detail {

constexpr bool in_range(uint8_t v, uint8_t lo, uint8_t hi) {
    return v >= lo && v <= hi;
}

/// Relative-encoder detection for one 7-bit CC's values. Relative encoders
/// send small tick counts, so their values cluster at the edges of a
/// convention's window; an absolute control passes through the middle.
inline LearnResult classify_cc7_values(std::span<const uint8_t> v, LearnResult base) {
    constexpr uint8_t kReach = 16; // largest tick count a slow-to-brisk turn produces
    constexpr std::size_t kMinEvents = 3;
    base.mode = RelativeMode::absolute;
    base.confidence = v.size() >= 2 ? 0.6f : 0.5f;
    if (v.size() < kMinEvents)
        return base;

    bool twos = true, signed_bit = true, offset = true;
    bool low = false, high = false, up65 = false, down48 = false;
    for (uint8_t x : v) {
        const bool lo = in_range(x, 1, kReach);
        const bool hi = in_range(x, 128 - kReach, 127);
        const bool u65 = in_range(x, 65, 64 + kReach);
        const bool d48 = in_range(x, 64 - kReach, 63);
        twos = twos && (lo || hi);
        signed_bit = signed_bit && (lo || u65);
        offset = offset && (d48 || u65);
        low = low || lo;
        high = high || hi;
        up65 = up65 || u65;
        down48 = down48 || d48;
    }

    // Both directions seen under one convention: strong evidence.
    if (twos && low && high) {
        base.mode = RelativeMode::twos_complement;
        base.confidence = 0.95f;
        return base;
    }
    if (signed_bit && low && up65) {
        base.mode = RelativeMode::signed_bit;
        base.confidence = 0.95f;
        return base;
    }
    if (offset && down48 && up65) {
        base.mode = RelativeMode::binary_offset;
        base.confidence = 0.95f;
        return base;
    }

    // One direction only: an absolute pot sweeping slowly through the same
    // values would look identical, but it ramps through distinct values while
    // an encoder repeats the same tick. Require mostly-repeated values.
    std::size_t repeats = 0;
    for (std::size_t i = 1; i < v.size(); ++i)
        repeats += (v[i] == v[i - 1]);
    if (repeats * 2 < v.size() - 1)
        return base;

    if (twos && (low != high)) {
        base.mode = RelativeMode::twos_complement;
        base.confidence = high ? 0.75f : 0.7f;
    } else if (offset && (down48 != up65)) {
        base.mode = RelativeMode::binary_offset;
        base.confidence = down48 ? 0.7f : 0.55f;
    }
    return base;
}

} // namespace detail

/// Decide which controller a burst of messages came from and, for a 7-bit CC,
/// whether it is a relative encoder. Recognises NRPN/RPN (selection + data or
/// increment/decrement), 14-bit CC pairs, 7-bit CC, pitch bend, channel
/// pressure, poly pressure, and program change. Ignores notes and anything
/// else; the first learnable controller in the burst wins.
///
/// Detection floor: relative detection needs at least 3 events from the same
/// controller. With fewer, or when the values pass through the middle of the
/// range, the result is `absolute`. A single repeated tick is indistinguishable
/// from an absolute control parked at that value; the confidence reports that.
inline LearnResult classify_learn_sequence(std::span<const RawMidi> burst) {
    LearnResult out;
    DecodedMidi d;

    NrpnDecoder nrpn;
    for (const auto& m : burst) {
        if (nrpn.feed(m, d)) {
            out.valid = true;
            out.source = d.source;
            out.confidence = 0.9f;
            return out;
        }
    }

    Cc14Decoder cc14;
    for (const auto& m : burst) {
        if (cc14.feed(m, d) && d.max_raw == 16383) {
            out.valid = true;
            out.source = d.source;
            out.confidence = 0.9f;
            return out;
        }
    }

    bool have_primary = false;
    for (const auto& m : burst) {
        DecodedMidi s;
        if (!decode_simple(m, s, /*include_data_entry_ccs=*/false))
            continue;
        if (!have_primary) {
            have_primary = true;
            out.valid = true;
            out.source = s.source;
            out.confidence = 0.9f;
            break;
        }
    }
    if (!have_primary)
        return out;

    if (out.source.kind == MidiSourceKind::cc7) {
        uint8_t values[64];
        std::size_t n = 0;
        for (const auto& m : burst) {
            DecodedMidi s;
            if (decode_simple(m, s, false) && s.source == out.source && n < 64)
                values[n++] = static_cast<uint8_t>(s.raw);
        }
        out = detail::classify_cc7_values(std::span<const uint8_t>(values, n), out);
    }
    return out;
}

// ── Takeover ──

/// How a controller whose position differs from the parameter takes over.
enum class TakeoverMode : uint8_t {
    jump,   ///< Apply immediately; the parameter leaps to the controller.
    pickup, ///< Ignore the controller until it reaches or crosses the parameter.
    scale,  ///< Move relative to the controller, converging so both meet at the end of travel.
};

/// Per-mapping takeover memory. Reset whenever something other than this
/// controller moves the parameter (automation, preset load, UI drag).
struct TakeoverState {
    bool engaged = false;
    bool has_last = false;
    float last_input = 0.0f;
};

inline void takeover_reset(TakeoverState& s) {
    s = {};
}

struct TakeoverResult {
    bool apply = false;
    float value = 0.0f; ///< Normalized value to write when `apply`.
};

/// One controller move: `input` and `current` are normalized [0, 1].
/// `epsilon` is the distance regarded as "reached".
inline TakeoverResult takeover_step(TakeoverMode mode, TakeoverState& s, float input, float current,
                                    float epsilon = 0.02f) {
    input = std::clamp(input, 0.0f, 1.0f);
    current = std::clamp(current, 0.0f, 1.0f);
    if (mode == TakeoverMode::jump) {
        s.engaged = true;
        s.has_last = true;
        s.last_input = input;
        return {true, input};
    }

    const bool had_last = s.has_last;
    const float last = s.last_input;
    s.has_last = true;
    s.last_input = input;

    if (s.engaged)
        return {true, input};
    if (std::fabs(input - current) <= epsilon) {
        s.engaged = true;
        return {true, input};
    }
    if (!had_last)
        return {false, 0.0f};

    if (mode == TakeoverMode::pickup) {
        // Crossed the parameter between the last and this position.
        if ((last - current) * (input - current) <= 0.0f) {
            s.engaged = true;
            return {true, input};
        }
        return {false, 0.0f};
    }

    // scale: move by the controller's delta, stretched so the controller's
    // remaining travel (toward the end it is moving to) maps onto the
    // parameter's remaining travel.
    const float delta = input - last;
    float ratio = 1.0f;
    if (delta > 0.0f) {
        const float room_in = 1.0f - last;
        if (room_in > 1e-6f)
            ratio = (1.0f - current) / room_in;
    } else if (delta < 0.0f) {
        if (last > 1e-6f)
            ratio = current / last;
    }
    const float out = std::clamp(current + delta * ratio, 0.0f, 1.0f);
    if (std::fabs(input - out) <= epsilon)
        s.engaged = true;
    return {true, out};
}

} // namespace pulp::state
