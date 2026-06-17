#pragma once

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/musical_typing.hpp>

#include <functional>
#include <span>

namespace pulp::view {

// ── MusicalTypingKeyboard ────────────────────────────────────────────────────
// Ink & Signal "Musical Typing Keyboard" catalog component (Category::audio).
//
// THE playable computer-typing + piano keyboard primitive. Use THIS for a
// "musical typing keyboard" — NOT MidiKeyboard (the plain piano strip). It is
// fully wired:
//   • computer keyboard → notes (a w s e d f t g y h u j k o l p), z/x octave —
//     via an owned MusicalTypingController, while the view has keyboard focus;
//   • clicking either the typing row OR the lower piano row plays + lights keys;
//   • pressed keys light with the accent gradient (white + black), per design.
// Wire on_note_on / on_note_off to a synth/sampler; everything else is internal.
//
// It is NOT a hand-painted widget: it renders the faithful, Figma-exported SVG
// 1:1 through DesignFrameView (SkSVGDOM), lowered from Figma node 187:2 via the
// faithful-vector lane (tools/import-design/figma_rest_export.py). Reskin via
// that lane (re-export → re-embed), not by hand.
class MusicalTypingKeyboard : public DesignFrameView {
public:
    MusicalTypingKeyboard();

    // Notes produced by typing (computer keyboard) OR clicking the keys.
    // `velocity` is 0..1. Wire to a synth/sampler note sink.
    std::function<void(int note, float velocity)> on_note_on;
    std::function<void(int note)> on_note_off;

    // The owned QWERTY→note controller (base note, octave, velocity). Exposed so
    // a host can set the base note / velocity or feed keys from its own path.
    MusicalTypingController& controller() { return controller_; }

    // ── Host-driven integration (e.g. PulpTempoSampler) ──────────────────────
    // Light keys from an EXTERNAL held-note set — host MIDI, an app-wide QWERTY
    // monitor, clicks elsewhere. Absolute MIDI notes; a note lights every key
    // that maps to it (the typing key at the current octave AND the piano key).
    // This REPLACES the lit display, so the component never relies solely on its
    // own key/mouse state. Pass an empty span to clear. Use when a host drives
    // notes (typically with set_input_capture(false)).
    void set_active_notes(std::span<const int> midi_notes);

    // When false, the component does NOT capture computer-keyboard events, so a
    // host that already feeds QWERTY (app-wide monitor → its own controller)
    // won't double-trigger. It still plays + lights on CLICK and from
    // set_active_notes. Default true (standalone playable). Also drops keyboard
    // focusability so it won't steal first-responder key events.
    void set_input_capture(bool capture);
    bool input_capture() const { return input_capture_; }

    // Computer-keyboard playing while focused (when input_capture is on):
    // a w s e d f t g y h u j k o l p play notes, z/x shift the octave. Returns
    // true when consumed (false when capture is off, so the host keeps the key).
    bool on_key_event(const KeyEvent& event) override;
    void on_focus_changed(bool gained) override;  // release held notes on blur

private:
    MusicalTypingController controller_;
    bool input_capture_ = true;   // false = host feeds QWERTY; we don't capture keys
    // Typing element (note == relative semitone 0..15) for a QWERTY semitone, or -1.
    int typing_element_for_semitone(int semitone) const;
    // MIDI note an element maps to: typing keys = base+octave+semitone, piano
    // keys = their absolute note. Used so clicks emit the right note.
    int midi_for_element(int index) const;
    void light_typing_semitone(int semitone, bool on);
};

}  // namespace pulp::view
