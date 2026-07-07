#pragma once

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/musical_typing.hpp>

#include <functional>
#include <span>
#include <vector>

namespace pulp::view {

// Frame indices for the two mode frames (DesignFrameView::set_active_frame
// targets). Frame 0 is the typing keyboard, frame 1 the piano keyboard.
inline constexpr int kTypingFrame = 0;
inline constexpr int kPianoFrame = 1;

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
// It is NOT a hand-painted widget: it renders TWO faithful, Figma-exported SVGs
// 1:1 through DesignFrameView (SkSVGDOM) — one per Mode (typing = node 187:15,
// piano = node 187:349), lowered via the faithful-vector lane
// (tools/import-design/figma_rest_export.py). The toggle swaps which frame
// renders (DesignFrameView::set_active_frame) and the view's intrinsic size.
// Reskin via that lane (re-export → re-embed), not by hand.
class MusicalTypingKeyboard : public DesignFrameView {
public:
    MusicalTypingKeyboard();

    // The two keyboard modes. typing = the QWERTY/computer-typing keyboard
    // (frame 0); piano = the full piano keyboard (frame 1). The 🎹/⌨ toggle
    // buttons baked into each frame switch between them on click; set_mode does
    // it programmatically. Switching REDRAWS the content and changes the
    // reported intrinsic size (typing is taller than piano) and requests a
    // re-layout — a host that sizes to intrinsic_width()/height() resizes to
    // match; a fixed-bounds host fits the new frame into its current size.
    enum class Mode { typing = kTypingFrame, piano = kPianoFrame };
    void set_mode(Mode mode);
    Mode mode() const;

    // Notes produced by typing (computer keyboard) OR clicking the keys.
    // `velocity` is 0..1. Wire to a synth/sampler note sink.
    std::function<void(int note, float velocity)> on_note_on;
    std::function<void(int note)> on_note_off;

    // The owned QWERTY→note controller (base note, octave, velocity). Exposed so
    // a host can set the base note / velocity or feed keys from its own path.
    MusicalTypingController& controller() { return controller_; }

    // ── On-screen control callbacks (Logic-faithful) ─────────────────────────
    // The on-screen octave −/+ and velocity −/+ buttons drive the controller
    // directly (they change the next note's octave/velocity, no callback needed).
    // These surface the controls that DON'T map to controller state. Keys 1–8 and
    // tab mirror the on-screen buttons exactly:
    //   • on_pitch_bend(−1..+1) — keys 1 / 2 (and the −/+ pads) are MOMENTARY:
    //     hold → full bend down (−1) / up (+1), release → 0 (spring to centre).
    //     Bipolar; the host maps it to its bend range. Readout shows −20 / 0 / +20.
    //   • on_sustain(bool)      — sustain pad / tab key, MOMENTARY hold: true while
    //     held, false on release (lit while held).
    //   • on_modulation(0..1)   — keys 3–8 (and the 6 mod pads) are a LATCHED
    //     selector: 3 = off (0, default) … 8 = max (1). The selection persists and
    //     highlights; 0..1 is mod_sel / 5.
    std::function<void(float bend)> on_pitch_bend;
    std::function<void(bool on)> on_sustain;
    std::function<void(float amount)> on_modulation;

    // Fired when the toggle (or set_mode) swaps frames and the intrinsic size
    // changes — piano mode (732×176) is shorter than typing (732×266). A host
    // that sizes itself to the keyboard wires this to resize its window/pane to
    // (w, h) (e.g. WindowHost::request_content_size + set_design_viewport), so
    // the piano frame shrinks the window top-aligned and toggling back grows it.
    // Carries the NEW frame's panel width/height.
    std::function<void(float w, float h)> on_intrinsic_size_changed;

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

    // Set the controller's base note (e.g. a sampler's root) AND refresh the
    // OCTAVE readout + overview highlight. Prefer this over controller().
    // set_base_note(), which leaves the readout showing the previous octave.
    void set_base_note(int note);

    // Computer-keyboard playing while focused (when input_capture is on):
    // a w s e d f t g y h u j k o l p play notes, z/x shift the octave. Returns
    // true when consumed (false when capture is off, so the host keeps the key).
    bool on_key_event(const KeyEvent& event) override;
    void on_focus_changed(bool gained) override;  // release held notes on blur

    // ── Overview-strip octave control (Logic-style) ──────────────────────────
    // The top overview strip's teal highlight is the canonical octave indicator:
    // it tracks the octave (z/x, the < > arrows, AND dragging it), always snapping
    // to a C boundary (each octave step is a C). Drag anywhere on the strip to set
    // the octave; the cursor shows grab/grabbing over it. The highlight is drawn
    // as a native overlay (the baked teal box is suppressed) so it can move.
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_hover_move(Point pos) override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

    // MIDI note an element maps to: in the typing frame, base+octave+semitone; in
    // the piano frame, the windowed note (frame-low-note 48 offset onto the live
    // octave window). Public so callers/tests can resolve a key's current pitch.
    int midi_for_element(int index) const;

protected:
    // On a mode swap (toggle button or set_mode): release any QWERTY-held notes
    // (so a held key can't sound forever after the typing frame goes away) and
    // re-apply the external held-note set to the NEW frame's keys (so a host's
    // sustained chord doesn't visually vanish across the swap).
    void on_active_frame_changed() override;

private:
    MusicalTypingController controller_;
    bool input_capture_ = true;   // false = host feeds QWERTY; we don't capture keys
    bool sustain_ = false;        // sustain-pad toggle state (surfaced via on_sustain)
    int pb_value_ = 0;            // live pitch-bend display value (−20 / 0 / +20)
    int mod_sel_ = 0;             // latched modulation selection (0 = "off" … 5 = "max")
    bool dragging_strip_ = false; // mid drag-to-octave on the overview strip
    static constexpr float kVelStep = 1.0f / 16.0f;  // on-screen velocity −/+ increment
    static constexpr int kPitchBendMax = 20;         // Logic-style ±20 readout extent

    // The overview strip is a full-range (C-2…G8 / MIDI 0–127) mini-piano ruler
    // drawn procedurally in paint() over the design's strip background — white-key
    // dividers, black-key marks, C-only labels, and the teal highlight that spans
    // the current playable octave window. The baked partial ribbon is covered.
    // Piano tab: lowest MIDI of the visible 3-octave (36-key) window. The window
    // slides over the full C-2…G8 range as the octave moves and CLAMPS at the top
    // so it ends on G8 (MIDI 127) — a partial top, like Logic (the keys shift, not
    // just the labels). The typing tab is unaffected (it keeps its narrow playable
    // window). Used to remap the piano keys' MIDI, the labels, and the highlight.
    int piano_window_lo() const;
    // The strip's horizontal extent (panel coords) for the active frame — the
    // ribbon is centered in the toolbar and is a different width per frame (the
    // typing/piano toolbars centered their own natural-width ribbons after #82).
    void strip_bounds(float& x0, float& x1) const;
    // Panel-x → octave shift, snapped to the nearest octave (each step is a C
    // boundary — "always snaps to a C range"), clamped to the controller's ±4.
    int octave_for_strip_x(float panel_x) const;
    // If `pos` (view-local) is over the strip band, set `panel_x` to its panel-x
    // and return true. Uses the shared panel_transform (panel origin is 0,0).
    bool point_over_strip(Point pos, float& panel_x) const;
    // Refresh the live OCTAVE / VEL / PITCH BEND value_label readouts of the
    // active frame from current state. Called after any change + on frame swap.
    void update_readouts();
    // Press/release of a tagged control (pitch-bend pb_down/pb_up momentary,
    // modulation mod_N latched, sustain hold) — shared by the mouse-gesture and
    // keyboard paths so on-screen buttons and keys behave identically.
    void control_press(const std::string& tag);
    void control_release(const std::string& tag);
    // Re-light the selected modulation button (mod_sel_) and clear the others;
    // call after a press auto-clears the momentary light on release.
    void refresh_mod_lights();
    // Index of the (first) element whose action tag == `tag`, or -1.
    int element_for_action(const std::string& tag) const;
    // Light (on) or clear (off) every momentary control with `tag` — the tap-flash
    // for octave/velocity/arrow buttons (mouse press or the z/x·c/v keys).
    void flash_action(const std::string& tag, bool on);
    // Last external held set from set_active_notes; re-applied after a frame swap
    // so the new frame reflects the host's still-held notes.
    std::vector<int> held_notes_;
    // Light every momentary key in the ACTIVE frame whose absolute MIDI is in
    // `held_notes_`; clear the rest. Shared by set_active_notes + frame swaps.
    void apply_held_notes();
    // Typing element (note == relative semitone 0..17) for a QWERTY semitone, or -1.
    int typing_element_for_semitone(int semitone) const;
    void light_typing_semitone(int semitone, bool on);
};

}  // namespace pulp::view
