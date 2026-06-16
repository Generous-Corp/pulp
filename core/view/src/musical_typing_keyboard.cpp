#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/runtime/base64.hpp>

#include <string>
#include <vector>

namespace pulp::view {

namespace detail {
const char* musical_typing_svg_b64();  // defined in musical_typing_keyboard_svg.cpp
}

namespace {
// Decode the embedded faithful SVG once. Host-side construction; never the
// audio/render thread.
std::string decode_embedded_svg() {
    if (auto bytes = runtime::base64_decode(detail::musical_typing_svg_b64()))
        return std::string(bytes->begin(), bytes->end());
    return {};
}

// Typing-view playable keys as momentary elements. Hit-rects are in panel
// (Figma node 187:2-local = SVG) coords, extracted from the Figma source; `note`
// is the relative semitone (0..17) in the Logic-style "a w s e d f t g y h u j k
// o l p ; '" row. view_group 0 = typing view. Black keys are narrower/shorter,
// so DesignFrameView's smallest-area hit tiebreak picks them over the white key
// they overlap. (Piano-view keys, view_group 1, are a follow-up.)
std::vector<DesignFrameElement> build_typing_keys() {
    struct K { int note; float x, y, w, h; };
    static const K keys[] = {
        {0, 164, 233, 53, 79}, {1, 204, 233, 30, 54}, {2, 219, 233, 53, 79},
        {3, 259, 233, 30, 54}, {4, 274, 233, 53, 79}, {5, 329, 233, 53, 79},
        {6, 369, 233, 30, 54}, {7, 384, 233, 53, 79}, {8, 424, 233, 30, 54},
        {9, 439, 233, 53, 79}, {10, 478, 233, 30, 54}, {11, 494, 233, 53, 79},
        {12, 549, 233, 53, 79}, {13, 588, 233, 30, 54}, {14, 604, 233, 53, 79},
        {15, 643, 233, 30, 54}, {16, 659, 233, 53, 79}, {17, 714, 233, 53, 79},
    };
    std::vector<DesignFrameElement> els;
    els.reserve(sizeof(keys) / sizeof(keys[0]));
    for (const auto& k : keys) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.note = k.note;
        e.view_group = 0;
        e.x = k.x; e.y = k.y; e.w = k.w; e.h = k.h;
        els.push_back(e);
    }
    return els;
}

// The lower piano keyboard's playable keys. `note` is the ABSOLUTE MIDI number
// (C2=48 … B4=83, three chromatic octaves), per the contract's piano-key
// convention — consumers distinguish typing (relative semitone 0..17) from
// piano (>=48) by magnitude. Rects are the path bounding boxes extracted from
// the same Figma frame; black keys are narrower/shorter so the smallest-area
// hit tiebreak picks them over the white key they overlap. view_group 0 too:
// this faithful frame shows BOTH keyboards at once, so both are always playable
// (the view_group toggle is for single-keyboard consumers, not this frame).
std::vector<DesignFrameElement> build_piano_keys() {
    struct K { int note; float x, y, w, h; };
    static const K keys[] = {
        {48, 90, 456, 30, 79}, {49, 112, 450, 22, 58}, {50, 123, 456, 30, 79},
        {51, 144, 450, 22, 58}, {52, 155, 456, 30, 79}, {53, 187, 456, 30, 79},
        {54, 208, 450, 22, 58}, {55, 219, 456, 30, 79}, {56, 240, 450, 22, 58},
        {57, 251, 456, 30, 79}, {58, 272, 450, 22, 58}, {59, 284, 456, 30, 79},
        {60, 316, 456, 30, 79}, {61, 336, 450, 22, 58}, {62, 348, 456, 30, 79},
        {63, 368, 450, 22, 58}, {64, 380, 456, 30, 79}, {65, 412, 456, 30, 79},
        {66, 433, 450, 22, 58}, {67, 445, 456, 30, 79}, {68, 466, 450, 22, 58},
        {69, 477, 456, 30, 79}, {70, 498, 450, 22, 58}, {71, 509, 456, 30, 79},
        {72, 541, 456, 30, 79}, {73, 562, 450, 22, 58}, {74, 573, 456, 30, 79},
        {75, 594, 450, 22, 58}, {76, 606, 456, 30, 79}, {77, 638, 456, 30, 79},
        {78, 658, 450, 22, 58}, {79, 670, 456, 30, 79}, {80, 690, 450, 22, 58},
        {81, 702, 456, 30, 79}, {82, 722, 450, 22, 58}, {83, 734, 456, 30, 79},
    };
    std::vector<DesignFrameElement> els;
    els.reserve(sizeof(keys) / sizeof(keys[0]));
    for (const auto& k : keys) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::momentary;
        e.note = k.note;
        e.view_group = 0;
        e.x = k.x; e.y = k.y; e.w = k.w; e.h = k.h;
        els.push_back(e);
    }
    return els;
}

// Typing row first (indices 0..17, relative semitones), then the piano keyboard
// (absolute MIDI). Both groups render in the faithful frame at once.
std::vector<DesignFrameElement> build_keys() {
    auto els = build_typing_keys();
    auto piano = build_piano_keys();
    els.insert(els.end(), piano.begin(), piano.end());
    return els;
}
}  // namespace

MusicalTypingKeyboard::MusicalTypingKeyboard()
    : DesignFrameView(decode_embedded_svg(), build_keys()) {
    set_active_view_group(0);       // both keyboards playable
    set_focusable(true);            // accept computer-keyboard focus for typing
    controller_.set_base_note(48);  // 'a' = C2, matching the design's OCTAVE C2

    // Computer keyboard → notes (controller) forwarded to the public note sink.
    controller_.on_note_on  = [this](int note, float vel) { if (on_note_on) on_note_on(note, vel); };
    controller_.on_note_off = [this](int note)            { if (on_note_off) on_note_off(note); };

    // Clicking a key (DesignFrameView momentary gesture) plays its note too:
    // typing keys follow base+octave, piano keys are absolute MIDI.
    on_gesture_begin = [this](int i) {
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_on) on_note_on(n, controller_.velocity);
    };
    on_gesture_end = [this](int i) {
        const int n = midi_for_element(i);
        if (n >= 0 && on_note_off) on_note_off(n);
    };
}

bool MusicalTypingKeyboard::on_key_event(const KeyEvent& event) {
    const bool consumed = controller_.handle_key(event);  // QWERTY→note + z/x octave
    // Light the matching typing key (by relative semitone, octave-independent).
    const int semi = MusicalTypingController::semitone_for_key(event.key);
    if (semi >= 0) {
        if (event.is_down && !event.is_repeat) light_typing_semitone(semi, true);
        else if (!event.is_down)               light_typing_semitone(semi, false);
    }
    return consumed;
}

void MusicalTypingKeyboard::on_focus_changed(bool gained) {
    DesignFrameView::on_focus_changed(gained);
    if (!gained) {  // release held notes + clear typing highlights so none stick
        controller_.all_notes_off();
        for (int i = 0; i < element_count(); ++i)
            if (element_kind(i) == DesignFrameElement::Kind::momentary && element_note(i) < 24)
                set_element_value(i, 0.0f);
        request_repaint();
    }
}

int MusicalTypingKeyboard::typing_element_for_semitone(int semitone) const {
    if (semitone < 0 || semitone >= 24) return -1;  // typing relative-semitone range
    for (int i = 0; i < element_count(); ++i)
        if (element_kind(i) == DesignFrameElement::Kind::momentary && element_note(i) == semitone)
            return i;
    return -1;
}

int MusicalTypingKeyboard::midi_for_element(int index) const {
    const int note = element_note(index);
    if (note < 0) return -1;
    if (note < 24)  // typing: relative semitone → base note + octave shift
        return controller_.base_note() + controller_.octave_shift() * 12 + note;
    return note;    // piano: absolute MIDI
}

void MusicalTypingKeyboard::light_typing_semitone(int semitone, bool on) {
    const int i = typing_element_for_semitone(semitone);
    if (i >= 0) { set_element_value(i, on ? 1.0f : 0.0f); request_repaint(); }
}

}  // namespace pulp::view
