#pragma once

/// @file keyboard_state.hpp
/// Polyphonic key tracking with velocity and listener callbacks.

#include <pulp/midi/message.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::midi {

/// Tracks the state of all 128 MIDI keys across 16 channels.
///
/// Processes note on/off events and maintains per-key velocity.
/// Optional listeners are called on state changes for UI updates
/// (e.g. on-screen keyboard highlighting).
///
/// @code
/// MidiKeyboardState keys;
/// keys.on_note_on = [&](uint8_t ch, uint8_t note, uint8_t vel) {
///     highlight_key(note, true);
/// };
/// keys.on_note_off = [&](uint8_t ch, uint8_t note) {
///     highlight_key(note, false);
/// };
///
/// // In process():
/// for (auto& event : midi_buffer) keys.process(event);
///
/// // Query:
/// if (keys.is_note_on(0, 60)) { /* middle C held */ }
/// @endcode
class MidiKeyboardState {
public:
    /// Called when a note turns on.
    std::function<void(uint8_t channel, uint8_t note, uint8_t velocity)> on_note_on;
    /// Called when a note turns off.
    std::function<void(uint8_t channel, uint8_t note)> on_note_off;

    /// Process a MIDI event. Only note on/off are tracked.
    void process(const MidiEvent& event) {
        auto msg = event.message;

        if (msg.isNoteOn()) {
            uint8_t ch = msg.getChannel0to15();
            uint8_t note = msg.getNoteNumber();
            uint8_t vel = msg.getVelocity();

            if (vel == 0) {
                // Note on with velocity 0 = note off
                set_key(ch, note, 0);
                if (on_note_off) on_note_off(ch, note);
            } else {
                set_key(ch, note, vel);
                if (on_note_on) on_note_on(ch, note, vel);
            }
        } else if (msg.isNoteOff()) {
            uint8_t ch = msg.getChannel0to15();
            uint8_t note = msg.getNoteNumber();
            set_key(ch, note, 0);
            if (on_note_off) on_note_off(ch, note);
        }
    }

    /// Check if a specific note is currently held on a channel.
    bool is_note_on(uint8_t channel, uint8_t note) const {
        return velocity(channel, note) > 0;
    }

    /// Get the velocity of a held note (0 if not held).
    uint8_t velocity(uint8_t channel, uint8_t note) const {
        if (channel >= 16 || note >= 128) return 0;
        return state_[channel][note];
    }

    /// Count how many notes are currently held on a channel.
    int notes_held(uint8_t channel) const {
        if (channel >= 16) return 0;
        int count = 0;
        for (int i = 0; i < 128; ++i) {
            if (state_[channel][i] > 0) ++count;
        }
        return count;
    }

    /// Count total notes held across all channels.
    int total_notes_held() const {
        int count = 0;
        for (int ch = 0; ch < 16; ++ch) count += notes_held(static_cast<uint8_t>(ch));
        return count;
    }

    /// Check if any note is held on any channel.
    bool any_notes_held() const {
        for (int ch = 0; ch < 16; ++ch) {
            for (int n = 0; n < 128; ++n) {
                if (state_[ch][n] > 0) return true;
            }
        }
        return false;
    }

    /// Get the lowest held note on a channel (-1 if none).
    int lowest_note(uint8_t channel) const {
        if (channel >= 16) return -1;
        for (int i = 0; i < 128; ++i) {
            if (state_[channel][i] > 0) return i;
        }
        return -1;
    }

    /// Get the highest held note on a channel (-1 if none).
    int highest_note(uint8_t channel) const {
        if (channel >= 16) return -1;
        for (int i = 127; i >= 0; --i) {
            if (state_[channel][i] > 0) return i;
        }
        return -1;
    }

    /// Release all notes on all channels.
    void all_notes_off() {
        for (int ch = 0; ch < 16; ++ch) {
            for (int n = 0; n < 128; ++n) {
                if (state_[ch][n] > 0) {
                    state_[ch][n] = 0;
                    if (on_note_off) on_note_off(static_cast<uint8_t>(ch),
                                                  static_cast<uint8_t>(n));
                }
            }
        }
    }

    /// Release all notes on a specific channel.
    void all_notes_off(uint8_t channel) {
        if (channel >= 16) return;
        for (int n = 0; n < 128; ++n) {
            if (state_[channel][n] > 0) {
                state_[channel][n] = 0;
                if (on_note_off) on_note_off(channel, static_cast<uint8_t>(n));
            }
        }
    }

    /// Reset all state without firing callbacks.
    void reset() {
        for (auto& ch : state_) ch.fill(0);
    }

private:
    std::array<std::array<uint8_t, 128>, 16> state_{};

    void set_key(uint8_t ch, uint8_t note, uint8_t vel) {
        if (ch < 16 && note < 128) state_[ch][note] = vel;
    }
};

} // namespace pulp::midi
