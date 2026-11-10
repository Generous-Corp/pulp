#pragma once

/// @file midi_feedback.hpp
/// The pure half of MIDI feedback: turning a parameter value back into the
/// message a mapped controller would send, so a motorised fader, an LED ring or
/// a controller display can follow the parameter.
///
/// `controller_position()` inverts a mapping's window; `encode_feedback()`
/// builds the messages for a position. Neither touches a `StateStore`; the
/// engine decides when to send (`MidiLearnEngine::emit_feedback`).
///
/// Not sent: program change (a trigger, not a position). An omni mapping sends
/// on channel 1 (index 0).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <pulp/state/midi_decode.hpp>
#include <pulp/state/midi_mapping.hpp>
#include <pulp/state/midi_source.hpp>

namespace pulp::state {

/// Source kinds `encode_feedback()` can produce.
inline constexpr MidiSourceKindMask kFeedbackEncodableKinds = MidiSourceKindMask::of(
    {MidiSourceKind::cc7, MidiSourceKind::cc14, MidiSourceKind::nrpn, MidiSourceKind::rpn,
     MidiSourceKind::pitch_bend, MidiSourceKind::channel_pressure, MidiSourceKind::poly_pressure});

/// Most messages one mapping's feedback needs (NRPN: select MSB, select LSB, data MSB, data LSB).
inline constexpr int kMaxFeedbackMessages = 4;

/// The controller position (0..1) that produces `param_normalized` through the
/// mapping's window, or nullopt when the window is flat. Positions outside the
/// window clamp to its ends.
inline std::optional<float> controller_position(const MidiMapping& m, float param_normalized) {
    const float span = m.scale.max - m.scale.min;
    if (std::fabs(span) < 1e-6f || !std::isfinite(param_normalized))
        return std::nullopt;
    return std::clamp((param_normalized - m.scale.min) / span, 0.0f, 1.0f);
}

/// Resolution of the controller's value for this kind (steps - 1).
constexpr int feedback_resolution(MidiSourceKind kind) {
    switch (kind) {
    case MidiSourceKind::cc14:
    case MidiSourceKind::pitch_bend:
        return 16383;
    case MidiSourceKind::nrpn:
    case MidiSourceKind::rpn:
        return 16383;
    default:
        return 127;
    }
}

/// Build the messages that put `position` (0..1) on the mapping's controller.
/// Returns how many were written to `out` (0 for kinds that cannot be sent).
inline int encode_feedback(const MidiMapping& m, float position,
                           RawMidi (&out)[kMaxFeedbackMessages]) {
    position = std::clamp(position, 0.0f, 1.0f);
    const uint8_t ch = m.source.channel == MidiSource::kOmni ? uint8_t{0} : m.source.channel;
    const auto v7 = [&] { return static_cast<uint8_t>(std::lround(position * 127.0f)); };
    const auto v14 = [&] { return static_cast<uint16_t>(std::lround(position * 16383.0f)); };
    const auto cc = [&](uint8_t n, uint8_t v) {
        return RawMidi{static_cast<uint8_t>(0xB0 | ch), n, v};
    };
    const uint16_t num = m.source.number;

    switch (m.source.kind) {
    case MidiSourceKind::cc7:
        out[0] = cc(static_cast<uint8_t>(num), v7());
        return 1;
    case MidiSourceKind::cc14: {
        const uint16_t v = v14();
        out[0] = cc(static_cast<uint8_t>(num), static_cast<uint8_t>(v >> 7));
        out[1] = cc(static_cast<uint8_t>(num + 32), static_cast<uint8_t>(v & 0x7F));
        return 2;
    }
    case MidiSourceKind::nrpn:
    case MidiSourceKind::rpn: {
        const bool nrpn = m.source.kind == MidiSourceKind::nrpn;
        const uint16_t v = v14();
        out[0] = cc(nrpn ? kCcNrpnMsb : kCcRpnMsb, static_cast<uint8_t>(num >> 7));
        out[1] = cc(nrpn ? kCcNrpnLsb : kCcRpnLsb, static_cast<uint8_t>(num & 0x7F));
        out[2] = cc(kCcDataEntryMsb, static_cast<uint8_t>(v >> 7));
        out[3] = cc(kCcDataEntryLsb, static_cast<uint8_t>(v & 0x7F));
        return 4;
    }
    case MidiSourceKind::pitch_bend: {
        const uint16_t v = v14();
        out[0] = RawMidi{static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(v & 0x7F),
                         static_cast<uint8_t>(v >> 7)};
        return 1;
    }
    case MidiSourceKind::channel_pressure:
        out[0] = RawMidi{static_cast<uint8_t>(0xD0 | ch), v7(), 0};
        return 1;
    case MidiSourceKind::poly_pressure:
        out[0] = RawMidi{static_cast<uint8_t>(0xA0 | ch), static_cast<uint8_t>(num & 0x7F), v7()};
        return 1;
    case MidiSourceKind::program_change:
        return 0;
    }
    return 0;
}

} // namespace pulp::state
