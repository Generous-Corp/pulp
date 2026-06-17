#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
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

// The typing frame's on-screen command controls (action elements), in frame-0
// (732×266) coords from Figma node 187:15: octave −/+ and velocity −/+ (the
// bottom Z/X · C/V buttons), the sustain pad, and the 8 pitch-bend preset
// buttons. Clicking one fires on_action(id); MusicalTypingKeyboard maps the id
// to the controller (octave/velocity) or a callback (sustain/pitch-bend).
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
    add("vel_down",    321, 213, 32, 32);
    add("vel_up",      362, 213, 32, 32);
    add("sustain",      21, 110, 66, 92);
    // 8 pitch-bend preset buttons, left→right (the design's "− + off … max" row).
    static const float pbx[] = {108, 150, 200, 242, 284, 326, 368, 410};
    for (int i = 0; i < 8; ++i)
        add("pb_" + std::to_string(i), pbx[i], 63, 36, 38);
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

    // Clicking a key (DesignFrameView momentary gesture) plays its note too:
    // typing keys follow base+octave, piano keys are absolute MIDI. (Swap-link
    // toggle clicks are handled inside DesignFrameView and never reach here.)
    on_gesture_begin = [this](int i) {
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_on) on_note_on(n, controller_.velocity);
    };
    on_gesture_end = [this](int i) {
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_off) on_note_off(n);
    };

    // On-screen command buttons (Kind::action) → controller state or a callback.
    // octave/velocity drive the SAME controller the z/x·c/v keys do (so the next
    // note reflects them); sustain + pitch-bend are surfaced as callbacks the
    // host wires. (Modulation has no baked control in this design — the label is
    // a placeholder — so on_modulation is defined for the contract but unwired.)
    on_action = [this](const std::string& a) {
        if (a == "octave_down")     controller_.set_octave_shift(controller_.octave_shift() - 1);
        else if (a == "octave_up")  controller_.set_octave_shift(controller_.octave_shift() + 1);
        else if (a == "vel_down")   controller_.velocity = std::clamp(controller_.velocity - kVelStep, 0.0f, 1.0f);
        else if (a == "vel_up")     controller_.velocity = std::clamp(controller_.velocity + kVelStep, 0.0f, 1.0f);
        else if (a == "sustain") {  sustain_ = !sustain_; if (on_sustain) on_sustain(sustain_); }
        else if (a.rfind("pb_", 0) == 0) {
            // 8 preset buttons, left→right → a normalized 0..1 bend; the host maps
            // it to its own bend range.
            const int i = std::clamp(std::atoi(a.c_str() + 3), 0, 7);
            if (on_pitch_bend) on_pitch_bend(static_cast<float>(i) / 7.0f);
        }
    };
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
}

void MusicalTypingKeyboard::set_input_capture(bool capture) {
    input_capture_ = capture;
    set_focusable(capture);          // don't steal first-responder when host feeds keys
    if (!capture) controller_.all_notes_off();  // stop our own QWERTY-held notes
}

bool MusicalTypingKeyboard::on_key_event(const KeyEvent& event) {
    if (!input_capture_) return false;  // host feeds QWERTY itself — don't double-trigger
    const bool consumed = controller_.handle_key(event);  // QWERTY→note + z/x octave
    // Light the matching typing key (by relative semitone, octave-independent).
    // No-op in piano mode (the frame has no typing keys), which is correct.
    const int semi = MusicalTypingController::semitone_for_key(event.key);
    if (semi >= 0) {
        if (event.is_down && !event.is_repeat) light_typing_semitone(semi, true);
        else if (!event.is_down)               light_typing_semitone(semi, false);
    }
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
