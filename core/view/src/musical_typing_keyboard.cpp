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
}  // namespace

MusicalTypingKeyboard::MusicalTypingKeyboard()
    : DesignFrameView(decode_embedded_svg(), build_typing_keys()) {
    set_active_view_group(0);  // typing view playable by default
}

}  // namespace pulp::view
