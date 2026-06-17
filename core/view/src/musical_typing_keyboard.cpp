#include <pulp/view/musical_typing_keyboard.hpp>
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
    auto add = [&](std::string id, float x, float y, float w, float h) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::action;
        e.action = std::move(id);
        e.x = x; e.y = y; e.w = w; e.h = h;
        els.push_back(e);
    };
    add("octave_down", 114, 213, 32, 32);
    add("octave_up",   155, 213, 32, 32);
    // The < > buttons flanking the top overview strip also step the octave.
    add("octave_down", 117, 23, 22, 24);
    add("octave_up",   550, 23, 22, 24);
    add("vel_down",    321, 213, 32, 32);
    add("vel_up",      362, 213, 32, 32);

    // Pitch bend (buttons 1,2) + sustain are MOMENTARY (press/release, lit while
    // held) → modelled as momentary elements with an action tag so the existing
    // key-overlay lights them. Modulation (buttons 3–8) is a LATCHED selector,
    // also momentary-tagged but re-lit on release to persist the selection.
    auto add_mom = [&](std::string id, float x, float y, float w, float h) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.action = std::move(id);   // control tag (routes away from notes)
        e.x = x; e.y = y; e.w = w; e.h = h;
        els.push_back(e);
    };
    add_mom("sustain", 21, 110, 66, 92);
    add_mom("pb_down", 108, 63, 36, 38);   // "−" / key 1
    add_mom("pb_up",   150, 63, 36, 38);   // "+" / key 2
    // Modulation 3..8 ("off" … "max"), keys 3-8 → mod_0 … mod_5.
    static const float mx[] = {200, 242, 284, 326, 368, 410};
    for (int i = 0; i < 6; ++i)
        add_mom("mod_" + std::to_string(i), mx[i], 63, 36, 38);
}

// Live value readouts (Kind::value_label) over the design's baked OCTAVE / VEL /
// PITCH BEND numbers. The `action` field tags which value each shows so
// update_readouts() can refresh them. Rects are the baked glyph boxes (build
// suppresses the frozen glyphs there). `who` = "typing" (full set) or "piano"
// (octave only — the piano toolbar shows just OCTAVE).
void append_readouts(std::vector<DesignFrameElement>& els, const char* who) {
    auto add = [&](std::string tag, float x, float y, float w, float h) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::value_label;
        e.action = std::move(tag);   // reused as the readout id
        e.x = x; e.y = y; e.w = w; e.h = h;
        els.push_back(e);
    };
    if (std::string(who) == "typing") {
        add("octave", 635, 26, 16, 19);   // top "OCTAVE C2"
        add("octave", 75, 219, 17, 21);   // bottom "OCTAVE C2"
        add("velocity", 695, 26, 16, 19); // top "VEL 98"
        add("velocity", 282, 219, 17, 21);// bottom "VELOCITY 98"
        add("pitchbend", 90, 74, 8, 18);  // "PITCH BEND 0"
    } else {  // piano
        add("octave", 695, 26, 16, 19);   // "OCTAVE C2"
    }
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
        {0, 102, 117, 55, 78}, {1, 142, 117, 30, 54}, {2, 157, 117, 55, 78},
        {3, 197, 117, 30, 54}, {4, 212, 117, 55, 78}, {5, 267, 117, 55, 78},
        {6, 307, 117, 30, 54}, {7, 322, 117, 55, 78}, {8, 362, 117, 30, 54},
        {9, 377, 117, 55, 78}, {10, 416, 117, 30, 54}, {11, 431, 117, 55, 78},
        {12, 486, 117, 55, 78}, {13, 526, 117, 30, 54}, {14, 541, 117, 55, 78},
        {15, 581, 117, 30, 54}, {16, 596, 117, 55, 78}, {17, 651, 117, 55, 78},
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
std::vector<DesignFrameElement> build_piano_frame() {
    struct K { int note; float x, y, w, h; };
    static const K keys[] = {
        {48, 28, 70, 32, 78}, {49, 50, 64, 22, 58}, {50, 60, 70, 32, 78},
        {51, 82, 64, 22, 58}, {52, 92, 70, 32, 78}, {53, 125, 70, 32, 78},
        {54, 146, 64, 22, 58}, {55, 157, 70, 32, 78}, {56, 178, 64, 22, 58},
        {57, 189, 70, 32, 78}, {58, 210, 64, 22, 58}, {59, 221, 70, 32, 78},
        {60, 253, 70, 32, 78}, {61, 274, 64, 22, 58}, {62, 286, 70, 32, 78},
        {63, 306, 64, 22, 58}, {64, 318, 70, 32, 78}, {65, 350, 70, 32, 78},
        {66, 371, 64, 22, 58}, {67, 382, 70, 32, 78}, {68, 404, 64, 22, 58},
        {69, 414, 70, 32, 78}, {70, 436, 64, 22, 58}, {71, 446, 70, 32, 78},
        {72, 479, 70, 32, 78}, {73, 500, 64, 22, 58}, {74, 511, 70, 32, 78},
        {75, 532, 64, 22, 58}, {76, 543, 70, 32, 78}, {77, 575, 70, 32, 78},
        {78, 596, 64, 22, 58}, {79, 607, 70, 32, 78}, {80, 628, 64, 22, 58},
        {81, 640, 70, 32, 78}, {82, 660, 64, 22, 58}, {83, 672, 70, 32, 78},
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
        DesignFrameElement e; e.kind = DesignFrameElement::Kind::action;
        e.action = std::move(id); e.x = x; e.y = 23; e.w = 22; e.h = 24;
        els.push_back(e);
    };
    add_oct("octave_down", 117);
    add_oct("octave_up", 610);
    append_readouts(els, "piano");   // live OCTAVE value (piano toolbar)
    return els;
}
}  // namespace

MusicalTypingKeyboard::MusicalTypingKeyboard()
    // Frame 0 = typing mode (732×266). Pass the panel explicitly = the whole
    // frame so the standalone export shows edge-to-edge (no detect_panel guess).
    : DesignFrameView(decode(detail::musical_typing_typing_svg_b64()),
                      build_typing_frame(), 0, 0, 732, 266) {
    // Frame 1 = piano mode (732×176).
    add_frame(decode(detail::musical_typing_piano_svg_b64()),
              build_piano_frame(), 0, 0, 732, 176);

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

    // On-screen command buttons (Kind::action) → controller state. octave/velocity
    // drive the SAME controller the z/x·c/v keys do (so the next note reflects
    // them). Pitch-bend, modulation, and sustain are momentary elements (handled
    // by the gesture path above), NOT actions.
    on_action = [this](const std::string& a) {
        if (a == "octave_down")     controller_.set_octave_shift(controller_.octave_shift() - 1);
        else if (a == "octave_up")  controller_.set_octave_shift(controller_.octave_shift() + 1);
        else if (a == "vel_down")   controller_.velocity = std::clamp(controller_.velocity - kVelStep, 0.0f, 1.0f);
        else if (a == "vel_up")     controller_.velocity = std::clamp(controller_.velocity + kVelStep, 0.0f, 1.0f);
        update_readouts();   // reflect the new octave / velocity value
    };

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

void MusicalTypingKeyboard::control_press(const std::string& tag) {
    const int e = element_for_action(tag);
    if (tag == "pb_down") {
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
    if (tag == "pb_down" || tag == "pb_up") {
        pb_value_ = 0;                              // momentary: spring back to centre
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
    const bool consumed = controller_.handle_key(event);  // QWERTY→note + z/x octave
    // Light the matching typing key (by relative semitone, octave-independent).
    // No-op in piano mode (the frame has no typing keys), which is correct.
    const int semi = MusicalTypingController::semitone_for_key(event.key);
    if (semi >= 0) {
        if (event.is_down && !event.is_repeat) light_typing_semitone(semi, true);
        else if (!event.is_down)               light_typing_semitone(semi, false);
    }
    // z/x (octave) and c/v (velocity) change controller state the readouts show.
    if (consumed && semi < 0) update_readouts();
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
    return note;    // piano: absolute MIDI
}

void MusicalTypingKeyboard::light_typing_semitone(int semitone, bool on) {
    const int i = typing_element_for_semitone(semitone);
    if (i >= 0) { set_element_value(i, on ? 1.0f : 0.0f); request_repaint(); }
}

}  // namespace pulp::view
