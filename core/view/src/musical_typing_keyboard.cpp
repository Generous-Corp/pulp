#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace pulp::view {

namespace detail {
const char* musical_typing_typing_svg_b64();  // musical_typing_keyboard_svg.cpp
const char* musical_typing_piano_svg_b64();
}

namespace {
// Decode an embedded faithful SVG once. Host-side construction; never the
// audio/render thread.
std::string decode(const char* b64) {
    if (auto bytes = runtime::base64_decode(b64))
        return std::string(bytes->begin(), bytes->end());
    return {};
}

// Decode an embedded SVG and remove the BAKED teal overview-highlight box (a
// filtered <g> whose first drawn coord is the box's top-left). The live overlay
// (MusicalTypingKeyboard::paint) owns that highlight so it can move with the
// octave — a frozen baked box can't. `box_x` is the box left edge in panel
// coords (typing 306.68, piano 331.008); the box top sits at y≈21–24.
std::string decode_no_strip_box(const char* b64, float box_x) {
    std::string svg = decode(b64);
    suppress_svg_glow_at(svg, box_x - 1.5f, 20.0f, 5.0f, 6.0f);
    return svg;
}

// Piano frame: also suppress the baked C2/C3/C4 key labels — they're static, but
// the visible range is a sliding window, so MusicalTypingKeyboard::paint draws
// dynamic "Cn" labels under whichever keys are Cs in the current window.
std::string decode_piano() {
    std::string svg = decode_no_strip_box(detail::musical_typing_piano_svg_b64(), 322.418f);
    for (float lx : {41.4f, 266.7f, 492.1f})
        suppress_svg_glyph_at(svg, lx - 2.0f, 125.0f, 9.0f, 10.0f);   // baked C2/C3/C4
    return svg;
}

// The two 🎹/⌨ toggle icon-buttons baked into BOTH mode frames' toolbars, in
// each frame's own (732-wide) coordinate space — they sit at the same spot in
// both. The left (piano icon) swaps to the piano frame; the right (keyboard
// icon) swaps to the typing frame. Modeled as swap-link elements so a click
// calls DesignFrameView::set_active_frame(target).
void append_toggle(std::vector<DesignFrameElement>& els) {
    auto add_swap = [&](float x, int target) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::swap;
        e.x = x; e.y = 22; e.w = 36; e.h = 26;
        e.target_frame = target;
        els.push_back(e);
    };
    add_swap(24, kPianoFrame);   // piano icon → piano mode (frame 1)
    add_swap(62, kTypingFrame);  // keyboard icon → typing mode (frame 0)
}

// The typing frame's on-screen controls, in frame-0 (732×266) coords from Figma
// node 187:15. Two kinds:
//   • Kind::action — octave −/+ and velocity −/+ (bottom Z/X · C/V buttons) and
//     the < > arrows flanking the overview strip. A click fires on_action(id),
//     which drives the controller (octave/velocity).
//   • Kind::momentary (with an action tag) — sustain pad, pitch-bend −/+ pads
//     (keys 1/2), and the 6 modulation pads (keys 3–8). These light via the key
//     overlay and route to control_press/control_release (pitch-bend momentary,
//     sustain hold, modulation latched), mirroring the number-row keys.
void append_controls(std::vector<DesignFrameElement>& els) {
    // ALL on-screen controls are MOMENTARY with an action tag, so they light on
    // press and clear on release (a tap-flash) via the existing key overlay, and
    // route through control_press/control_release. octave/velocity/< > fire their
    // step on press; pitch-bend 1/2 are momentary holds; modulation 3–8 latch;
    // sustain holds. (Tagged momentary carry note == −1 → never play a note.)
    auto add_mom = [&](std::string id, float x, float y, float w, float h) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.action = std::move(id);   // control tag (routes away from notes)
        e.x = x; e.y = y; e.w = w; e.h = h;
        els.push_back(e);
    };
    add_mom("octave_down", 114, 205, 32, 32);
    add_mom("octave_up",   155, 205, 32, 32);
    // The < > buttons flanking the (now centered) top overview strip step the
    // octave. After #82 centered the strip, < ≈ chevron 150 → element x=139,
    // > ≈ chevron 582 → element x=571 (typing).
    add_mom("octave_down", 139, 17, 22, 24);
    add_mom("octave_up",   571, 17, 22, 24);
    add_mom("vel_down",    321, 205, 32, 32);
    add_mom("vel_up",      362, 205, 32, 32);
    // y shifted −8 vs the pre-#82 export (the toolbar shrank when the top-right
    // readouts were removed, lifting every below-toolbar row by 8px).
    add_mom("sustain", 21, 102, 66, 92);
    // pitch-bend + modulation buttons: match the baked button rect exactly
    // (y[53,90], h=37) so the press highlight fills the WHOLE button — pixel-
    // measured from the rendered SVG; an earlier h=32 left the bottom ~5px of
    // the pad uncovered (a dark band under the lit teal).
    add_mom("pb_down", 108, 53, 36, 37);   // "−" / key 1
    add_mom("pb_up",   150, 53, 36, 37);   // "+" / key 2
    static const float mx[] = {200, 242, 284, 326, 368, 410};
    for (int i = 0; i < 6; ++i)
        add_mom("mod_" + std::to_string(i), mx[i], 53, 36, 37);
}

// Live value readouts (Kind::value_label) over the design's baked OCTAVE / VEL /
// PITCH BEND numbers. The `action` field tags which value each shows so
// update_readouts() can refresh them. Rects are the baked glyph boxes (build
// suppresses the frozen glyphs there). `who` = "typing" (full set) or "piano"
// (octave only — the piano toolbar shows just OCTAVE).
void append_readouts(std::vector<DesignFrameElement>& els, const char* who) {
    auto add = [&](std::string tag, float x, float y, float w, float h, bool left = false) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::value_label;
        e.action = std::move(tag);   // reused as the readout id
        e.x = x; e.y = y; e.w = w; e.h = h;
        e.value_left_align = left;
        els.push_back(e);
    };
    // The redundant top-right OCTAVE/VEL cluster was removed from the design
    // (#82): the bottom-left readouts are the single source now. Piano shows no
    // OCTAVE readout — the overview highlight reflects the range (Logic-style).
    if (std::string(who) == "typing") {
        // All three readouts are RIGHT-aligned with a fixed right edge that sits in
        // the gap between the baked label and the next control, so a wide reading
        // ("C-2", "106", "−20") lines up on the right and never jumps left over its
        // label. The value_label renderer auto-shrinks any reading wider than its
        // rect, so a 3-char value fits the gap instead of overflowing onto the
        // neighboring button/label. (Boxes widened from the 1–2 char baked slots.)
        add("octave",   72, 211, 34, 21);   // "OCTAVE C2/C-2"  right edge 106 (z pad @114)
        add("velocity", 284, 211, 34, 21);  // "VELOCITY 98/106" right edge 318 (c pad @321)
        add("pitchbend", 76, 66, 30, 18);   // "PITCH BEND 0/−20" right edge 106 (1-pad @108)
    }
    // piano: no value_labels (range shown by the overview highlight only)
}

// MIDI note → pitch-class + octave, in the design's convention where C2 = 48
// (so octave = midi/12 − 2): 48→"C2", 60→"C3", 36→"C1".
std::string note_name(int midi) {
    static const char* kPc[] = {"C", "C#", "D", "D#", "E", "F",
                                "F#", "G", "G#", "A", "A#", "B"};
    midi = std::clamp(midi, 0, 127);
    return std::string(kPc[midi % 12]) + std::to_string(midi / 12 - 2);
}

// Typing-mode playable keys, frame-0 (732×266) coords extracted from Figma node
// 187:15. `note` is the relative semitone (0..17) in the Logic-style
// "a w s e d f t g y h u j k o l p ; '" row. Black keys are narrower/shorter, so
// DesignFrameView's smallest-area hit tiebreak picks them over the white key
// they overlap.
std::vector<DesignFrameElement> build_typing_frame() {
    struct K { int note; float x, y, w, h; };
    static const K keys[] = {
        {0, 102, 109, 55, 78}, {1, 142, 109, 30, 54}, {2, 157, 109, 55, 78},
        {3, 197, 109, 30, 54}, {4, 212, 109, 55, 78}, {5, 267, 109, 55, 78},
        {6, 307, 109, 30, 54}, {7, 322, 109, 55, 78}, {8, 362, 109, 30, 54},
        {9, 377, 109, 55, 78}, {10, 416, 109, 30, 54}, {11, 431, 109, 55, 78},
        {12, 486, 109, 55, 78}, {13, 526, 109, 30, 54}, {14, 541, 109, 55, 78},
        {15, 581, 109, 30, 54}, {16, 596, 109, 55, 78}, {17, 651, 109, 55, 78},
    };
    std::vector<DesignFrameElement> els;
    for (const auto& k : keys) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.note = k.note;
        e.x = k.x; e.y = k.y; e.w = k.w; e.h = k.h;
        els.push_back(e);
    }
    append_toggle(els);
    append_controls(els);   // octave / velocity / sustain / pitch-bend (typing only)
    append_readouts(els, "typing");   // live OCTAVE / VEL / PITCH BEND values
    return els;
}

// Piano-mode playable keys, frame-1 (732×176) coords extracted from Figma node
// 187:349. `note` is the ABSOLUTE MIDI number (C2=48 … B4=83). Black keys are
// narrower/shorter so the smallest-area hit tiebreak picks them.
// The piano frame's lowest baked key carries note 48; all 36 keys are stored as
// ascending absolute notes 48..83. midi_for_element maps them onto the live
// window relative to THIS frame-intrinsic low note (NOT the runtime base note),
// so the visible window is correct regardless of the controller's base — a
// sampler root of C3 (60) must not shift the window off the C-2..G8 range.
constexpr int kPianoFrameLowNote = 48;

std::vector<DesignFrameElement> build_piano_frame() {
    struct K { int note; float x, y, w, h; };
    static const K keys[] = {
        {48, 28, 62, 32, 78}, {49, 50, 56, 22, 58}, {50, 60, 62, 32, 78},
        {51, 82, 56, 22, 58}, {52, 92, 62, 32, 78}, {53, 125, 62, 32, 78},
        {54, 146, 56, 22, 58}, {55, 157, 62, 32, 78}, {56, 178, 56, 22, 58},
        {57, 189, 62, 32, 78}, {58, 210, 56, 22, 58}, {59, 221, 62, 32, 78},
        {60, 253, 62, 32, 78}, {61, 274, 56, 22, 58}, {62, 286, 62, 32, 78},
        {63, 306, 56, 22, 58}, {64, 318, 62, 32, 78}, {65, 350, 62, 32, 78},
        {66, 371, 56, 22, 58}, {67, 382, 62, 32, 78}, {68, 404, 56, 22, 58},
        {69, 414, 62, 32, 78}, {70, 436, 56, 22, 58}, {71, 446, 62, 32, 78},
        {72, 479, 62, 32, 78}, {73, 500, 56, 22, 58}, {74, 511, 62, 32, 78},
        {75, 532, 56, 22, 58}, {76, 543, 62, 32, 78}, {77, 575, 62, 32, 78},
        {78, 596, 56, 22, 58}, {79, 607, 62, 32, 78}, {80, 628, 56, 22, 58},
        {81, 640, 62, 32, 78}, {82, 660, 56, 22, 58}, {83, 672, 62, 32, 78},
    };
    std::vector<DesignFrameElement> els;
    for (const auto& k : keys) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.note = k.note;
        e.x = k.x; e.y = k.y; e.w = k.w; e.h = k.h;
        els.push_back(e);
    }
    append_toggle(els);
    // The < > buttons flanking the piano overview strip step the octave.
    auto add_oct = [&](std::string id, float x) {
        DesignFrameElement e; e.kind = DesignFrameElement::Kind::momentary;  // tap-flash
        e.action = std::move(id); e.x = x; e.y = 17; e.w = 22; e.h = 24;
        els.push_back(e);
    };
    add_oct("octave_down", 109);   // piano < chevron ≈120 → element x=109 (centered)
    add_oct("octave_up", 601);     // piano > chevron ≈612 → element x=601
    append_readouts(els, "piano");   // live OCTAVE value (piano toolbar)
    return els;
}
}  // namespace

MusicalTypingKeyboard::MusicalTypingKeyboard()
    // Frame 0 = typing mode (732×266). Pass the panel explicitly = the whole
    // frame so the standalone export shows edge-to-edge (no detect_panel guess).
    : DesignFrameView(decode_no_strip_box(detail::musical_typing_typing_svg_b64(), 306.68f),
                      build_typing_frame(), 0, 0, 732, 266) {
    // Frame 1 = piano mode (732×176).
    add_frame(decode_piano(), build_piano_frame(), 0, 0, 732, 176);

    set_focusable(true);            // accept computer-keyboard focus for typing
    // The keyboard renders its faithful SVG via Canvas::draw_svg (Skia), which
    // only composites on the GPU window host — on a CPU-only host the panel is
    // blank. Declaring this lets an embedding app (e.g. a sampler) host it on
    // the GPU surface automatically, without having to know the requirement.
    set_requires_gpu_host(true);
    controller_.set_base_note(48);  // 'a' = C2, matching the design's OCTAVE C2

    // Computer keyboard → notes (controller) forwarded to the public note sink.
    controller_.on_note_on  = [this](int note, float vel) { if (on_note_on) on_note_on(note, vel); };
    controller_.on_note_off = [this](int note)            { if (on_note_off) on_note_off(note); };

    // A gesture on a momentary element is either a NOTE key (note >= 0, empty
    // action) or a tagged CONTROL (pitch-bend / modulation / sustain — action set,
    // note = −1). Route controls to control_press/release so on-screen buttons
    // behave identically to the number-row keys; play notes otherwise. (Swap-link
    // toggle clicks are handled inside DesignFrameView and never reach here.)
    on_gesture_begin = [this](int i) {
        const std::string& tag = element_action(i);
        if (!tag.empty()) { control_press(tag); return; }
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_on) on_note_on(n, controller_.velocity);
    };
    on_gesture_end = [this](int i) {
        const std::string& tag = element_action(i);
        if (!tag.empty()) { control_release(tag); return; }
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_off) on_note_off(n);
    };

    // ALL on-screen controls are now tagged momentary (handled by the gesture
    // path above → control_press/control_release), so there's no Kind::action
    // element and no on_action handler: octave/velocity steps, the < > arrows,
    // pitch-bend, modulation, and sustain all route through control_press, which
    // also gives them a press tap-flash.

    controller_.velocity = 98.0f / 127.0f;  // match the design's "VEL 98" default
    refresh_mod_lights();                    // light the default modulation step (off)
    update_readouts();                       // seed the readouts to current state
}

void MusicalTypingKeyboard::update_readouts() {
    const std::string oct = note_name(controller_.base_note() + controller_.octave_shift() * 12);
    const int vel127 = static_cast<int>(std::lround(controller_.velocity * 127.0f));
    // Logic-style signed pitch-bend readout: −20 / 0 / +20.
    const std::string pb = pb_value_ > 0 ? "+" + std::to_string(pb_value_)
                                         : std::to_string(pb_value_);
    for (int i = 0; i < element_count(); ++i) {
        if (element_kind(i) != DesignFrameElement::Kind::value_label) continue;
        const std::string& tag = element_action(i);
        if (tag == "octave")         set_element_text(i, oct);
        else if (tag == "velocity")  set_element_text(i, std::to_string(vel127));
        else if (tag == "pitchbend") set_element_text(i, pb);
    }
}

int MusicalTypingKeyboard::element_for_action(const std::string& tag) const {
    for (int i = 0; i < element_count(); ++i)
        if (element_action(i) == tag) return i;
    return -1;
}

void MusicalTypingKeyboard::flash_action(const std::string& tag, bool on) {
    // Light/clear EVERY momentary control with this tag (e.g. octave_down is both
    // the bottom −/+ button AND the < arrow), so a tap or its key flashes them.
    for (int i = 0; i < element_count(); ++i)
        if (element_action(i) == tag) set_element_value(i, on ? 1.0f : 0.0f);
    request_repaint();
}

void MusicalTypingKeyboard::control_press(const std::string& tag) {
    const int e = element_for_action(tag);
    if (tag == "octave_down" || tag == "octave_up") {
        controller_.set_octave_shift(controller_.octave_shift() + (tag == "octave_up" ? 1 : -1));
        flash_action(tag, true);   // light every button with this tag (bottom + arrow)
    } else if (tag == "vel_down" || tag == "vel_up") {
        controller_.velocity = std::clamp(
            controller_.velocity + (tag == "vel_up" ? kVelStep : -kVelStep), 0.0f, 1.0f);
        flash_action(tag, true);
    } else if (tag == "pb_down") {
        pb_value_ = -kPitchBendMax;
        if (on_pitch_bend) on_pitch_bend(-1.0f);   // bipolar −1 (full bend down)
        if (e >= 0) set_element_value(e, 1.0f);
    } else if (tag == "pb_up") {
        pb_value_ = kPitchBendMax;
        if (on_pitch_bend) on_pitch_bend(1.0f);     // bipolar +1 (full bend up)
        if (e >= 0) set_element_value(e, 1.0f);
    } else if (tag == "sustain") {
        sustain_ = true;
        if (on_sustain) on_sustain(true);
        if (e >= 0) set_element_value(e, 1.0f);
    } else if (tag.rfind("mod_", 0) == 0) {
        // Latched selector: pick this step (0 = off … 5 = max) and persist it.
        mod_sel_ = std::clamp(std::atoi(tag.c_str() + 4), 0, 5);
        if (on_modulation) on_modulation(static_cast<float>(mod_sel_) / 5.0f);
        refresh_mod_lights();
    }
    update_readouts();
    request_repaint();
}

void MusicalTypingKeyboard::control_release(const std::string& tag) {
    const int e = element_for_action(tag);
    if (tag == "octave_down" || tag == "octave_up" ||
        tag == "vel_down" || tag == "vel_up") {
        flash_action(tag, false);   // end the tap-flash (the step already fired on press)
    } else if (tag == "pb_down" || tag == "pb_up") {
        pb_value_ = 0;                              // momentary: spring back to center
        if (on_pitch_bend) on_pitch_bend(0.0f);
        if (e >= 0) set_element_value(e, 0.0f);
    } else if (tag == "sustain") {
        sustain_ = false;                           // momentary hold: released on key-up
        if (on_sustain) on_sustain(false);
        if (e >= 0) set_element_value(e, 0.0f);
    } else if (tag.rfind("mod_", 0) == 0) {
        // Modulation LATCHES: the momentary press just auto-cleared this button's
        // light on release — re-light the selected step so the selection persists.
        refresh_mod_lights();
    }
    update_readouts();
    request_repaint();
}

void MusicalTypingKeyboard::refresh_mod_lights() {
    for (int i = 0; i < 6; ++i) {
        const int e = element_for_action("mod_" + std::to_string(i));
        if (e >= 0) set_element_value(e, i == mod_sel_ ? 1.0f : 0.0f);
    }
}

void MusicalTypingKeyboard::set_mode(Mode mode) {
    set_active_frame(static_cast<int>(mode));
}

MusicalTypingKeyboard::Mode MusicalTypingKeyboard::mode() const {
    return static_cast<Mode>(active_frame());
}

void MusicalTypingKeyboard::set_active_notes(std::span<const int> midi_notes) {
    // Remember the host's held set so a frame swap can re-apply it to the new
    // frame's keys (on_active_frame_changed), then light the active frame.
    held_notes_.assign(midi_notes.begin(), midi_notes.end());
    apply_held_notes();
}

void MusicalTypingKeyboard::apply_held_notes() {
    // Light every momentary key (in the ACTIVE frame) whose absolute MIDI is in
    // the external held set; clear the rest. Host-driven display — independent of
    // our own key/mouse state.
    for (int i = 0; i < element_count(); ++i) {
        if (element_kind(i) != DesignFrameElement::Kind::momentary) continue;
        if (element_note(i) < 0) continue;   // control momentary (pitch/mod/sustain) — not a note
        const int midi = midi_for_element(i);
        bool held = false;
        for (int n : held_notes_) if (n == midi) { held = true; break; }
        set_element_value(i, held ? 1.0f : 0.0f);
    }
    request_repaint();
}

void MusicalTypingKeyboard::on_active_frame_changed() {
    // A mode swap (toggle button or set_mode) just changed the active frame.
    // Release any QWERTY-held notes — the typing keys may no longer exist (piano
    // mode), and the design's release-on-mode-switch edge applies here too — then
    // re-light the new frame from the host's still-held external set so a
    // sustained chord doesn't visually vanish across the swap.
    controller_.all_notes_off();
    apply_held_notes();
    refresh_mod_lights();   // re-apply the latched modulation light on the new frame
    update_readouts();      // the new frame's readouts (e.g. piano OCTAVE) reflect state
    // The intrinsic size just changed (piano is shorter) — let a self-sizing host
    // resize its window/pane to the new frame, top-aligned (toggles stay fixed).
    if (on_intrinsic_size_changed) on_intrinsic_size_changed(panel_width(), panel_height());
}

void MusicalTypingKeyboard::set_input_capture(bool capture) {
    input_capture_ = capture;
    set_focusable(capture);          // don't steal first-responder when host feeds keys
    if (!capture) controller_.all_notes_off();  // stop our own QWERTY-held notes
}

void MusicalTypingKeyboard::set_base_note(int note) {
    controller_.set_base_note(note);
    // Re-clamp the shift to the new base's range — set_base_note alone leaves a
    // previously-valid octave_shift that may now fall outside [floor, top] for the
    // new base, which would push the play-window off C-2..G8.
    controller_.set_octave_shift(controller_.octave_shift());
    update_readouts();   // the OCTAVE readout + overview highlight track the base; a
    request_repaint();   // bare controller().set_base_note() would leave them stale.
}

bool MusicalTypingKeyboard::on_key_event(const KeyEvent& event) {
    if (!input_capture_) return false;  // host feeds QWERTY itself — don't double-trigger
    // Number row + tab mirror the on-screen controls exactly: 1/2 = momentary
    // pitch bend (down/up), 3–8 = latched modulation selector (3 = off … 8 = max),
    // tab = momentary sustain hold. Routed through the SAME control_press/release
    // the click path uses, so keys and buttons stay in lockstep.
    std::string tag;
    switch (event.key) {
        case KeyCode::num1: tag = "pb_down"; break;
        case KeyCode::num2: tag = "pb_up";   break;
        case KeyCode::num3: tag = "mod_0";   break;
        case KeyCode::num4: tag = "mod_1";   break;
        case KeyCode::num5: tag = "mod_2";   break;
        case KeyCode::num6: tag = "mod_3";   break;
        case KeyCode::num7: tag = "mod_4";   break;
        case KeyCode::num8: tag = "mod_5";   break;
        case KeyCode::tab:  tag = "sustain"; break;
        default: break;
    }
    if (!tag.empty()) {
        if (event.is_down) { if (!event.is_repeat) control_press(tag); }
        else               control_release(tag);
        return true;
    }
    // z/x/c/v drive octave/velocity in the controller — also flash the matching
    // on-screen button (tap-feedback parity with a mouse press). Held on key-down,
    // cleared on key-up.
    const char* btn = nullptr;
    switch (event.key) {
        case KeyCode::z: btn = "octave_down"; break;
        case KeyCode::x: btn = "octave_up";   break;
        case KeyCode::c: btn = "vel_down";    break;
        case KeyCode::v: btn = "vel_up";      break;
        default: break;
    }
    if (btn) flash_action(btn, event.is_down);

    const bool consumed = controller_.handle_key(event);  // QWERTY→note + z/x octave
    // Light the matching typing key (by relative semitone, octave-independent).
    // No-op in piano mode (the frame has no typing keys), which is correct.
    const int semi = MusicalTypingController::semitone_for_key(event.key);
    if (semi >= 0) {
        if (event.is_down && !event.is_repeat) light_typing_semitone(semi, true);
        else if (!event.is_down)               light_typing_semitone(semi, false);
    }
    // z/x (octave) and c/v (velocity) change controller state the readouts show;
    // octave also moves the overview highlight.
    if (consumed && semi < 0) { update_readouts(); request_repaint(); }
    return consumed;
}

void MusicalTypingKeyboard::on_focus_changed(bool gained) {
    DesignFrameView::on_focus_changed(gained);
    if (!gained) {
        // Lost focus: release our QWERTY-held notes (you can't hold keys while
        // unfocused), then re-apply the external held set so host-driven
        // highlights persist while QWERTY-only highlights clear.
        controller_.all_notes_off();
        apply_held_notes();
    }
}

int MusicalTypingKeyboard::typing_element_for_semitone(int semitone) const {
    if (active_frame() != kTypingFrame) return -1;  // typing keys live in frame 0
    if (semitone < 0) return -1;
    for (int i = 0; i < element_count(); ++i)
        if (element_kind(i) == DesignFrameElement::Kind::momentary && element_note(i) == semitone)
            return i;
    return -1;
}

int MusicalTypingKeyboard::midi_for_element(int index) const {
    const int note = element_note(index);
    if (note < 0) return -1;
    // Discriminate by FRAME, not note magnitude: the typing frame's `note` is a
    // relative semitone (resolved against base + octave); the piano frame's is an
    // absolute MIDI note. (A magnitude threshold would misclassify a low piano
    // octave, e.g. MIDI C-1..B0, as a typing semitone.)
    if (active_frame() == kTypingFrame)
        return controller_.base_note() + controller_.octave_shift() * 12 + note;
    // Piano: the 36 keys are a WINDOW over the full range. Map each key by its
    // offset from the FRAME's intrinsic low note (48), not the runtime base note,
    // so the window stays on C-2..G8 whatever the base is. (Using base_note here
    // shifted the whole window by base-48 — e.g. a C3 root pushed the top down to
    // ~D#7 instead of G8 and desynced the keys from the overview highlight.)
    return piano_window_lo() + (note - kPianoFrameLowNote);
}

int MusicalTypingKeyboard::piano_window_lo() const {
    // Low note of the visible 36-key window, octave-driven, clamped to [C-2(0),
    // G8(127)-35=92] so the top window ends on G8 — a partial top (Logic-style
    // "shift the keys"). Literals here: the ruler constants live further down.
    return std::clamp(controller_.base_note() + controller_.octave_shift() * 12, 0, 92);
}

void MusicalTypingKeyboard::light_typing_semitone(int semitone, bool on) {
    const int i = typing_element_for_semitone(semitone);
    if (i >= 0) { set_element_value(i, on ? 1.0f : 0.0f); request_repaint(); }
}

// ── Overview-strip octave control — full-range mini-piano ruler ─────────────
// The strip is a full C-2…G8 (MIDI 0–127) piano overview drawn procedurally over
// the design's #EBEEF1 strip background: white-key dividers, black-key marks,
// C-only labels, and a teal highlight spanning the current playable window. The
// baked partial ribbon is covered. Both frames share the same strip rect (the
// toolbars reflowed identically after #82 removed their right-side readouts).

namespace {
constexpr float kStripY  = 17.0f,  kStripH  = 24.0f;   // vertical band (post-#82 centering)
constexpr int   kRulerLo = 0, kRulerHi = 127;          // C-2 … G8
constexpr int   kPlaySpan = 17;                        // typing keys a..' = 18 semitones
constexpr int   kPianoSpan = 35;                       // piano frame = 36 keys (C2..B4)

// White-key units from C-2 (MIDI 0): 7 per octave; black keys sit on the
// half-unit between neighbours. Monotonic in MIDI, so it maps the keyboard to an
// even white-key ruler (a piano-roll overview, like Logic's).
float white_units(int midi) {
    static const float wu[12] = {0, 0.5f, 1, 1.5f, 2, 3, 3.5f, 4, 4.5f, 5, 5.5f, 6};
    return 7.0f * (midi / 12) + wu[midi % 12];
}
bool key_is_black(int midi) {
    const int pc = midi % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
float midi_to_x(int midi, float x0, float x1) {
    return x0 + white_units(midi) / white_units(kRulerHi) * (x1 - x0);
}
// Pitch-class + octave label in the C2=48 convention, C keys only ("C2", "C-1").
std::string c_label(int midi) { return "C" + std::to_string(midi / 12 - 2); }
}  // namespace

void MusicalTypingKeyboard::strip_bounds(float& x0, float& x1) const {
    // The ribbon is centered in the toolbar; each frame's natural width differs.
    // Exact strip-background rect edges (the white rounded rect), so the ruler,
    // cover, and highlight fill it fully — at the top window the highlight then
    // reaches the strip's right edge instead of leaving a sliver.
    if (active_frame() == kTypingFrame) { x0 = 168.531f; x1 = 559.469f; }
    else                                { x0 = 138.41f;  x1 = 589.59f; }
}

bool MusicalTypingKeyboard::point_over_strip(Point pos, float& panel_x) const {
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0.0f) return false;
    const float px = (pos.x - t.ox) / t.scale;   // panel origin is (0,0)
    const float py = (pos.y - t.oy) / t.scale;
    panel_x = px;
    float x0, x1; strip_bounds(x0, x1);
    constexpr float pad = 3.0f;
    return px >= x0 && px <= x1 && py >= kStripY - pad && py <= kStripY + kStripH + pad;
}

int MusicalTypingKeyboard::octave_for_strip_x(float panel_x) const {
    float x0, x1; strip_bounds(x0, x1);
    const int base = controller_.base_note();
    // Snap to the octave whose visible-window CENTER is nearest x. The window is
    // wider on the piano tab (3 octaves) than typing (the ~1.5-octave play span).
    const int span = (active_frame() == kTypingFrame) ? kPlaySpan : kPianoSpan;
    const float c0 = (midi_to_x(base, x0, x1) + midi_to_x(base + span, x0, x1)) * 0.5f;
    const float step = midi_to_x(base + 12, x0, x1) - midi_to_x(base, x0, x1);  // one octave
    // Both bounds are base-relative so the window reaches the true range edges
    // whatever the base note is. Bottom: the most-negative shift that lands the
    // window low on C-2 (MIDI 0). Top (piano): enough to bring the window low to
    // G8−35 = MIDI 92, where piano_window_lo saturates so the rightmost key is G8;
    // typing keeps +5 so its 18-key play-window top reaches G8 on the strip. A
    // fixed −4/+4 bottomed out at C-1 and topped out short of G8 for a C3 root.
    const int top = (active_frame() == kTypingFrame) ? 5
                                                     : std::max(0, (92 - base + 11) / 12);
    const int bot = -((base + 11) / 12);
    return std::clamp(static_cast<int>(std::lround((panel_x - c0) / step)), bot, top);
}

void MusicalTypingKeyboard::paint(canvas::Canvas& canvas) {
    DesignFrameView::paint(canvas);   // faithful SVG + key/control highlights
    const auto t = panel_transform(local_bounds());
    if (t.scale <= 0.0f) return;
    float x0, x1; strip_bounds(x0, x1);
    auto vx = [&](float px) { return t.ox + px * t.scale; };
    auto vy = [&](float py) { return t.oy + py * t.scale; };
    // Faithful strip colors — must match the baked SVG exactly (not theme
    // tokens), so they're deliberate literals. token-lint:allow
    const auto bg   = canvas::Color::rgba8(0xEB, 0xEE, 0xF1);          // strip key color  token-lint:allow
    const auto dark = canvas::Color::rgba(0, 0, 0, 0.16f);            // white-key divider  token-lint:allow
    const auto blk  = canvas::Color::rgba(0x16/255.f, 0x19/255.f, 0x1E/255.f, 1.0f);  // token-lint:allow

    // 1) Cover the baked partial ribbon with the strip background (seamless — same
    //    #EBEEF1), so only our full-range ruler shows.
    canvas.set_fill_color(bg);
    canvas.fill_rect(vx(x0), vy(kStripY), (x1 - x0) * t.scale, kStripH * t.scale);

    // 2) Full-range marks: a white-key divider at each white key's left edge, and
    //    a short black bar for each black key.
    const float bw = std::max(1.0f, 1.0f * t.scale);   // divider width
    const float kw = std::max(1.5f, 2.2f * t.scale);   // black-mark width
    for (int m = kRulerLo; m <= kRulerHi; ++m) {
        const float x = vx(midi_to_x(m, x0, x1));
        if (key_is_black(m)) {
            canvas.set_fill_color(blk);
            canvas.fill_rect(x - kw * 0.5f, vy(kStripY), kw, kStripH * t.scale * 0.55f);
        } else {
            canvas.set_fill_color(dark);
            canvas.fill_rect(x, vy(kStripY), bw, kStripH * t.scale);
        }
    }

    // 3) C-only labels along the bottom of the strip.
    const float lf = std::max(6.0f, 7.0f * t.scale);
    canvas.set_font("Inter", lf);
    canvas.set_fill_color(canvas::Color::rgba(0x16/255.f, 0x19/255.f, 0x1E/255.f, 0.55f));  // C-label ink  token-lint:allow
    for (int m = kRulerLo; m <= kRulerHi; m += 12) {   // every C
        canvas.fill_text(c_label(m), vx(midi_to_x(m, x0, x1)) + 1.5f, vy(kStripY + kStripH) - 1.5f);
    }

    // 3b) Piano tab: dynamic C-only key labels that FOLLOW the window — draw "Cn"
    //     under each piano white key whose windowed MIDI is a C (the baked
    //     C2/C3/C4 are suppressed at load). So sliding the selection relabels the
    //     keyboard (C-2 reads C-2), and the partial top shows C6/C7/C8.
    if (active_frame() == kPianoFrame) {
        canvas.set_font("Inter", std::max(7.0f, 9.0f * t.scale));
        canvas.set_fill_color(canvas::Color::rgba8(0x50, 0x59, 0x64));  // baked label ink  token-lint:allow
        for (int i = 0; i < element_count(); ++i) {
            if (element_kind(i) != DesignFrameElement::Kind::momentary || element_note(i) < 0) continue;
            const int midi = midi_for_element(i);
            if (midi % 12 != 0) continue;   // C keys only
            const Rect r = element_rect(i);
            canvas.fill_text("C" + std::to_string(midi / 12 - 2),
                             vx(r.x + 13.0f), vy(r.y + r.height - 11.0f));
        }
    }

    // 4) Teal highlight over the active frame's visible window: typing = the
    //    ~1.5-octave play span; piano = the 3-octave key window (wider). 15% fill
    //    + hairline border, matching the design's box.
    int hlo, hhi;
    if (active_frame() == kTypingFrame) {
        hlo = controller_.base_note() + controller_.octave_shift() * 12;
        hhi = hlo + kPlaySpan;
    } else {
        hlo = piano_window_lo();
        hhi = hlo + kPianoSpan;
    }
    // midi_to_x(m) is the LEFT edge of key m, so the window [hlo, hhi] is fully
    // enclosed only when the right edge sits at the left edge of the key AFTER the
    // last one (hhi+1). Without the +1 the highlight stopped at the last key's left
    // edge — visibly ~half a key short of the piano window's right edge. Clamped to
    // the ruler max so the top window (…hhi=127=G8) still ends exactly at the strip.
    const float hx0 = vx(midi_to_x(std::clamp(hlo, kRulerLo, kRulerHi), x0, x1));
    const float hx1 = vx(midi_to_x(std::clamp(hhi + 1, kRulerLo, kRulerHi), x0, x1));
    const float r = 3.0f * t.scale;
    const auto teal = resolve_color("accent.primary", canvas::Color::rgba8(22, 218, 194));
    canvas.set_fill_color(canvas::Color::rgba(teal.r, teal.g, teal.b,        // token-lint:allow (derived from resolved accent)
                                              dragging_strip_ ? 0.30f : 0.16f));
    canvas.fill_rounded_rect(hx0, vy(kStripY), hx1 - hx0, kStripH * t.scale, r);
    canvas.set_stroke_color(canvas::Color::rgba(teal.r, teal.g, teal.b, 1.0f));  // token-lint:allow (derived from resolved accent)
    canvas.set_line_width(std::max(1.0f, 1.5f * t.scale));
    canvas.stroke_rounded_rect(hx0, vy(kStripY), hx1 - hx0, kStripH * t.scale, r);
}

void MusicalTypingKeyboard::on_mouse_down(Point pos) {
    float px;
    if (point_over_strip(pos, px)) {
        dragging_strip_ = true;
        set_cursor(CursorStyle::grabbing);
        controller_.set_octave_shift(octave_for_strip_x(px));
        update_readouts();
        request_repaint();
        return;   // a strip grab is not a key/toggle click
    }
    DesignFrameView::on_mouse_down(pos);
}

void MusicalTypingKeyboard::on_mouse_drag(Point pos) {
    if (dragging_strip_) {
        float px;
        if (!point_over_strip(pos, px)) {
            // Dragged off the band — still map by x so the octave keeps tracking.
            const auto t = panel_transform(local_bounds());
            px = t.scale > 0.0f ? (pos.x - t.ox) / t.scale : 0.0f;
        }
        controller_.set_octave_shift(octave_for_strip_x(px));
        update_readouts();
        request_repaint();
        return;
    }
    DesignFrameView::on_mouse_drag(pos);
}

void MusicalTypingKeyboard::on_mouse_up(Point pos) {
    if (dragging_strip_) {
        dragging_strip_ = false;
        float px;
        set_cursor(point_over_strip(pos, px) ? CursorStyle::grab : CursorStyle::default_);
        request_repaint();
        return;
    }
    DesignFrameView::on_mouse_up(pos);
}

void MusicalTypingKeyboard::on_hover_move(Point pos) {
    float px;
    if (point_over_strip(pos, px))
        set_cursor(dragging_strip_ ? CursorStyle::grabbing : CursorStyle::grab);
    else if (!dragging_strip_)
        set_cursor(CursorStyle::default_);
    DesignFrameView::on_hover_move(pos);
}

void MusicalTypingKeyboard::on_mouse_leave() {
    if (!dragging_strip_) set_cursor(CursorStyle::default_);
    DesignFrameView::on_mouse_leave();
}

}  // namespace pulp::view
