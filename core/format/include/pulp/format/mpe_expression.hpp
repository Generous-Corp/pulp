#pragma once

/// @file mpe_expression.hpp
/// Per-note expression → MIDI 1.0 synthesis shared by the format adapters.
///
/// Every host format carries per-note expression in its own currency — CLAP
/// `clap_event_note_expression_t`, VST3 `kNoteExpressionValueEvent`, AU v3
/// `AUParameterEvent` ramps — but Pulp's per-note model is MPE: one note per
/// member channel, fed by `MpeVoiceTracker` from a channel-wide MIDI 1.0
/// stream. So each adapter decodes its own event struct and then synthesizes
/// the *same three* channel-wide messages for the *same three* MPE axes:
///
///   - **pitch** → 14-bit pitch bend, normalized to the tracker's default
///     member-bend range so the tracker expands it back to the right per-note
///     bend;
///   - **pressure** → channel pressure (status `0xD0`);
///   - **timbre** → CC 74, the MPE timbre controller.
///
/// The per-format *decode* (which type ID means which axis, and how a raw host
/// value maps onto the axis) stays in the adapter — the formats genuinely
/// disagree there. What lives here is only the synthesis, which was copy-pasted
/// per adapter with nothing that noticed when a copy drifted.
///
/// **Scoping.** Bridging noteId/key-targeted expression onto channel-wide
/// messages is exact when each note lives on its own MPE member channel (the
/// expressive-controller case MPE is built for); when several notes share one
/// channel the expression collapses to channel-wide. That is a property of the
/// MPE model, not of this mapping.
///
/// **Real-time contract.** Pure functions over scalars; allocation-, lock-, and
/// branch-light. Callers set `sample_offset` on the returned event themselves,
/// because each format reads the block offset from a different field.

#include <pulp/midi/message.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::format::mpe {

/// Quantize a normalized [0,1] axis value to a 7-bit MIDI data byte.
/// Values outside the range are clamped, so a host that overshoots cannot
/// produce a malformed MIDI byte.
inline std::uint8_t to_7bit(double norm) noexcept {
    const double clamped = std::clamp(norm, 0.0, 1.0);
    return static_cast<std::uint8_t>(
        std::clamp<int>(static_cast<int>(clamped * 127.0 + 0.5), 0, 127));
}

/// Per-note pitch as a channel-wide 14-bit pitch bend.
///
/// @p semitones is the signed detune the host asked for. It is normalized to
/// `MpeVoiceTracker::kDefaultMemberBendSemitones` (and clamped to ±1 of that
/// range) so the tracker's inverse expansion recovers the requested detune;
/// a request beyond the member range saturates rather than wrapping.
inline midi::MidiEvent pitch_bend_from_semitones(std::uint8_t channel,
                                                 double semitones) {
    const double bend_norm = std::clamp(
        semitones / static_cast<double>(
            midi::MpeVoiceTracker::kDefaultMemberBendSemitones),
        -1.0, 1.0);
    const int bend14 = static_cast<int>(std::lround(8192.0 + bend_norm * 8191.0));
    return midi::MidiEvent::pitch_bend(
        channel, static_cast<std::uint16_t>(std::clamp(bend14, 0, 16383)));
}

/// Per-note pressure as channel pressure (status `0xD0`). There is no
/// `MidiEvent` factory for channel pressure, so the message is built directly.
inline midi::MidiEvent channel_pressure(std::uint8_t channel, double norm) {
    return midi::MidiEvent{
        choc::midi::ShortMessage(
            static_cast<std::uint8_t>(0xD0 | (channel & 0x0F)),
            to_7bit(norm), 0),
        0, 0.0};
}

/// MPE timbre controller.
inline constexpr std::uint8_t kTimbreController = 74;

/// Per-note timbre as CC 74.
inline midi::MidiEvent timbre_cc(std::uint8_t channel, double norm) {
    return midi::MidiEvent::cc(channel, kTimbreController, to_7bit(norm));
}

}  // namespace pulp::format::mpe
