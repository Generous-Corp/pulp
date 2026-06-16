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
        {0, 166, 233, 50, 78}, {1, 203, 233, 30, 54}, {2, 219, 233, 50, 78},
        {3, 256, 233, 30, 54}, {4, 272, 233, 50, 78}, {5, 325, 233, 50, 78},
        {6, 362, 233, 30, 54}, {7, 379, 233, 50, 78}, {8, 416, 233, 30, 54},
        {9, 432, 233, 50, 78}, {10, 469, 233, 30, 54}, {11, 485, 233, 50, 78},
        {12, 538, 233, 50, 78}, {13, 575, 233, 30, 54}, {14, 592, 233, 50, 78},
        {15, 629, 233, 30, 54}, {16, 645, 233, 50, 78}, {17, 698, 233, 50, 78},
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
    set_active_view_group(0);  // both keyboards playable
}

}  // namespace pulp::view
