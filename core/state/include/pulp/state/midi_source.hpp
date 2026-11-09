#pragma once

/// @file midi_source.hpp
/// Identity of a MIDI controller that can drive a parameter: the kind of
/// message, its channel, and the controller / parameter / note number.
///
/// A `MidiSource` is a plain value (trivially copyable, no allocation) so it
/// can be a mapping key on the audio thread and a record in saved state.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

namespace pulp::state {

enum class MidiSourceKind : uint8_t {
    cc7,              ///< 7-bit Control Change; `number` = CC 0..127.
    cc14,             ///< 14-bit CC pair; `number` = the MSB controller 0..31 (LSB = number + 32).
    nrpn,             ///< Non-Registered Parameter Number; `number` = 14-bit parameter number.
    rpn,              ///< Registered Parameter Number; `number` = 14-bit parameter number.
    pitch_bend,       ///< 14-bit pitch bend; `number` unused.
    channel_pressure, ///< Channel aftertouch; `number` unused.
    poly_pressure,    ///< Polyphonic key pressure; `number` = note 0..127.
    program_change,   ///< Program change as a trigger; `number` = program 0..127.
};

struct MidiSource {
    /// Match any channel (same value `MidiParameterMap::kOmni` uses).
    static constexpr uint8_t kOmni = 0xFF;

    MidiSourceKind kind = MidiSourceKind::cc7;
    uint8_t channel = kOmni; ///< 0..15, or kOmni.
    uint16_t number = 0;

    friend constexpr bool operator==(const MidiSource&, const MidiSource&) = default;

    /// True when `incoming` (a concrete message source, channel 0..15) is
    /// matched by this source, honouring an omni channel on this side.
    constexpr bool matches(const MidiSource& incoming) const {
        return kind == incoming.kind && number == incoming.number &&
               (channel == kOmni || channel == incoming.channel);
    }

    /// True for kinds that carry a meaningful `number`.
    constexpr bool has_number() const {
        return kind != MidiSourceKind::pitch_bend && kind != MidiSourceKind::channel_pressure;
    }
};

/// A set of source kinds (e.g. the kinds a format can send back to a controller).
struct MidiSourceKindMask {
    uint16_t bits = 0;

    static constexpr MidiSourceKindMask of(std::initializer_list<MidiSourceKind> kinds) {
        MidiSourceKindMask m;
        for (auto k : kinds)
            m.bits = static_cast<uint16_t>(m.bits | (1u << static_cast<unsigned>(k)));
        return m;
    }
    constexpr bool has(MidiSourceKind k) const {
        return (bits >> static_cast<unsigned>(k)) & 1u;
    }
    constexpr bool empty() const {
        return bits == 0;
    }
    friend constexpr bool operator==(const MidiSourceKindMask&,
                                     const MidiSourceKindMask&) = default;
};

struct MidiSourceHash {
    std::size_t operator()(const MidiSource& s) const noexcept {
        std::size_t h = static_cast<std::size_t>(s.kind);
        h = h * 31u + s.channel;
        h = h * 31u + s.number;
        return std::hash<std::size_t>{}(h);
    }
};

/// Short human-readable label, e.g. "CC 74 ch 1", "NRPN 1234 ch 3",
/// "Pitch Bend (any ch)". Channels display 1-based. UI thread only.
inline std::string label(const MidiSource& s) {
    std::string out;
    switch (s.kind) {
    case MidiSourceKind::cc7:
        out = "CC " + std::to_string(s.number);
        break;
    case MidiSourceKind::cc14:
        out = "CC " + std::to_string(s.number) + "/" + std::to_string(s.number + 32) + " (14-bit)";
        break;
    case MidiSourceKind::nrpn:
        out = "NRPN " + std::to_string(s.number);
        break;
    case MidiSourceKind::rpn:
        out = "RPN " + std::to_string(s.number);
        break;
    case MidiSourceKind::pitch_bend:
        out = "Pitch Bend";
        break;
    case MidiSourceKind::channel_pressure:
        out = "Pressure";
        break;
    case MidiSourceKind::poly_pressure:
        out = "Poly Pressure note " + std::to_string(s.number);
        break;
    case MidiSourceKind::program_change:
        out = "Program " + std::to_string(s.number);
        break;
    }
    out += s.channel == MidiSource::kOmni ? " (any ch)" : " ch " + std::to_string(s.channel + 1);
    return out;
}

} // namespace pulp::state
