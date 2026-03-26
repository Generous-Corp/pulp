#pragma once

/// @file rpn_parser.hpp
/// RPN/NRPN parser — extracts 14-bit parameter messages from CC sequences.

#include <pulp/midi/message.hpp>
#include <functional>
#include <cstdint>

namespace pulp::midi {

/// Parses RPN (Registered Parameter Number) and NRPN (Non-Registered
/// Parameter Number) messages from a stream of MIDI CC events.
///
/// RPN/NRPN messages are multi-CC sequences:
/// 1. CC 101/99 (MSB) + CC 100/98 (LSB) → selects the parameter
/// 2. CC 6 (Data Entry MSB) + CC 38 (Data Entry LSB) → sets the value
/// 3. CC 96/97 → increment/decrement
///
/// @code
/// RpnParser rpn;
/// rpn.on_rpn = [](uint8_t ch, uint16_t param, uint16_t value) {
///     if (param == 0) set_pitch_bend_range(value >> 7); // semitones
/// };
/// rpn.on_nrpn = [](uint8_t ch, uint16_t param, uint16_t value) {
///     handle_custom_param(param, value);
/// };
///
/// // Feed CC events from your process() callback:
/// rpn.process(event);
/// @endcode
class RpnParser {
public:
    /// Called when a complete RPN message is received.
    /// @param channel MIDI channel (0-15).
    /// @param parameter 14-bit parameter number (MSB << 7 | LSB).
    /// @param value 14-bit data value (MSB << 7 | LSB).
    std::function<void(uint8_t channel, uint16_t parameter, uint16_t value)> on_rpn;

    /// Called when a complete NRPN message is received.
    std::function<void(uint8_t channel, uint16_t parameter, uint16_t value)> on_nrpn;

    /// Called when an increment (CC 96) is received for the active parameter.
    std::function<void(uint8_t channel, uint16_t parameter, bool is_rpn)> on_increment;

    /// Called when a decrement (CC 97) is received for the active parameter.
    std::function<void(uint8_t channel, uint16_t parameter, bool is_rpn)> on_decrement;

    /// Process a MIDI event. Non-CC events are ignored.
    void process(const MidiEvent& event) {
        auto msg = event.message;
        if (!msg.isControllerMessage()) return;

        uint8_t channel = msg.getChannel0to15();
        uint8_t cc = msg.getControllerNumber();
        uint8_t val = msg.getControllerValue();

        auto& state = channel_state_[channel];

        switch (cc) {
            case 101: // RPN MSB
                state.param_msb = val;
                state.is_rpn = true;
                state.param_set = false;
                break;

            case 100: // RPN LSB
                state.param_lsb = val;
                state.is_rpn = true;
                state.param_set = true;
                break;

            case 99: // NRPN MSB
                state.param_msb = val;
                state.is_rpn = false;
                state.param_set = false;
                break;

            case 98: // NRPN LSB
                state.param_lsb = val;
                state.is_rpn = false;
                state.param_set = true;
                break;

            case 6: // Data Entry MSB
                state.value_msb = val;
                break;

            case 38: // Data Entry LSB
                if (state.param_set) {
                    uint16_t param = combine(state.param_msb, state.param_lsb);
                    uint16_t value = combine(state.value_msb, val);
                    if (state.is_rpn) {
                        if (on_rpn) on_rpn(channel, param, value);
                    } else {
                        if (on_nrpn) on_nrpn(channel, param, value);
                    }
                }
                break;

            case 96: // Increment
                if (state.param_set) {
                    uint16_t param = combine(state.param_msb, state.param_lsb);
                    if (on_increment) on_increment(channel, param, state.is_rpn);
                }
                break;

            case 97: // Decrement
                if (state.param_set) {
                    uint16_t param = combine(state.param_msb, state.param_lsb);
                    if (on_decrement) on_decrement(channel, param, state.is_rpn);
                }
                break;

            default:
                break;
        }
    }

    /// Reset all channel state.
    void reset() {
        for (auto& s : channel_state_) s = {};
    }

private:
    struct ChannelState {
        uint8_t param_msb = 0;
        uint8_t param_lsb = 0;
        uint8_t value_msb = 0;
        bool is_rpn = true;
        bool param_set = false;
    };

    ChannelState channel_state_[16]{};

    static uint16_t combine(uint8_t msb, uint8_t lsb) {
        return static_cast<uint16_t>((msb << 7) | lsb);
    }
};

} // namespace pulp::midi
