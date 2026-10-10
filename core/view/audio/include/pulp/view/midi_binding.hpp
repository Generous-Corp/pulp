#pragma once

/// @file midi_binding.hpp
/// Bind a UI control to emit MIDI through a `pulp::midi::MidiControlBus`.
///
/// `bind_midi_cc(knob, bus, channel, cc)` makes the control send a MIDI
/// Control Change as the user moves it; `bind_midi_note(button, …)` plays
/// a note while a toggle is on. The plugin's processor drains the bus into
/// its MIDI-out buffer each block, so the moves land in the host's MIDI
/// lane. This complements `bind_parameter` (which drives plugin
/// parameters); a control can do either, or — by binding both — both.
///
/// One-way (UI → MIDI). UI-thread only.

#include <pulp/midi/midi_control_bus.hpp>
#include <pulp/view/widgets.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::view {

namespace detail {
inline uint8_t to_cc_value(float normalized) {
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 127.0f));
}
} // namespace detail

/// Knob → MIDI CC.
inline void bind_midi_cc(Knob& knob, midi::MidiControlBus& bus, uint8_t channel, uint8_t cc) {
    knob.on_change = [&bus, channel, cc](float v) {
        bus.send_cc(channel, cc, detail::to_cc_value(v));
    };
}

/// Fader → MIDI CC.
inline void bind_midi_cc(Fader& fader, midi::MidiControlBus& bus, uint8_t channel, uint8_t cc) {
    fader.on_change = [&bus, channel, cc](float v) {
        bus.send_cc(channel, cc, detail::to_cc_value(v));
    };
}

/// Range slider → MIDI CC.
inline void bind_midi_cc(RangeSlider& slider, midi::MidiControlBus& bus, uint8_t channel, uint8_t cc) {
    slider.on_change = [&bus, channel, cc](float v) {
        bus.send_cc(channel, cc, detail::to_cc_value(v));
    };
}

/// Toggle button → MIDI CC (0 / 127).
inline void bind_midi_cc(ToggleButton& button, midi::MidiControlBus& bus, uint8_t channel, uint8_t cc) {
    button.on_toggle = [&bus, channel, cc](bool on) {
        bus.send_cc(channel, cc, on ? 127 : 0);
    };
}

/// Toggle → MIDI CC (0 / 127).
inline void bind_midi_cc(Toggle& toggle, midi::MidiControlBus& bus, uint8_t channel, uint8_t cc) {
    toggle.on_toggle = [&bus, channel, cc](bool on) {
        bus.send_cc(channel, cc, on ? 127 : 0);
    };
}

/// Toggle button → MIDI note: note-on while on, note-off when off.
inline void bind_midi_note(ToggleButton& button, midi::MidiControlBus& bus,
                           uint8_t channel, uint8_t note, uint8_t velocity = 100) {
    button.on_toggle = [&bus, channel, note, velocity](bool on) {
        if (on) bus.send_note_on(channel, note, velocity);
        else bus.send_note_off(channel, note);
    };
}

/// Toggle → MIDI note: note-on while on, note-off when off.
inline void bind_midi_note(Toggle& toggle, midi::MidiControlBus& bus,
                           uint8_t channel, uint8_t note, uint8_t velocity = 100) {
    toggle.on_toggle = [&bus, channel, note, velocity](bool on) {
        if (on) bus.send_note_on(channel, note, velocity);
        else bus.send_note_off(channel, note);
    };
}

} // namespace pulp::view
