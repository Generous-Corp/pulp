#pragma once

/// @file panic.hpp
/// Pure MIDI 1.0 "panic" message generation and classification.
///
/// A panic silences a receiver: it ends sounding notes, drops sustain, and puts
/// the controllers it may have left displaced back at rest. Everything here is
/// a plain function over caller-owned storage — no allocation, no locks, no
/// state — so it is safe on the audio thread and can be unit tested without a
/// host. The held-note source of truth is MidiKeyboardState; this header only
/// turns its contents (or, as a fallback, "every key") into note-off events.
///
/// The messages used are the channel mode and controller messages the MIDI 1.0
/// specification defines for this purpose:
///
/// | Message | Meaning | Receiver behaviour |
/// |---|---|---|
/// | CC 120 value 0 | All Sound Off | stop sounding immediately, release tails included |
/// | CC 123 value 0 | All Notes Off | release held notes through the normal release |
/// | CC 121 value 0 | Reset All Controllers | controllers return to their defaults |
/// | CC 64 value 0 | Damper (sustain) off | release notes the pedal was holding |
/// | Pitch bend 8192 | Pitch wheel centre | undo a bend left displaced |
///
/// CC 120 and CC 123 are deliberately different: sending both is the safe panic
/// (the immediate stop covers receivers that implement only one of them), and a
/// "release-only" panic omits CC 120 so tails ring out.

#include <pulp/midi/keyboard_state.hpp>
#include <pulp/midi/message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace pulp::midi {

/// One kind of message a panic can contain.
enum class PanicMessageKind : std::uint8_t {
    all_sound_off,         ///< CC 120 = 0, immediate stop
    all_notes_off,         ///< CC 123 = 0, release
    reset_all_controllers, ///< CC 121 = 0
    sustain_off,           ///< CC 64 = 0
    pitch_bend_centre,     ///< pitch bend 8192
};

/// Which channels and which message kinds a panic covers.
struct PanicScope {
    /// Bit n set means channel n (0-15) is included. Default: all 16.
    std::uint16_t channel_mask = 0xFFFFu;
    /// Immediate-stop CC 120. Clear for a release-only panic.
    bool all_sound_off = true;
    bool all_notes_off = true;
    bool reset_all_controllers = true;
    bool sustain_off = true;
    bool pitch_bend_centre = true;

    /// Panic that lets release tails ring out (no CC 120).
    static constexpr PanicScope release_only() {
        PanicScope scope;
        scope.all_sound_off = false;
        return scope;
    }

    constexpr bool includes_channel(unsigned channel) const {
        return channel < 16u && ((channel_mask >> channel) & 1u) != 0u;
    }
};

/// Largest number of events panic_messages() can produce (16 channels x 5).
inline constexpr std::size_t kMaxPanicMessages = 16u * 5u;

/// Largest number of events held_note_offs() / all_note_offs() can produce.
inline constexpr std::size_t kMaxNoteOffs = 16u * 128u;

namespace panic_detail {
inline constexpr std::uint8_t kCcSustain = 64;
inline constexpr std::uint8_t kCcResetAllControllers = 121;
inline constexpr std::uint8_t kCcAllSoundOff = 120;
inline constexpr std::uint8_t kCcAllNotesOff = 123;
inline constexpr std::uint16_t kPitchBendCentre = 8192;

inline constexpr std::size_t kind_count(const PanicScope& scope) {
    return std::size_t{scope.all_sound_off} + std::size_t{scope.all_notes_off} +
           std::size_t{scope.reset_all_controllers} + std::size_t{scope.sustain_off} +
           std::size_t{scope.pitch_bend_centre};
}

inline constexpr unsigned popcount16(std::uint16_t mask) {
    unsigned n = 0;
    for (; mask != 0u; mask = static_cast<std::uint16_t>(mask & (mask - 1u)))
        ++n;
    return n;
}
} // namespace panic_detail

/// Number of events panic_messages() will write for `scope`.
inline constexpr std::size_t panic_message_count(const PanicScope& scope) {
    return panic_detail::kind_count(scope) * panic_detail::popcount16(scope.channel_mask);
}

/// Write the panic messages for `scope` into `out`, channel by channel. Within a
/// channel the order is: sustain off, All Notes Off, All Sound Off, Reset All
/// Controllers, pitch bend centre. Sustain goes first so a pedal-held note is
/// released before the notes message arrives; the controller reset and bend
/// centre come last so they are not undone by anything earlier.
///
/// Every event carries `sample_offset`. Returns the number of events written,
/// which is panic_message_count(scope) when `out` is large enough; if `out` is
/// too small the output is cut at a whole-event boundary and the return value
/// is the count actually written (compare with panic_message_count() to detect
/// truncation).
inline std::size_t panic_messages(const PanicScope& scope, std::span<MidiEvent> out,
                                  std::int32_t sample_offset = 0) noexcept {
    using namespace panic_detail;
    std::size_t n = 0;
    const auto put = [&](MidiEvent event) {
        if (n >= out.size())
            return false;
        event.sample_offset = sample_offset;
        out[n++] = event;
        return true;
    };
    for (unsigned ch = 0; ch < 16u; ++ch) {
        if (!scope.includes_channel(ch))
            continue;
        const auto channel = static_cast<std::uint8_t>(ch);
        if (scope.sustain_off && !put(MidiEvent::cc(channel, kCcSustain, 0)))
            return n;
        if (scope.all_notes_off && !put(MidiEvent::cc(channel, kCcAllNotesOff, 0)))
            return n;
        if (scope.all_sound_off && !put(MidiEvent::cc(channel, kCcAllSoundOff, 0)))
            return n;
        if (scope.reset_all_controllers && !put(MidiEvent::cc(channel, kCcResetAllControllers, 0)))
            return n;
        if (scope.pitch_bend_centre && !put(MidiEvent::pitch_bend(channel, kPitchBendCentre)))
            return n;
    }
    return n;
}

/// Call `fn(channel, note)` for every key `keys` reports as held on the
/// channels in `channel_mask`, in channel then note order. The allocation-free
/// way to walk the held set when a caller writes straight into its own buffer
/// instead of an intermediate span.
template <typename Fn>
inline void for_each_held_note(const MidiKeyboardState& keys, std::uint16_t channel_mask,
                               Fn&& fn) noexcept(noexcept(fn(std::uint8_t{}, std::uint8_t{}))) {
    for (unsigned ch = 0; ch < 16u; ++ch) {
        if (((channel_mask >> ch) & 1u) == 0u)
            continue;
        for (unsigned note = 0; note < 128u; ++note) {
            if (keys.is_note_on(static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(note)))
                fn(static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(note));
        }
    }
}

/// Note-off events for exactly the notes `keys` reports as held, on the
/// channels in `channel_mask`, in channel then note order. This is the precise
/// form: nothing is sent for a key that is not down. Returns the count written
/// (truncated at a whole event if `out` is too small).
inline std::size_t held_note_offs(const MidiKeyboardState& keys, std::span<MidiEvent> out,
                                  std::uint16_t channel_mask = 0xFFFFu,
                                  std::int32_t sample_offset = 0) noexcept {
    std::size_t n = 0;
    for_each_held_note(keys, channel_mask, [&](std::uint8_t ch, std::uint8_t note) {
        if (n >= out.size())
            return;
        auto event = MidiEvent::note_off(ch, note);
        event.sample_offset = sample_offset;
        out[n++] = event;
    });
    return n;
}

/// Brute-force fallback: a note-off for every key on the channels in
/// `channel_mask`, whether or not it was tracked as held. For hosts and formats
/// that bypass the tracker. Up to kMaxNoteOffs events.
inline std::size_t all_note_offs(std::span<MidiEvent> out, std::uint16_t channel_mask = 0xFFFFu,
                                 std::int32_t sample_offset = 0) noexcept {
    std::size_t n = 0;
    for (unsigned ch = 0; ch < 16u; ++ch) {
        if (((channel_mask >> ch) & 1u) == 0u)
            continue;
        for (unsigned note = 0; note < 128u; ++note) {
            if (n >= out.size())
                return n;
            auto event =
                MidiEvent::note_off(static_cast<std::uint8_t>(ch), static_cast<std::uint8_t>(note));
            event.sample_offset = sample_offset;
            out[n++] = event;
        }
    }
    return n;
}

/// Classify a message as one of the panic kinds, or nullopt for anything else.
/// The value byte must be the one panic_messages() sends (0 for the CCs, centre
/// for pitch bend): CC 123 with a nonzero value is not an All Notes Off.
inline std::optional<PanicMessageKind> classify_panic_message(const MidiEvent& event) noexcept {
    using namespace panic_detail;
    const auto& msg = event.message;
    if (msg.isController()) {
        if (msg.getControllerValue() != 0)
            return std::nullopt;
        switch (msg.getControllerNumber()) {
        case kCcAllSoundOff:
            return PanicMessageKind::all_sound_off;
        case kCcAllNotesOff:
            return PanicMessageKind::all_notes_off;
        case kCcResetAllControllers:
            return PanicMessageKind::reset_all_controllers;
        case kCcSustain:
            return PanicMessageKind::sustain_off;
        default:
            return std::nullopt;
        }
    }
    if (msg.isPitchWheel() && msg.getPitchWheelValue() == kPitchBendCentre)
        return PanicMessageKind::pitch_bend_centre;
    return std::nullopt;
}

} // namespace pulp::midi
