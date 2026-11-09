#pragma once

/// @file midi_decode.hpp
/// Pure MIDI 1.0 decoding for controller mapping: raw bytes to a typed,
/// normalized value, with the stateful pieces (14-bit CC pairs, NRPN/RPN data
/// entry) as small fixed-size state machines.
///
/// Everything here is real-time safe: no allocation, no locks, no exceptions,
/// fixed-size state. None of it touches a `StateStore`, so a test, a learn
/// UI, or another subsystem can decode a message stream without running audio.
///
/// Relative encoders: `decode_relative()` turns a raw 7-bit value into a signed
/// tick count under the three common conventions, and `apply_relative()` moves
/// a normalized value by that many ticks.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <pulp/state/midi_source.hpp>

namespace pulp::state {

/// One short channel-voice message as raw bytes (status includes channel).
struct RawMidi {
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;

    constexpr uint8_t type() const {
        return status & 0xF0;
    }
    constexpr uint8_t channel() const {
        return status & 0x0F;
    }
};

/// A decoded controller value. `kind == step` carries a signed increment
/// (NRPN/RPN data increment/decrement) instead of an absolute position.
struct DecodedMidi {
    enum class Kind : uint8_t { absolute, step };

    Kind kind = Kind::absolute;
    MidiSource source{};
    uint16_t raw = 0;       ///< Absolute raw value, 0..max_raw.
    uint16_t max_raw = 127; ///< 127 for 7-bit, 16383 for 14-bit.
    int16_t step = 0;       ///< Signed increment when kind == step.

    /// Position in [0, 1]. Zero for a step event.
    constexpr float normalized() const {
        if (kind == Kind::step || max_raw == 0)
            return 0.0f;
        return static_cast<float>(raw) / static_cast<float>(max_raw);
    }
};

// ── Controller numbers with a fixed role in the data-entry protocols ──

inline constexpr uint8_t kCcDataEntryMsb = 6;
inline constexpr uint8_t kCcDataEntryLsb = 38;
inline constexpr uint8_t kCcDataIncrement = 96;
inline constexpr uint8_t kCcDataDecrement = 97;
inline constexpr uint8_t kCcNrpnLsb = 98;
inline constexpr uint8_t kCcNrpnMsb = 99;
inline constexpr uint8_t kCcRpnLsb = 100;
inline constexpr uint8_t kCcRpnMsb = 101;

/// True for the CCs the NRPN/RPN protocol consumes (6, 38, 96..101). A
/// 7-bit CC mapping should not claim these while a parameter is selected.
constexpr bool is_data_entry_cc(uint8_t cc) {
    return cc == kCcDataEntryMsb || cc == kCcDataEntryLsb ||
           (cc >= kCcDataIncrement && cc <= kCcRpnMsb);
}

/// Decode the stateless message kinds: 7-bit CC, pitch bend, channel pressure,
/// poly pressure, program change. Returns false for anything else (notes,
/// system messages) and for CCs a stateful decoder owns when
/// `include_data_entry_ccs` is false.
constexpr bool decode_simple(const RawMidi& m, DecodedMidi& out,
                             bool include_data_entry_ccs = true) {
    out = {};
    out.source.channel = m.channel();
    switch (m.type()) {
    case 0xB0:
        if (!include_data_entry_ccs && is_data_entry_cc(m.data1))
            return false;
        out.source.kind = MidiSourceKind::cc7;
        out.source.number = m.data1 & 0x7F;
        out.raw = m.data2 & 0x7F;
        return true;
    case 0xE0:
        out.source.kind = MidiSourceKind::pitch_bend;
        out.raw = static_cast<uint16_t>(((m.data2 & 0x7F) << 7) | (m.data1 & 0x7F));
        out.max_raw = 16383;
        return true;
    case 0xD0:
        out.source.kind = MidiSourceKind::channel_pressure;
        out.raw = m.data1 & 0x7F;
        return true;
    case 0xA0:
        out.source.kind = MidiSourceKind::poly_pressure;
        out.source.number = m.data1 & 0x7F;
        out.raw = m.data2 & 0x7F;
        return true;
    case 0xC0:
        out.source.kind = MidiSourceKind::program_change;
        out.source.number = m.data1 & 0x7F;
        out.raw = 127;
        return true;
    default:
        return false;
    }
}

/// Reassembles 14-bit CC pairs (MSB 0..31, LSB 32..63) on every channel.
///
/// An LSB that arrives before its MSB is held and combined when the MSB
/// shows up. Until an LSB has been seen for a pair, its MSB decodes as a
/// 7-bit value (`max_raw == 127`) so a controller that only sends the MSB
/// still reaches full range; once an LSB is seen the pair decodes at 14 bits
/// and stays there until `reset()`.
class Cc14Decoder {
  public:
    /// Feed one message. Returns true and fills `out` when it completed or
    /// updated a pair (the source is `cc14` with `number` = MSB controller).
    bool feed(const RawMidi& m, DecodedMidi& out) {
        if (m.type() != 0xB0)
            return false;
        const uint8_t cc = m.data1 & 0x7F;
        if (cc >= 64)
            return false;
        const bool is_lsb = cc >= 32;
        const uint8_t msb_cc = is_lsb ? static_cast<uint8_t>(cc - 32) : cc;
        Slot& s = slots_[m.channel()][msb_cc];
        if (is_lsb) {
            s.lsb = m.data2 & 0x7F;
            s.has_lsb = true;
            if (!s.has_msb)
                return false;
        } else {
            s.msb = m.data2 & 0x7F;
            s.has_msb = true;
        }
        out = {};
        out.source = {MidiSourceKind::cc14, m.channel(), msb_cc};
        if (s.has_lsb) {
            out.raw = static_cast<uint16_t>((s.msb << 7) | s.lsb);
            out.max_raw = 16383;
        } else {
            out.raw = s.msb;
            out.max_raw = 127;
        }
        return true;
    }

    void reset() {
        slots_ = {};
    }

  private:
    struct Slot {
        uint8_t msb = 0;
        uint8_t lsb = 0;
        bool has_msb = false;
        bool has_lsb = false;
    };
    std::array<std::array<Slot, 32>, 16> slots_{};
};

/// NRPN / RPN data entry on every channel: parameter selection (CC 99/98 or
/// 101/100), data entry (CC 6 + optional 38), and increment/decrement
/// (CC 96/97). A selection needs both halves and persists until changed, as
/// the protocol specifies; RPN 127/127 (null) deselects. The data LSB is
/// forgotten whenever the selection changes, so a sender that omits CC 38
/// decodes at 7 bits.
class NrpnDecoder {
  public:
    /// Feed one message. Returns true and fills `out` when a data entry or
    /// increment/decrement completed against a selected parameter. Selection
    /// messages and data entry with nothing selected return false.
    bool feed(const RawMidi& m, DecodedMidi& out) {
        if (m.type() != 0xB0)
            return false;
        Chan& c = chans_[m.channel()];
        const uint8_t v = m.data2 & 0x7F;
        switch (m.data1 & 0x7F) {
        case kCcNrpnMsb:
            select(c, true, true, v);
            return false;
        case kCcNrpnLsb:
            select(c, true, false, v);
            return false;
        case kCcRpnMsb:
            select(c, false, true, v);
            return false;
        case kCcRpnLsb:
            select(c, false, false, v);
            return false;
        case kCcDataEntryMsb:
            if (!c.ready())
                return false;
            c.data_msb = v;
            c.has_msb = true;
            return emit_absolute(m.channel(), c, out);
        case kCcDataEntryLsb:
            if (!c.ready())
                return false;
            c.data_lsb = v;
            c.has_lsb = true;
            if (!c.has_msb)
                return false;
            return emit_absolute(m.channel(), c, out);
        case kCcDataIncrement:
            return emit_step(m.channel(), c, +1, v, out);
        case kCcDataDecrement:
            return emit_step(m.channel(), c, -1, v, out);
        default:
            return false;
        }
    }

    void reset() {
        chans_ = {};
    }

  private:
    struct Chan {
        bool is_nrpn = true;
        uint8_t sel_msb = 0;
        uint8_t sel_lsb = 0;
        bool has_sel_msb = false;
        bool has_sel_lsb = false;
        uint8_t data_msb = 0;
        uint8_t data_lsb = 0;
        bool has_msb = false;
        bool has_lsb = false;

        bool ready() const {
            return has_sel_msb && has_sel_lsb;
        }
        uint16_t number() const {
            return static_cast<uint16_t>((sel_msb << 7) | sel_lsb);
        }
    };

    static void select(Chan& c, bool nrpn, bool msb_half, uint8_t v) {
        if (c.is_nrpn != nrpn) {
            c.is_nrpn = nrpn;
            c.has_sel_msb = c.has_sel_lsb = false;
        }
        if (msb_half) {
            c.sel_msb = v;
            c.has_sel_msb = true;
        } else {
            c.sel_lsb = v;
            c.has_sel_lsb = true;
        }
        c.has_msb = c.has_lsb = false;
        if (!nrpn && c.has_sel_msb && c.has_sel_lsb && c.sel_msb == 127 && c.sel_lsb == 127)
            c.has_sel_msb = c.has_sel_lsb = false; // null RPN deselects
    }

    static bool emit_absolute(uint8_t ch, const Chan& c, DecodedMidi& out) {
        out = {};
        out.source = {c.is_nrpn ? MidiSourceKind::nrpn : MidiSourceKind::rpn, ch, c.number()};
        if (c.has_lsb) {
            out.raw = static_cast<uint16_t>((c.data_msb << 7) | c.data_lsb);
            out.max_raw = 16383;
        } else {
            out.raw = c.data_msb;
            out.max_raw = 127;
        }
        return true;
    }

    static bool emit_step(uint8_t ch, const Chan& c, int sign, uint8_t amount, DecodedMidi& out) {
        if (!c.ready())
            return false;
        out = {};
        out.kind = DecodedMidi::Kind::step;
        out.source = {c.is_nrpn ? MidiSourceKind::nrpn : MidiSourceKind::rpn, ch, c.number()};
        out.step = static_cast<int16_t>(sign * std::max<int>(1, amount));
        return true;
    }

    std::array<Chan, 16> chans_{};
};

// ── Relative encoders ──

enum class RelativeMode : uint8_t {
    absolute,        ///< Value is a position, not a tick count.
    twos_complement, ///< 1..63 up, 127..65 down (127 = -1), 0 = none.
    signed_bit,      ///< Bit 6 is the sign, bits 0..5 the magnitude (1 up, 65 down).
    binary_offset,   ///< 64 = none, above is up, below is down.
};

/// Signed tick count for one raw 7-bit value. `absolute` yields 0.
constexpr int decode_relative(RelativeMode mode, uint8_t raw) {
    raw &= 0x7F;
    switch (mode) {
    case RelativeMode::twos_complement:
        return raw < 64 ? raw : static_cast<int>(raw) - 128;
    case RelativeMode::signed_bit: {
        const int mag = raw & 0x3F;
        return (raw & 0x40) ? -mag : mag;
    }
    case RelativeMode::binary_offset:
        return static_cast<int>(raw) - 64;
    case RelativeMode::absolute:
        return 0;
    }
    return 0;
}

/// How a tick count moves a normalized value.
struct RelativeConfig {
    float step = 1.0f / 128.0f; ///< Normalized change per tick.
    float acceleration = 0.0f;  ///< 0 = linear; larger values amplify fast bursts.

    friend constexpr bool operator==(const RelativeConfig&, const RelativeConfig&) = default;
};

/// Move `current` (normalized) by `ticks`, clamped to [0, 1]. Acceleration
/// scales a burst of n ticks by `1 + acceleration * (n - 1)`.
inline float apply_relative(float current, int ticks, const RelativeConfig& cfg = {}) {
    if (ticks == 0)
        return std::clamp(current, 0.0f, 1.0f);
    const float n = static_cast<float>(std::abs(ticks));
    const float gain = 1.0f + std::max(0.0f, cfg.acceleration) * (n - 1.0f);
    const float delta = static_cast<float>(ticks) * cfg.step * gain;
    return std::clamp(current + delta, 0.0f, 1.0f);
}

} // namespace pulp::state
